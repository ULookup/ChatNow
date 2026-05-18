#pragma once

#include "infra/etcd.hpp"     // 服务注册模块封装
#include "infra/logger.hpp"   // 日志模块封装
#include "auth/auth_context.hpp"
#include "auth/forward_auth.hpp"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "mq/rabbitmq.hpp"
#include "mq/channel.hpp"
#include "mq/trace_headers.hpp"
#include "utils/utils.hpp"
#include "infra/snowflake.hpp"
#include "dao/data_redis.hpp"
#include "utils/worker_id.hpp"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "identity/identity_service.pb.h"
#include "conversation/conversation_service.pb.h"
#include "message/message_types.pb.h"
#include "message/message_service.pb.h"
#include "message/message_internal.pb.h"
#include "transmite/transmite_service.pb.h"
#include <brpc/server.h>
#include <butil/logging.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace chatnow
{

// 大群门限：>= LARGE_GROUP_THRESHOLD 时启用读扩散，仅写 message 主表
inline constexpr size_t LARGE_GROUP_THRESHOLD = 200;

class TransmiteServiceImpl : public chatnow::transmite::MsgTransmitService
{
public:
    TransmiteServiceImpl(const std::string &identity_service_name,
                        const std::string &conversation_service_name,
                        const std::string &message_service_name,
                        const ServiceManager::ptr &channels,
                        const std::string &exchange_name,
                        const std::string &routing_key,
                        const Publisher::ptr &publisher,
                        const std::shared_ptr<SnowflakeId> &id_generator,
                        const SeqGen::ptr &seq_gen,
                        const Members::ptr &members_cache,
                        const RateLimiter::ptr &rate_limiter,
                        const std::shared_ptr<sw::redis::Redis> &redis)
                        : _identity_service_name(identity_service_name),
                        _conversation_service_name(conversation_service_name),
                        _message_service_name(message_service_name),
                        _mm_channels(channels),
                        _exchange_name(exchange_name),
                        _routing_key(routing_key),
                        _publisher(publisher),
                        _id_generator(id_generator),
                        _seq_gen(seq_gen),
                        _members_cache(members_cache),
                        _rate_limiter(rate_limiter),
                        _redis(redis) {}
    ~TransmiteServiceImpl() = default;

    void SendMessage(google::protobuf::RpcController *controller,
                const ::chatnow::transmite::SendMessageReq *request,
                ::chatnow::transmite::SendMessageRsp *response,
                ::google::protobuf::Closure *done)
    {
        brpc::ClosureGuard rpc_guard(done);

        auto err_response = [response](const std::string &rid, int32_t code, const std::string &msg) {
            response->mutable_header()->set_request_id(rid);
            response->mutable_header()->set_success(false);
            response->mutable_header()->set_error_code(code);
            response->mutable_header()->set_error_message(msg);
        };

        // ① 从 metadata 提取 user_id（替代旧 request->user_id()）
        chatnow::auth::AuthContext auth;
        try {
            auth = chatnow::auth::extract_auth(static_cast<brpc::Controller*>(controller));
        } catch (const chatnow::ServiceError &e) {
            err_response(request->request_id(), e.code(), e.what());
            return;
        }
        std::string uid = auth.user_id;
        std::string rid = request->request_id();
        std::string chat_ssid = request->conversation_id();
        const std::string &client_msg_id = request->client_msg_id();

        // ② 文件消息必须前置上传（body 仅带 file_id）
        const auto &content = request->content();
        switch (content.type()) {
            case chatnow::message::MessageType::IMAGE:
                if (content.image().file_id().empty()) {
                    LOG_ERROR("请求ID: {} - IMAGE 消息缺少 file_id（应客户端前置上传）", rid);
                    return err_response(rid, chatnow::error::kSystemInvalidArgument, "请先上传图片再发送");
                }
                break;
            case chatnow::message::MessageType::FILE:
                if (content.file().file_id().empty()) {
                    LOG_ERROR("请求ID: {} - FILE 消息缺少 file_id（应客户端前置上传）", rid);
                    return err_response(rid, chatnow::error::kSystemInvalidArgument, "请先上传文件再发送");
                }
                break;
            case chatnow::message::MessageType::AUDIO:
                if (content.audio().file_id().empty()) {
                    LOG_ERROR("请求ID: {} - AUDIO 消息缺少 file_id", rid);
                    return err_response(rid, chatnow::error::kSystemInvalidArgument, "请先上传语音再发送");
                }
                break;
            default:
                break;
        }

        // ③ 客户端幂等去重：Redis SET NX，命中直接返回（零 RPC）
        if (!client_msg_id.empty() && _redis) {
            std::string idem_key = "im:msg:idem:" + uid + ":" + client_msg_id;
            try {
                auto result = _redis->set(idem_key, "pending", std::chrono::seconds(86400), sw::redis::UpdateType::SET_NX);
                if (!result) {
                    // key 已存在 → 重复消息
                    auto cached = _redis->get(idem_key);
                    if (cached && cached.value() != "pending") {
                        LOG_INFO("请求ID: {} - 命中幂等 client_msg_id={} 直接返回旧消息", rid, client_msg_id);
                        response->mutable_header()->set_request_id(rid);
                        response->mutable_header()->set_success(true);
                        response->mutable_message()->set_message_id(
                            std::stoull(cached.value()));
                        return;
                    }
                    LOG_WARN("请求ID: {} - client_msg_id={} 幂等冲突（前一条未完成）", rid, client_msg_id);
                    return err_response(rid, chatnow::error::kSystemUnavailable, "duplicate request in flight");
                }
            } catch (std::exception &e) {
                LOG_WARN("请求ID: {} - Redis 幂等检查异常: {}（fail-open）", rid, e.what());
            }
        }

        // ④ 限流检查（用户级 + 会话级）
        if (_rate_limiter) {
            if (!_rate_limiter->allow_user(uid, 600, 60)) {
                LOG_WARN("请求ID: {} - 用户级限流命中 uid={}", rid, uid);
                return err_response(rid, chatnow::error::kSystemUnavailable, "rate_limited");
            }
            if (!_rate_limiter->allow_session(chat_ssid, 3000, 60)) {
                LOG_WARN("请求ID: {} - 会话级限流命中 ssid={}", rid, chat_ssid);
                return err_response(rid, chatnow::error::kSystemUnavailable, "rate_limited");
            }
        }

        // ⑤ 并行 RPC：Identity.GetProfile（sender 信息）+ Conversation.GetMemberIds（收件人列表）
        auto identity_channel = _mm_channels->choose(_identity_service_name);
        if (!identity_channel) {
            LOG_ERROR("请求ID: {} - identity_service 节点缺失", rid);
            return err_response(rid, chatnow::error::kSystemUnavailable, "依赖服务暂不可用");
        }

        // 成员列表：先查 Redis 缓存，未命中再 RPC + 回填
        std::vector<std::string> member_id_list;
        bool members_from_cache = false;
        if (_members_cache) {
            member_id_list = _members_cache->list(chat_ssid);
            if (!member_id_list.empty()) members_from_cache = true;
        }

        chatnow::identity::IdentityService_Stub identity_stub(identity_channel.get());
        chatnow::identity::GetProfileReq profile_req;
        chatnow::identity::GetProfileRsp profile_rsp;
        brpc::Controller profile_cntl;
        profile_req.set_request_id(rid);
        profile_req.set_user_id(uid);
        chatnow::auth::forward_auth_metadata(static_cast<brpc::Controller*>(controller), &profile_cntl);
        identity_stub.GetProfile(&profile_cntl, &profile_req, &profile_rsp, brpc::DoNothing());

        // 成员列表未命中缓存：RPC 拉取
        ::chatnow::conversation::GetMemberIdsRsp member_rsp;
        brpc::Controller member_cntl;
        if (!members_from_cache) {
            auto conv_channel = _mm_channels->choose(_conversation_service_name);
            if (!conv_channel) {
                brpc::Join(profile_cntl.call_id());
                LOG_ERROR("请求ID: {} - conversation_service 节点缺失", rid);
                return err_response(rid, chatnow::error::kSystemUnavailable, "依赖服务暂不可用");
            }
            ::chatnow::conversation::ConversationService_Stub conv_stub(conv_channel.get());
            ::chatnow::conversation::GetMemberIdsReq member_req;
            member_req.set_request_id(rid);
            member_req.set_conversation_id(chat_ssid);
            chatnow::auth::forward_auth_metadata(static_cast<brpc::Controller*>(controller), &member_cntl);
            conv_stub.GetMemberIds(&member_cntl, &member_req, &member_rsp, brpc::DoNothing());
            brpc::Join(member_cntl.call_id());
            if (member_cntl.Failed() || !member_rsp.header().success()) {
                brpc::Join(profile_cntl.call_id());
                LOG_ERROR("请求ID: {} - 获取群成员失败: {} {}", rid,
                          member_cntl.ErrorText(), member_rsp.header().error_message());
                return err_response(rid, chatnow::error::kSystemUnavailable, "获取群成员失败");
            }
            for (const auto &m : member_rsp.member_ids()) member_id_list.push_back(m);
            if (_members_cache) _members_cache->warm(chat_ssid, member_id_list);
        }

        brpc::Join(profile_cntl.call_id());
        if (profile_cntl.Failed() || !profile_rsp.header().success()) {
            LOG_ERROR("请求ID: {} - 获取用户信息失败: {}", rid, profile_cntl.ErrorText());
            return err_response(rid, chatnow::error::kSystemUnavailable, "获取用户信息失败");
        }
        if (member_id_list.empty()) {
            LOG_ERROR("请求ID: {} - 会话成员为空 ssid={}", rid, chat_ssid);
            return err_response(rid, chatnow::error::kConversationNotFound, "会话已解散或不存在");
        }

        // ⑥ sender 成员校验（sendMessage 语义要求发送者必须是群成员）
        bool sender_is_member = false;
        for (const auto &m : member_id_list) {
            if (m == uid) { sender_is_member = true; break; }
        }
        if (!sender_is_member) {
            LOG_ERROR("请求ID: {} - sender {} 不在会话 {} 中", rid, uid, chat_ssid);
            return err_response(rid, chatnow::error::kConversationNotMember, "无发消息权限");
        }

        // ⑦ 申请 session_seq（Redis INCR）
        unsigned long session_seq = _seq_gen->next_session_seq(chat_ssid);
        if (session_seq == 0) {
            LOG_ERROR("请求ID: {} - 申请 session_seq 失败 ssid={}", rid, chat_ssid);
            return err_response(rid, chatnow::error::kSystemUnavailable, "序号生成失败");
        }

        // ⑧ 组装 InternalMessage
        chatnow::message::internal::InternalMessage internal_msg;
        chatnow::message::Message *msg = internal_msg.mutable_message();

        msg->set_message_id(_id_generator->Next());
        msg->set_conversation_id(chat_ssid);
        msg->set_created_at_ms(static_cast<int64_t>(time(nullptr)) * 1000);
        msg->mutable_content()->CopyFrom(request->content());
        msg->set_sender_id(uid);
        msg->set_seq_id(session_seq);
        msg->set_client_msg_id(client_msg_id);
        if (request->has_reply_to()) msg->mutable_reply_to()->CopyFrom(request->reply_to());
        for (const auto &muid : request->mentioned_user_ids()) msg->add_mentioned_user_ids(muid);
        if (request->has_forward_info()) msg->mutable_forward_info()->CopyFrom(request->forward_info());

        // 成员列表
        size_t member_count = member_id_list.size();
        bool is_large = member_count >= LARGE_GROUP_THRESHOLD;
        internal_msg.set_is_large_group(is_large);
        for (const auto &member_id : member_id_list) {
            internal_msg.add_member_id_list(member_id);
        }

        // 写扩散群：批量申请 user_seq（pipeline 一次往返）；大群跳过
        if (!is_large) {
            auto user_seqs = _seq_gen->next_user_seq_batch(member_id_list);
            if (user_seqs.size() != member_id_list.size()) {
                LOG_ERROR("请求ID: {} - 批量申请 user_seq 失败", rid);
                return err_response(rid, chatnow::error::kSystemUnavailable, "用户序号生成失败");
            }
            for (size_t i = 0; i < member_id_list.size(); ++i) {
                auto *pair = internal_msg.add_user_seqs();
                pair->set_user_id(member_id_list[i]);
                pair->set_user_seq(user_seqs[i]);
            }
        }

        // ⑨ 组装响应（仅 message 不暴露 target_id_list）
        response->mutable_header()->set_request_id(rid);
        response->mutable_header()->set_success(true);
        response->mutable_message()->CopyFrom(*msg);

        // ⑩ MQ publish_confirm（异步回调完成后 Run done）
        google::protobuf::Closure *async_done = rpc_guard.release();
        auto msg_id = msg->message_id();
        std::string idem_key;
        if (!client_msg_id.empty() && _redis) {
            idem_key = "im:msg:idem:" + uid + ":" + client_msg_id;
        }
        auto redis = _redis;  // 捕获 shared_ptr 延长生命周期

        std::shared_ptr<std::atomic<bool>> done_called =
            std::make_shared<std::atomic<bool>>(false);
        try {
            std::map<std::string, std::string> _mq_headers;
            ::chatnow::mq::mq_inject_trace_headers(_mq_headers);
            _publisher->publish_confirm(internal_msg.SerializeAsString(),
                _mq_headers,
                [async_done, response, rid, done_called, redis, idem_key, msg_id](PublishStatus status, const std::string &mq_msg) {
                    if (done_called->exchange(true)) return;
                    if (status == PublishStatus::Acked) {
                        LOG_DEBUG("请求ID: {} - 消息成功投递到 Broker", rid);
                        // 更新幂等 key：pending → 真实 msg_id
                        if (!idem_key.empty() && redis) {
                            try { redis->set(idem_key, std::to_string(msg_id), std::chrono::seconds(86400)); }
                            catch (...) {}
                        }
                    } else {
                        LOG_ERROR("请求ID: {} - 消息投递到 Broker 失败: {}", rid, mq_msg);
                        // 投递失败清除幂等 key，允许客户端重试
                        if (!idem_key.empty() && redis) {
                            try { redis->del(idem_key); } catch (...) {}
                        }
                        response->mutable_header()->set_success(false);
                        response->mutable_header()->set_error_code(chatnow::error::kSystemUnavailable);
                        response->mutable_header()->set_error_message("消息发送失败，请重试");
                        response->clear_message();
                    }
                    async_done->Run();
                });
        } catch (std::exception &e) {
            LOG_ERROR("请求ID: {} - publish_confirm 同步异常: {}", rid, e.what());
            if (!done_called->exchange(true)) {
                response->mutable_header()->set_success(false);
                response->mutable_header()->set_error_code(chatnow::error::kSystemUnavailable);
                response->mutable_header()->set_error_message("MQ 不可用");
                response->clear_message();
                async_done->Run();
            }
        }
    }
private:
    std::string _identity_service_name;
    std::string _conversation_service_name;
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
    std::shared_ptr<sw::redis::Redis> _redis;
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
                            const std::string &identity_service_name,
                            const std::string &conversation_service_name,
                            const std::string &message_service_name)
    {
        _identity_service_name = identity_service_name;
        _conversation_service_name = conversation_service_name;
        _message_service_name = message_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_identity_service_name);
        _mm_channels->declared(_conversation_service_name);
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
        TransmiteServiceImpl *transmite_service = new TransmiteServiceImpl(_identity_service_name,
                                                                        _conversation_service_name,
                                                                        _message_service_name,
                                                                        _mm_channels,
                                                                        _exchange_name,
                                                                        _routing_key,
                                                                        _publisher,
                                                                        _id_generator,
                                                                        _seq_gen,
                                                                        _members_cache,
                                                                        _rate_limiter,
                                                                        _redis);
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
    std::string _identity_service_name;
    std::string _conversation_service_name;
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
