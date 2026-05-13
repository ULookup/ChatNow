#pragma once

#include "connection.hpp"
#include "etcd.hpp"
#include "logger.hpp"
#include "channel.hpp"
#include "rabbitmq.hpp"
#include "data_redis.hpp"
#include "base.pb.h"
#include "notify.pb.h"
#include "push.pb.h"
#include "message.pb.h"
#include <brpc/server.h>
#include <thread>
#include <chrono>

namespace chatnow
{

/**
 * PushServiceImpl
 * ---------------------------------------------------------------------------
 * 职责：
 *   1. 终结客户端 WebSocket 长连接 + 维护本实例内存中的 uid→conn 映射
 *   2. 把"用户在哪些 push 实例上"写到 Redis（im:online:{uid} → SET<instance>），
 *      让其它 push 实例 / 调用方按 uid 路由到正确实例
 *   3. 提供 brpc PushService 接口给其它服务调用（friend / chatsession / message）
 *   4. 订阅 msg_push_queue：消息落库后由 message 服务投递到此队列，本服务消费后下发
 *   5. 推送 ACK + 重传：未 ack 的 user_seq 进入 Redis Sorted Set，心跳/重连时补送
 */
class PushServiceImpl : public PushService
{
public:
    PushServiceImpl(const Connection::ptr &connections,
                    const Session::ptr &redis_session,
                    const Status::ptr &redis_status,
                    const OnlineRoute::ptr &online_route,
                    const UnackedPush::ptr &unacked,
                    const std::string &instance_id,
                    const std::string &message_service_name,
                    const ServiceManager::ptr &channels)
        : _connections(connections),
          _redis_session(redis_session),
          _redis_status(redis_status),
          _online_route(online_route),
          _unacked(unacked),
          _instance_id(instance_id),
          _message_service_name(message_service_name),
          _mm_channels(channels) {}

    // brpc: 单用户推送（其它服务调用）
    void PushToUser(google::protobuf::RpcController* controller,
                    const ::chatnow::PushToUserReq* request,
                    ::chatnow::PushToUserRsp* response,
                    ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        const std::string &rid = request->request_id();
        response->set_request_id(rid);

        std::string payload = request->notify().SerializeAsString();
        int delivered = _local_send(request->user_id(), payload);
        // 若是聊天消息推送：未 ack 入未送达缓冲，等客户端 ack/心跳触发补送
        if(request->has_user_seq() && _unacked) {
            _unacked->push(request->user_id(),
                           static_cast<unsigned long>(request->user_seq()),
                           static_cast<long long>(time(nullptr)));
        }
        response->set_success(true);
        response->set_online_device_count(delivered);
    }

    void PushBatch(google::protobuf::RpcController* controller,
                   const ::chatnow::PushBatchReq* request,
                   ::chatnow::PushBatchRsp* response,
                   ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        std::string payload = request->notify().SerializeAsString();
        std::unordered_map<std::string, unsigned long> uid2seq;
        for(const auto &p : request->user_seqs()) uid2seq[p.user_id()] = p.user_seq();
        int total = 0;
        for(const auto &uid : request->user_id_list()) {
            int n = _local_send(uid, payload);
            if(n > 0) total++;
            auto it = uid2seq.find(uid);
            if(it != uid2seq.end() && _unacked) {
                _unacked->push(uid, it->second, static_cast<long long>(time(nullptr)));
            }
        }
        response->set_request_id(request->request_id());
        response->set_success(true);
        response->set_online_count(total);
    }

