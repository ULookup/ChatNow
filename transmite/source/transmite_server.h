#pragma once

#include "infra/etcd.hpp"     // 服务注册模块封装
#include "infra/logger.hpp"   // 日志模块封装
#include "mq/rabbitmq.hpp"
#include "mq/channel.hpp"
#include "utils/utils.hpp"
#include "infra/snowflake.hpp"
#include "dao/data_redis.hpp"
#include "utils/worker_id.hpp"
#include "base.pb.h"
#include "user.pb.h"
#include "chatsession.pb.h"
#include "message.pb.h"
#include "transmite.pb.h"  // protobuf框架代码
#include <brpc/server.h>
#include <butil/logging.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace chatnow
{

// 大群门限：>= LARGE_GROUP_THRESHOLD 时启用读扩散，仅写 message 主表
inline constexpr size_t LARGE_GROUP_THRESHOLD = 200;

class TransmiteServiceImpl : public chatnow::MsgTransmitService
{
public:
    TransmiteServiceImpl(const std::string &user_service_name,
                        const std::string &chatsession_service_name,
                        const std::string &message_service_name,
                        const ServiceManager::ptr &channels,
                        const std::string &exchange_name,
                        const std::string &routing_key,
                        const Publisher::ptr &publisher,
                        const std::shared_ptr<SnowflakeId> &id_generator,
                        const SeqGen::ptr &seq_gen,
                        const Members::ptr &members_cache,
                        const RateLimiter::ptr &rate_limiter)
                        : _user_service_name(user_service_name),
                        _chatsession_service_name(chatsession_service_name),
                        _message_service_name(message_service_name),
                        _mm_channels(channels),
                        _exchange_name(exchange_name),
                        _routing_key(routing_key),
                        _publisher(publisher),
                        _id_generator(id_generator),
                        _seq_gen(seq_gen),
                        _members_cache(members_cache),
                        _rate_limiter(rate_limiter) {}
    ~TransmiteServiceImpl() = default;

    void GetTransmitTarget(google::protobuf::RpcController *controller,
                        const ::chatnow::NewMessageReq *request,
                        ::chatnow::GetTransmitTargetRsp *response,
                        ::google::protobuf::Closure *done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
        };
        // 从请求中获取：用户ID，所属会话ID，消息内容
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        std::string chat_ssid = request->chat_session_id();
        const std::string &client_msg_id = request->client_msg_id();

        // ============ 步骤 0: 入参合法性校验（文件类型必须前置上传，body 仅带 file_id） ============
        const auto &content = request->message();
        switch(content.message_type()) {
            case MessageType::IMAGE:
                if(content.image_message().file_id().empty()) {
                    LOG_ERROR("请求ID: {} - IMAGE 消息缺少 file_id（应客户端前置上传）", rid);
                    return err_response(rid, "请先上传图片再发送");
                }
                break;
            case MessageType::FILE:
                if(content.file_message().file_id().empty()) {
                    LOG_ERROR("请求ID: {} - FILE 消息缺少 file_id（应客户端前置上传）", rid);
                    return err_response(rid, "请先上传文件再发送");
                }
                break;
            case MessageType::SPEECH:
                if(content.speech_message().file_id().empty()) {
                    LOG_ERROR("请求ID: {} - SPEECH 消息缺少 file_id", rid);
                    return err_response(rid, "请先上传语音再发送");
                }
                break;
            default: break;
        }

        // ============ 步骤 1: 客户端幂等去重 ============
        // 命中 client_msg_id 索引则直接返回旧消息，绕过雪花/seq/MQ
        if(!client_msg_id.empty()) {
            auto msg_channel = _mm_channels->choose(_message_service_name);
            if(msg_channel) {
                MsgStorageService_Stub stub(msg_channel.get());
                SelectByClientMsgReq dup_req;
                SelectByClientMsgRsp dup_rsp;
                brpc::Controller dup_cntl;
                dup_req.set_request_id(rid);
                dup_req.set_user_id(uid);
                dup_req.set_client_msg_id(client_msg_id);
                stub.SelectByClientMsg(&dup_cntl, &dup_req, &dup_rsp, nullptr);
                if(!dup_cntl.Failed() && dup_rsp.success() && dup_rsp.exists()) {
                    LOG_INFO("请求ID: {} - 命中幂等 client_msg_id={} 直接返回旧消息", rid, client_msg_id);
                    response->set_request_id(rid);
                    response->set_success(true);
                    response->mutable_message()->CopyFrom(dup_rsp.message());
                    // 幂等返回不带 target_id_list（避免重复推送）
                    return;
                }
            } else {
                LOG_WARN("请求ID: {} - message_service 不可用，跳过幂等检查", rid);
            }
        }

        // ============ 步骤 1.5: 限流（用户级 + 会话级） ============
        // 实现：基于 INCR + EXPIRE 的固定窗口计数器（简化版）。
        //   - 阈值语义为"每窗口内最多 N 次"（窗口=60s）
        //   - 已知缺陷：相邻窗口边界可能放行 ≈2N，对反爬场景不严格；
        //     生产环境推荐 redis-cell（CL.THROTTLE）或 Lua 令牌桶替换 RateLimiter::allow
        if(_rate_limiter) {
            // 用户级：60s 内 600 次（≈10 QPS 平均；爆发可在窗口内集中）
            if(!_rate_limiter->allow_user(uid, /*max_count=*/600, /*window_sec=*/60)) {
                LOG_WARN("请求ID: {} - 用户级限流命中 uid={}", rid, uid);
                return err_response(rid, "rate_limited");
            }
            // 会话级：60s 内 3000 次（≈50 QPS）
            if(!_rate_limiter->allow_session(chat_ssid, /*max_count=*/3000, /*window_sec=*/60)) {
                LOG_WARN("请求ID: {} - 会话级限流命中 ssid={}", rid, chat_ssid);
                return err_response(rid, "rate_limited");
            }
        }

        // ============ 步骤 2: 并行 RPC 拿用户信息 + 群成员列表（成员优先走缓存） ============
        auto user_channel = _mm_channels->choose(_user_service_name);
        if(!user_channel) {
            LOG_ERROR("请求ID: {} - user_service 节点缺失", rid);
            return err_response(rid, "依赖服务节点缺失");
        }

        // 成员列表：先查 Redis 缓存，未命中再 RPC + 回填
        std::vector<std::string> member_id_list;
        bool members_from_cache = false;
        if(_members_cache) {
            member_id_list = _members_cache->list(chat_ssid);
            if(!member_id_list.empty()) members_from_cache = true;
        }

        UserService_Stub user_stub(user_channel.get());
        GetUserInfoReq user_req;
        GetUserInfoRsp user_rsp;
        brpc::Controller user_cntl;
        user_req.set_request_id(rid);
        user_req.set_user_id(uid);

        // 用户信息 RPC 异步发起
        user_stub.GetUserInfo(&user_cntl, &user_req, &user_rsp, brpc::DoNothing());

        // 成员列表未命中缓存：RPC 拉取
        GetMemberIdListRsp session_rsp;
        brpc::Controller session_cntl;
        if(!members_from_cache) {
            auto session_channel = _mm_channels->choose(_chatsession_service_name);
            if(!session_channel) {
                brpc::Join(user_cntl.call_id());
                LOG_ERROR("请求ID: {} - chatsession_service 节点缺失", rid);
                return err_response(rid, "依赖服务节点缺失");
            }
            ChatSessionService_Stub session_stub(session_channel.get());
            GetMemberIdListReq session_req;
            session_req.set_request_id(rid);
            session_req.set_chat_session_id(chat_ssid);
            session_stub.GetMemberIdList(&session_cntl, &session_req, &session_rsp, brpc::DoNothing());
            brpc::Join(session_cntl.call_id());
            if(session_cntl.Failed() || !session_rsp.success()) {
                brpc::Join(user_cntl.call_id());
                LOG_ERROR("请求ID: {} - 获取群成员失败: {}", rid, session_cntl.ErrorText());
                return err_response(rid, "获取群成员失败");
            }
            for(const auto &m : session_rsp.member_id_list()) member_id_list.push_back(m);
            // 回填缓存
            if(_members_cache) _members_cache->warm(chat_ssid, member_id_list);
        }

        brpc::Join(user_cntl.call_id());

        if(user_cntl.Failed() || !user_rsp.success()) {
            LOG_ERROR("请求ID: {} - 获取用户信息失败: {}", rid, user_cntl.ErrorText());
            return err_response(rid, "获取用户信息失败");
        }
        if(member_id_list.empty()) {
            LOG_ERROR("请求ID: {} - 会话成员为空 ssid={}", rid, chat_ssid);
            return err_response(rid, "会话成员为空");
        }

        // ============ 步骤 3: 申请 message_id (Snowflake) + session_seq (Redis INCR) ============
        unsigned long session_seq = _seq_gen->next_session_seq(chat_ssid);
        if(session_seq == 0) {
            LOG_ERROR("请求ID: {} - 申请 session_seq 失败 ssid={}", rid, chat_ssid);
            return err_response(rid, "序号生成失败");
        }

        // ============ 步骤 4: 组装 InternalMessage ============
        InternalMessage internal_msg;
        MessageInfo* msg_info = internal_msg.mutable_message_info();

        msg_info->set_message_id(_id_generator->Next());
        msg_info->set_chat_session_id(chat_ssid);
        msg_info->set_timestamp(time(nullptr));
        msg_info->mutable_message()->CopyFrom(request->message());
        msg_info->mutable_sender()->CopyFrom(user_rsp.user_info());
        msg_info->set_seq_id(session_seq);
        msg_info->set_client_msg_id(client_msg_id);

        // 成员列表
        size_t member_count = member_id_list.size();
        bool is_large = member_count >= LARGE_GROUP_THRESHOLD;
        internal_msg.set_is_large_group(is_large);
        for (const auto& member_id : member_id_list) {
            internal_msg.add_member_id_list(member_id);
        }

        // 写扩散群：批量申请 user_seq（pipeline 一次往返）；大群跳过
        if(!is_large) {
            auto user_seqs = _seq_gen->next_user_seq_batch(member_id_list);
            if(user_seqs.size() != member_id_list.size()) {
                LOG_ERROR("请求ID: {} - 批量申请 user_seq 失败", rid);
                return err_response(rid, "用户序号生成失败");
            }
            for(size_t i = 0; i < member_id_list.size(); ++i) {
                auto *pair = internal_msg.add_user_seqs();
                pair->set_user_id(member_id_list[i]);
                pair->set_user_seq(user_seqs[i]);
            }
        }

        // ============ 步骤 5: 组装响应 ============
        response->set_request_id(rid);
        response->set_message_id(msg_info->message_id());
        response->set_seq_id(session_seq);
        response->mutable_message()->CopyFrom(*msg_info);
        for(const auto& member_id : member_id_list) {
            response->add_target_id_list(member_id);
        }

        // 解除 rpc_guard 对 done 的管理权，等 MQ 投递完再 Run
        google::protobuf::Closure* async_done = rpc_guard.release();

        // ============ 步骤 6: 投递 MQ ============
        // 兜底：publish_confirm 同步抛异常 / 提交失败 → 必须保证 done 被调用一次，
        //       否则 brpc 会泄漏请求并卡到超时。flag + try/catch 双保险。
        std::shared_ptr<std::atomic<bool>> done_called =
            std::make_shared<std::atomic<bool>>(false);
        try {
            _publisher->publish_confirm(internal_msg.SerializeAsString(),
                [async_done, response, rid, done_called](PublishStatus status, const std::string& msg) {
                if(done_called->exchange(true)) return;  // 防止重复 Run
                if(status == PublishStatus::Acked) {
                    LOG_DEBUG("请求ID: {} - 消息成功投递到 Broker", rid);
                    response->set_success(true);
                } else {
                    LOG_ERROR("请求ID: {} - 消息投递到 Broker 失败: {}", rid, msg);
                    response->set_success(false);
                    response->clear_message();
                    response->clear_target_id_list();
                    response->set_errmsg("消息投递失败,请重试");
                }
                async_done->Run();
            });
        } catch(std::exception &e) {
            LOG_ERROR("请求ID: {} - publish_confirm 同步异常: {}", rid, e.what());
            if(!done_called->exchange(true)) {
                response->set_success(false);
                response->clear_message();
                response->clear_target_id_list();
                response->set_errmsg("MQ 不可用");
                async_done->Run();
            }
        }
    }
private:
    std::string _user_service_name;
    std::string _chatsession_service_name;
    std::string _message_service_name;
    ServiceManager::ptr _mm_channels;

    // 消息队列客户端句柄
    std::string _exchange_name;
    std::string _routing_key;
    Publisher::ptr _publisher;
    std::shared_ptr<SnowflakeId> _id_generator;
    SeqGen::ptr _seq_gen;
    Members::ptr _members_cache;
    RateLimiter::ptr _rate_limiter;
};