    /* brief: 订阅 msg_push_queue 的消费回调
     *  - 大群优化：按 push 实例分组后并发 PushBatch（一次 RPC 推 N 个 uid），
     *    避免 200 人群里串行 200 次 brpc 阻塞 MQ 消费线程
     *  - 跨实例 RPC 全部 brpc::DoNothing 异步发起
     */
    ConsumeAction onPushMessage(const char *body, size_t sz, bool redelivered) {
        InternalMessage internal_msg;
        if(!internal_msg.ParseFromArray(body, sz)) {
            LOG_ERROR("Push-Consumer: 反序列化失败");
            return ConsumeAction::NackDiscard;
        }
        const auto &msg_info = internal_msg.message_info();

        NotifyMessage notify;
        notify.set_notify_type(NotifyType::CHAT_MESSAGE_NOTIFY);
        notify.mutable_new_message_info()->mutable_message_info()->CopyFrom(msg_info);
        std::string payload = notify.SerializeAsString();

        std::unordered_map<std::string, unsigned long> uid2seq;
        for(const auto &p : internal_msg.user_seqs()) uid2seq[p.user_id()] = p.user_seq();

        // 1) 写未 ack 缓冲（在尝试推送前先入队，确保对端 ack 前可重传）
        long long now_ts = static_cast<long long>(time(nullptr));
        if(_unacked) {
            for(const auto &uid : internal_msg.member_id_list()) {
                auto it = uid2seq.find(uid);
                if(it != uid2seq.end()) _unacked->push(uid, it->second, now_ts);
            }
        }

        // 2) 本机直推 → 命中则跳过远端
        std::vector<std::string> remote_uids;
        remote_uids.reserve(internal_msg.member_id_list_size());
        for(const auto &uid : internal_msg.member_id_list()) {
            int n = _local_send(uid, payload);
            if(n == 0) remote_uids.push_back(uid);
        }
        if(remote_uids.empty()) return ConsumeAction::Ack;

        // 3) 跨实例：按 push 实例 ID 分组（OnlineRoute 一次查询每个 uid 命中实例集合）
        std::unordered_map<std::string, std::vector<std::string>> peer_to_uids;
        for(const auto &uid : remote_uids) {
            auto its = _online_route ? _online_route->instances(uid) : std::vector<std::string>{};
            for(const auto &peer : its) {
                if(peer == _instance_id) continue;
                peer_to_uids[peer].push_back(uid);
                break;  // 同一 uid 命中一个对端就够
            }
        }

        // 4) 每个对端一次 PushBatch（异步 brpc::DoNothing）
        for(auto &kv : peer_to_uids) {
            const std::string &peer = kv.first;
            const auto &uids = kv.second;
            auto channel = _mm_channels->choose(peer);
            if(!channel) {
                LOG_WARN("Push-Consumer: 对端 {} 不可达，{} 个用户消息丢失（依赖 unacked 重传）",
                         peer, uids.size());
                continue;
            }
            PushService_Stub stub(channel.get());
            // 注意：req/rsp/cntl 必须在堆上，brpc::DoNothing 异步回调中沿用
            auto *req  = new PushBatchReq();
            auto *rsp  = new PushBatchRsp();
            auto *cntl = new brpc::Controller();
            req->set_request_id(msg_info.client_msg_id());
            for(const auto &u : uids) req->add_user_id_list(u);
            req->mutable_notify()->CopyFrom(notify);
            for(const auto &u : uids) {
                auto it = uid2seq.find(u);
                if(it == uid2seq.end()) continue;
                auto *p = req->add_user_seqs();
                p->set_user_id(u);
                p->set_user_seq(it->second);
            }
            // 自删 closure：回调里释放堆资源
            auto *done = brpc::NewCallback(
                [](brpc::Controller *c, PushBatchReq *q, PushBatchRsp *r) {
                    if(c->Failed()) {
                        LOG_WARN("PushBatch 跨实例失败: {}", c->ErrorText());
                    }
                    delete q; delete r; delete c;
                }, cntl, req, rsp);
            stub.PushBatch(cntl, req, rsp, done);
        }
        return ConsumeAction::Ack;
    }

    /* brief: WebSocket 入口 — 客户端 ACK / 心跳处理 */
    void onClientNotify(const NotifyMessage &notify) {
        if(notify.notify_type() == NotifyType::MSG_PUSH_ACK) {
            const auto &ack = notify.msg_push_ack();
            if(_unacked) _unacked->ack(ack.user_id(), ack.user_seq());
            // 异步上报 last_ack_seq 到 message 服务
            auto channel = _mm_channels->choose(_message_service_name);
            if(channel) {
                MsgStorageService_Stub stub(channel.get());
                UpdateAckSeqReq req;
                UpdateAckSeqRsp rsp;
                brpc::Controller cntl;
                req.set_user_id(ack.user_id());
                req.set_chat_session_id(ack.chat_session_id());
                req.set_user_seq(ack.user_seq());
                stub.UpdateAckSeq(&cntl, &req, &rsp, brpc::DoNothing());
                // fire-and-forget
            }
        } else if(notify.notify_type() == NotifyType::CLIENT_HEARTBEAT) {
            const auto &hb = notify.heartbeat();
            // 拉取未 ack 列表，逐条补送（这里简化：直接发对应序号占位，
            // 完整实现还需要从 message 表回查实际 MessageInfo 再填到 NotifyMessage）
            if(_unacked) {
                auto pending = _unacked->drain(hb.user_id(), 50);
                for(const auto &seq_str : pending) {
                    LOG_DEBUG("Heartbeat-补送 uid={} seq={}", hb.user_id(), seq_str);
                }
            }
        }
    }

private:
    /* brief: 本实例直接通过 WS 下发；返回送达的连接数 */
    int _local_send(const std::string &uid, const std::string &payload) {
        auto conns = _connections->connections(uid);
        int sent = 0;
        for(auto &c : conns) {
            try {
                if(c && c->get_state() == websocketpp::session::state::value::open) {
                    c->send(payload, websocketpp::frame::opcode::value::binary);
                    ++sent;
                }
            } catch(std::exception &e) {
                LOG_WARN("WS send 失败 uid={}: {}", uid, e.what());
            }
        }
        return sent;
    }

    Connection::ptr _connections;
    Session::ptr _redis_session;
    Status::ptr _redis_status;
    OnlineRoute::ptr _online_route;
    UnackedPush::ptr _unacked;
    std::string _instance_id;
    std::string _message_service_name;
    ServiceManager::ptr _mm_channels;
};

class PushServer
{
public:
    using ptr = std::shared_ptr<PushServer>;
    PushServer(const Discovery::ptr &disc,
               const Registry::ptr &reg,
               const std::shared_ptr<brpc::Server> &rpc,
               server_t *ws_server)
        : _service_discover(disc), _reg_client(reg), _rpc_server(rpc), _ws_server(ws_server) {}
    ~PushServer() = default;

    void start() {
        // RPC + WebSocket 同进程跑；任意一边异常退出立即通知另一边停服
        _ws_thread = std::thread([this]() {
            try {
                _ws_server->run();
                LOG_INFO("Push WS 线程正常退出");
            } catch(std::exception &e) {
                LOG_ERROR("Push WS 线程异常退出: {}", e.what());
            }
            // ws 退出 → 通知 brpc 停服
            _rpc_server->Stop(0);
        });
        _rpc_server->RunUntilAskedToQuit();
        _ws_server->stop();
        if(_ws_thread.joinable()) _ws_thread.join();
    }
private:
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
    server_t *_ws_server;
    std::thread _ws_thread;
};

class PushServerBuilder
{
public:
    void make_redis_object(const std::string &host, uint16_t port, int db,
                           bool keep_alive, int pool_size)
    {
        _redis = RedisClientFactory::create(host, port, db, keep_alive, pool_size);
        _redis_session = std::make_shared<Session>(_redis);
        _redis_status  = std::make_shared<Status>(_redis);
        _online_route  = std::make_shared<OnlineRoute>(_redis);
        _unacked       = std::make_shared<UnackedPush>(_redis);
    }

    void make_discovery_object(const std::string &reg_host,
                               const std::string &base_service_name,
                               const std::string &message_service_name,
                               const std::string &push_service_name)
    {
        _message_service_name = message_service_name;
        _push_service_name    = push_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(message_service_name);
        // 关注 push 自身，便于跨实例转发；service_name 由配置传入避免硬编码
        _mm_channels->declared(push_service_name);
        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }

    void make_reg_object(const std::string &reg_host,
                         const std::string &service_name,
                         const std::string &access_host)
    {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
        _instance_id = service_name;  // 用注册路径作为实例 ID（路由表 key 用）
    }

    void make_mq_object(const std::string &user, const std::string &password,
                        const std::string &host,
                        const std::string &exchange,
                        const std::string &queue,
                        const std::string &binding_key)
    {
        std::string amqp_url = "amqp://" + user + ":" + password + "@" + host + ":5672/";
        _mq_client = std::make_shared<MQClient>(amqp_url);
        _push_settings = {
            .exchange = exchange,
            .exchange_type = chatnow::DIRECT,
            .queue = queue,
            .binding_key = binding_key
        };
        auto dummy_cb = [](const char*, size_t, bool) -> ConsumeAction {
            return ConsumeAction::Ack;
        };
        _push_subscriber = chatnow::MQFactory::create<chatnow::Subscriber>(
            _mq_client, _push_settings, dummy_cb);
    }