class TransmiteServer
{
public:
    using ptr = std::shared_ptr<TransmiteServer>;

    TransmiteServer(const Discovery::ptr &service_discover,
                const Registry::ptr &reg_client,
                const std::shared_ptr<brpc::Server> &server,
                const WorkerIdAllocator::ptr &worker_allocator = nullptr)
        : _service_discover(service_discover), _reg_client(reg_client), _rpc_server(server),
          _worker_allocator(worker_allocator) {}
    ~TransmiteServer() {
        _watchdog_running.store(false);
        if(_watchdog_thread.joinable()) _watchdog_thread.join();
    }

    /* M4: 搭建RPC服务器，并启动服务器
     *  - 启动 lease_lost watchdog：每 1s 轮询 worker_id 租约状态，
     *    一旦发现租约丢失（其他实例占走了同一 worker_id），主动停服 + abort，
     *    避免 SnowflakeId 用已被别人占据的 worker_id 继续发号产生重号。
     */
    void start() {
        if(_worker_allocator) {
            _watchdog_running.store(true);
            _watchdog_thread = std::thread([this]() {
                while(_watchdog_running.load()) {
                    if(_worker_allocator->lease_lost()) {
                        // brpc Stop(0) 不会打断 in-flight handler；
                        // 在 worker_id 已被别人占走的状态下，每多发一个雪花 ID 都
                        // 必然撞向对端实例，污染 message 主键唯一约束。
                        // → 唯一正确语义：立刻 abort，让所有 bthread 一起死。
                        LOG_ERROR("worker_id 租约丢失，立即 abort 防雪花 ID 重号");
                        try { if(_reg_client) _reg_client->unregister(); } catch(...) {}
                        std::abort();
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            });
        }
        _rpc_server->RunUntilAskedToQuit();
        // 主退出后顺手 stop watchdog（若不是 watchdog 触发退出）
        _watchdog_running.store(false);
        if(_watchdog_thread.joinable()) _watchdog_thread.join();
    }
private:
    Discovery::ptr _service_discover;   // 服务发现客户端
    Registry::ptr _reg_client;          // 服务注册客户端
    std::shared_ptr<brpc::Server> _rpc_server;
    WorkerIdAllocator::ptr _worker_allocator;
    std::atomic<bool> _watchdog_running {false};
    std::thread _watchdog_thread;
};

/* 建造者模式: 将对象真正的构造过程封装，便于后期扩展和调整 */
class TransmiteServerBuilder
{
public:
    /* brief: 构造分布式有序ID生成器（worker_id 优先从 Redis 自动申请，否则用 fallback） */
    void make_id_generator_object(uint64_t fallback_worker_id, uint64_t epoch_ms, bool wait_on_clock_backwards)
    {
        try {
            uint64_t worker_id = fallback_worker_id;
            if(_redis) {
                std::string owner = _instance_owner.empty() ? std::to_string(getpid()) : _instance_owner;
                _worker_allocator = std::make_shared<WorkerIdAllocator>(_redis, "transmite", owner);
                int allocated = _worker_allocator->acquire(static_cast<int>(fallback_worker_id));
                if(allocated >= 0) worker_id = static_cast<uint64_t>(allocated);
            } else {
                LOG_WARN("Redis 未初始化，worker_id 退回配置值 {}", fallback_worker_id);
            }
            _id_generator = std::make_shared<SnowflakeId>(worker_id, epoch_ms, wait_on_clock_backwards);
            LOG_INFO("Snowflake worker_id={} epoch_ms={}", worker_id, epoch_ms);
        } catch(std::exception &e) {
            LOG_ERROR("构造分布式有序ID生成器失败: {}", e.what());
        }
    }
    /* brief: 设置实例标识（host:pid），用于 worker_id 租约识别 */
    void set_instance_owner(const std::string &owner) { _instance_owner = owner; }
    /* brief: 用于构造服务发现&信道管理客户端对象（含 message_service 用于幂等查询） */
    void make_discovery_object(const std::string &reg_host,
                            const std::string &base_service_name,
                            const std::string &user_service_name,
                            const std::string &chatsession_service_name,
                            const std::string &message_service_name)
    {
        _user_service_name = user_service_name;
        _chatsession_service_name = chatsession_service_name;
        _message_service_name = message_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_user_service_name);
        _mm_channels->declared(_chatsession_service_name);
        _mm_channels->declared(_message_service_name);
        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);

        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    /* brief: 用于构造服务注册客户端对象 */
    void make_reg_object(const std::string &reg_host,
                        const std::string &service_name,
                        const std::string &access_host) {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    /* brief: 构造 RabbitMQ 客户端对象
     * 契约：transmite 是 publisher-only，exchange_name 必须与 message 服务的
     *      mq_msg_exchange 完全一致（FANOUT 下 binding 不参与匹配，仅 exchange 决定路由）。
     *      queue_name / binding_key 留空，避免声明孤儿队列。
     */
    void make_mq_object(const std::string &user,
                    const std::string &password,
                    const std::string &host,
                    const std::string &exchange_name,
                    const std::string &queue_name,
                    const std::string &binding_key)
    {
        if(exchange_name.empty()) {
            LOG_ERROR("Transmite MQ exchange 不能为空，必须与 message 服务的 mq_msg_exchange 对齐");
            abort();
        }
        // publisher-only：即使配置写了 queue/binding，也必须清空后再 declare，
        // 否则 transmite 会声明绑到同一 exchange 的孤儿队列（FANOUT 下复制副本却无消费者）。
        std::string queue_for_declare;
        std::string binding_for_declare;
        if(!queue_name.empty() || !binding_key.empty()) {
            LOG_WARN("Transmite 是 publisher-only，忽略配置中的 mq_msg_queue={} / mq_msg_binding_key={}，"
                     "避免声明孤儿队列（请清空配置）",
                     queue_name, binding_key);
        }
        _exchange_name = exchange_name;
        _routing_key.clear();  // publisher-only：FANOUT 路由忽略 routing_key，固定空串
        std::string amqp_url = "amqp://" + user + ":" + password + "@" + host + ":5672/";
        _mq_client = std::make_shared<MQClient>(amqp_url);
        declare_settings settings {
            .exchange = exchange_name,
            .exchange_type = chatnow::FANOUT,
            .queue = queue_for_declare,
            .binding_key = binding_for_declare
        };
        _publisher = std::make_shared<Publisher>(_mq_client, settings);
        LOG_INFO("Transmite MQ 已就绪: exchange={} (FANOUT, publisher-only)", exchange_name);
    }
    /* brief: 构造 Redis 客户端 + SeqGen + Members + RateLimiter */
    void make_redis_object(const std::string &host, uint16_t port, int db,
                          bool keep_alive, int pool_size)
    {
        _redis = RedisClientFactory::create(host, port, db, keep_alive, pool_size);
        _seq_gen = std::make_shared<SeqGen>(_redis);
        _members_cache = std::make_shared<Members>(_redis);
        _rate_limiter = std::make_shared<RateLimiter>(_redis);
    }
    /* brief: 构造RPC服务器对象，并添加服务 */
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        if(!_id_generator) {
            LOG_ERROR("还未初始化分布式ID生成器");
            abort();
        }
        if(!_publisher || !_mq_client) {
            LOG_ERROR("还未初始化消息队列客户端模块");
            abort();
        }
        if(!_mm_channels) {
            LOG_ERROR("还未初始化信道管理模块");
            abort();
        }
        if(!_seq_gen) {
            LOG_ERROR("还未初始化 SeqGen / Redis");
            abort();
        }
        _rpc_server = std::make_shared<brpc::Server>();
        TransmiteServiceImpl *transmite_service = new TransmiteServiceImpl(_user_service_name,
                                                                        _chatsession_service_name,
                                                                        _message_service_name,
                                                                        _mm_channels,
                                                                        _exchange_name,
                                                                        _routing_key,
                                                                        _publisher,
                                                                        _id_generator,
                                                                        _seq_gen,
                                                                        _members_cache,
                                                                        _rate_limiter);
        int ret = _rpc_server->AddService(transmite_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
        if(ret == -1) {
            LOG_ERROR("添加RPC服务失败!");
            abort();
        }

        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads = num_threads;
        ret = _rpc_server->Start(port, &options);
        if(ret == -1) {
            LOG_ERROR("服务启动失败!");
            abort();
        }
    }
    TransmiteServer::ptr build() {
        if(!_service_discover) {
            LOG_ERROR("还未初始化服务发现模块");
            abort();
        }
        if(!_reg_client) {
            LOG_ERROR("还未初始化服务注册模块");
            abort();
        }
        if(!_rpc_server) {
            LOG_ERROR("还未初始化RPC服务器模块");
            abort();
        }

        // M4: 把 worker_allocator 传给 server，让 watchdog 线程在 start() 中轮询 lease_lost
        TransmiteServer::ptr server = std::make_shared<TransmiteServer>(
            _service_discover, _reg_client, _rpc_server, _worker_allocator);
        return server;
    }
private:
    std::string _user_service_name;
    std::string _chatsession_service_name;
    std::string _message_service_name;
    ServiceManager::ptr _mm_channels;

    std::string _exchange_name;
    std::string _routing_key;
    MQClient::ptr _mq_client;
    Publisher::ptr _publisher;
    std::shared_ptr<SnowflakeId> _id_generator;

    std::shared_ptr<sw::redis::Redis> _redis;
    SeqGen::ptr _seq_gen;
    Members::ptr _members_cache;
    RateLimiter::ptr _rate_limiter;
    std::string _instance_owner;
    WorkerIdAllocator::ptr _worker_allocator;

    Discovery::ptr _service_discover;   // 服务发现客户端
    Registry::ptr _reg_client;          // 服务注册客户端
    std::shared_ptr<brpc::Server> _rpc_server;
};

}