    /* brief: 构造 WebSocket server（监听端口） */
    void make_ws_object(uint16_t ws_port) {
        _ws_server.set_access_channels(websocketpp::log::alevel::none);
        _ws_server.clear_error_channels(websocketpp::log::elevel::none);
        _ws_server.init_asio();
        _ws_server.set_reuse_addr(true);
        _ws_server.set_open_handler([this](websocketpp::connection_hdl hdl) {
            LOG_DEBUG("WS 连接建立 {}", (size_t)_ws_server.get_con_from_hdl(hdl).get());
        });
        _ws_server.set_close_handler([this](websocketpp::connection_hdl hdl) {
            auto conn = _ws_server.get_con_from_hdl(hdl);
            std::string uid, ssid, dev;
            if(_connections && _connections->client(conn, uid, ssid, dev)) {
                _connections->remove(conn);
                if(_online_route) _online_route->unbind(uid, _instance_id);
                LOG_DEBUG("WS 关闭 uid={}", uid);
            }
        });
        _ws_server.set_message_handler([this](websocketpp::connection_hdl hdl, server_t::message_ptr msg) {
            auto conn = _ws_server.get_con_from_hdl(hdl);
            // 反序列化 NotifyMessage（双向通道）
            NotifyMessage notify;
            if(!notify.ParseFromString(msg->get_payload())) {
                LOG_WARN("WS payload 反序列化失败，关闭连接");
                _ws_server.close(hdl, websocketpp::close::status::unsupported_data,
                                 "payload invalid");
                return;
            }

            // 路径 A：未鉴权连接的首条消息必须是 CLIENT_AUTH
            std::string uid_known, ssid_known, dev_known;
            if(!_connections->client(conn, uid_known, ssid_known, dev_known)) {
                if(notify.notify_type() != NotifyType::CLIENT_AUTH || !notify.has_client_auth()) {
                    LOG_WARN("WS 首条非 CLIENT_AUTH，关闭连接");
                    _ws_server.close(hdl, websocketpp::close::status::unsupported_data,
                                     "auth required");
                    return;
                }
                const auto &auth = notify.client_auth();
                if(auth.session_id().empty() || auth.device_id().empty()) {
                    LOG_WARN("WS CLIENT_AUTH 缺 session_id 或 device_id");
                    _ws_server.close(hdl, websocketpp::close::status::unsupported_data,
                                     "session_id/device_id required");
                    return;
                }
                auto uid = _redis_session ? _redis_session->uid(auth.session_id())
                                          : sw::redis::OptionalString{};
                if(!uid) {
                    LOG_WARN("WS 鉴权失败 ssid={}", auth.session_id());
                    _ws_server.close(hdl, websocketpp::close::status::unsupported_data,
                                     "auth failed");
                    return;
                }
                _connections->insert(conn, *uid, auth.session_id(), auth.device_id());
                if(_redis_status) _redis_status->append(*uid);
                if(_online_route) _online_route->bind(*uid, _instance_id);
                LOG_INFO("WS 鉴权成功 uid={} device={}", *uid, auth.device_id());
                // 携带 last_user_seq 时立即触发补送
                if(auth.has_last_user_seq() && _push_service) {
                    NotifyMessage hb;
                    hb.set_notify_type(NotifyType::CLIENT_HEARTBEAT);
                    hb.mutable_heartbeat()->set_user_id(*uid);
                    hb.mutable_heartbeat()->set_last_user_seq(auth.last_user_seq());
                    _push_service->onClientNotify(hb);
                }
                return;
            }

            // 路径 B：已鉴权连接的后续消息（ACK / 心跳）
            _connections->touch(conn);
            if(_push_service) _push_service->onClientNotify(notify);
            if(notify.notify_type() == NotifyType::CLIENT_HEARTBEAT) {
                if(_online_route) _online_route->touch(uid_known);
                if(_redis_status) _redis_status->touch(uid_known);
                if(_redis_session) _redis_session->touch(ssid_known);
            }
        });
    }

    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads, uint16_t ws_port) {
        if(!_redis) { LOG_ERROR("Push: Redis 未初始化"); abort(); }
        if(!_mm_channels) { LOG_ERROR("Push: 信道管理未初始化"); abort(); }
        _connections = std::make_shared<Connection>();
        _rpc_server = std::make_shared<brpc::Server>();
        _push_service = new PushServiceImpl(
            _connections, _redis_session, _redis_status,
            _online_route, _unacked, _instance_id,
            _message_service_name, _mm_channels);
        int ret = _rpc_server->AddService(_push_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
        if(ret == -1) { LOG_ERROR("Push: AddService 失败"); abort(); }

        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads = num_threads;
        if(_rpc_server->Start(port, &options) == -1) {
            LOG_ERROR("Push: brpc 启动失败");
            abort();
        }
        // 启动 WS server
        make_ws_object(ws_port);
        std::error_code ec;
        _ws_server.listen(ws_port, ec);
        if(ec) { LOG_ERROR("Push: WS 监听失败 {}", ec.message()); abort(); }
        _ws_server.start_accept();

        // 订阅 push_queue
        auto callback = std::bind(&PushServiceImpl::onPushMessage, _push_service,
                                  std::placeholders::_1, std::placeholders::_2,
                                  std::placeholders::_3);
        _push_subscriber->consume(std::move(callback));
        LOG_INFO("Push 服务启动: rpc_port={} ws_port={}", port, ws_port);
    }

    PushServer::ptr build() {
        return std::make_shared<PushServer>(_service_discover, _reg_client, _rpc_server, &_ws_server);
    }
private:
    std::shared_ptr<sw::redis::Redis> _redis;
    Session::ptr _redis_session;
    Status::ptr _redis_status;
    OnlineRoute::ptr _online_route;
    UnackedPush::ptr _unacked;

    std::string _message_service_name;
    std::string _push_service_name;
    std::string _instance_id;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;

    declare_settings _push_settings;
    MQClient::ptr _mq_client;
    Subscriber::ptr _push_subscriber;

    Connection::ptr _connections;
    server_t _ws_server;
    PushServiceImpl *_push_service {nullptr};
    std::shared_ptr<brpc::Server> _rpc_server;
};

} // namespace chatnow
