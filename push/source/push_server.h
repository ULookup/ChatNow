#pragma once

#include "connection.hpp"
#include "infra/etcd.hpp"
#include "infra/leader_election.hpp"
#include "infra/logger.hpp"
#include "mq/channel.hpp"
#include "mq/rabbitmq.hpp"
#include "mq/trace_headers.hpp"
#include "log/log_context.hpp"
#include <brpc/server.h>
#include "dao/data_redis.hpp"
#include "auth/auth_context.hpp"
#include "auth/forward_auth.hpp"
#include "common/auth/metadata.pb.h"
#include "auth/auth_config_loader.hpp"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "utils/brpc_closure.hpp"
#include "utils/local_cache.hpp"
#include "utils/inflight.hpp"
#include "utils/random_ttl.hpp"
#include "utils/trace_id.hpp"
#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "presence/presence_service.pb.h"
#include "push/notify.pb.h"
#include "push/push_service.pb.h"
#include "message/message_types.pb.h"
#include "message/message_service.pb.h"
#include "message/message_internal.pb.h"
#include <sw/redis++/redis++.h>
#include "picojson/picojson.h"
#include <openssl/evp.h>
#include <algorithm>
#include <thread>
#include <chrono>
#include <limits>
#include <unordered_set>

namespace chatnow::push {

struct RouteEntry {
    std::vector<std::string> device_ids;
    std::unordered_map<std::string, std::string> device_to_instance;
};

class PushServiceImpl : public PushService
{
public:
    PushServiceImpl(const Connection::ptr &connections,
                    const std::shared_ptr<chatnow::auth::JwtCodec> &jwt_codec,
                    const RedisClient::ptr &redis,
                    const OnlineRoute::ptr &online_route,
                    const UnackedPush::ptr &unacked,
                    const CrossInstanceOutbox::ptr &cross_outbox,
                    const std::string &instance_id,
                    const std::string &message_service_name,
                    const ServiceManager::ptr &channels,
                    LeaderElection::ptr cross_reaper_election = nullptr,
                    LocalCache<RouteEntry>::ptr local_route_cache = nullptr,
                    InflightRegistry::ptr inflight_registry = nullptr)
        : _connections(connections),
          _jwt_codec(jwt_codec),
          _redis(redis),
          _online_route(online_route),
          _unacked(unacked),
          _cross_outbox(cross_outbox),
          _instance_id(instance_id),
          _message_service_name(message_service_name),
          _mm_channels(channels),
          _cross_reaper_election(std::move(cross_reaper_election)),
          _local_route_cache(std::move(local_route_cache)),
          _inflight_registry(std::move(inflight_registry)) {}

    void set_resend_params(long batch, long max_age_sec) {
        _resend_batch = batch;
        _resend_max_age_sec = max_age_sec;
    }
    static constexpr int kPresenceTtlSec = 120;
    ~PushServiceImpl() {
        stop_cross_outbox_reaper();  // joins _cross_reaper_thread before 'this' destroyed
    }

    void PushToUser(google::protobuf::RpcController* base_cntl,
                    const PushToUserReq* request,
                    PushToUserRsp* response,
                    google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        std::unordered_set<std::string> target_dids;
        for (const auto &did : request->target_device_ids()) target_dids.insert(did);
        bool filter_devices = !target_dids.empty();
        try {
            response->mutable_header()->set_success(true);
            response->mutable_header()->set_error_code(::chatnow::error::kOK);
            response->mutable_header()->set_request_id(request->request_id());

            std::string payload;
            const auto &notify = request->notify();
            if (request->has_user_seq() &&
                notify.notify_type() == NotifyType::CHAT_MESSAGE_NOTIFY &&
                notify.has_new_message_info()) {
                NotifyMessage per_user = notify;
                per_user.mutable_new_message_info()->mutable_message_info()
                    ->set_user_seq(request->user_seq());
                payload = per_user.SerializeAsString();
            } else {
                payload = notify.SerializeAsString();
            }

            int delivered = 0;
            auto route = resolve_route(request->user_id());
            for (const auto &did : route.device_ids) {
                if (filter_devices && target_dids.find(did) == target_dids.end()) continue;
                if (_local_send(request->user_id(), did, payload) > 0) ++delivered;
            }

            if (request->has_user_seq() && _unacked) {
                std::string payload_b64 = _utils_base64_encode(payload);
                long long now_ts = static_cast<long long>(time(nullptr));
                for (const auto &did : route.device_ids) {
                    if (filter_devices && target_dids.find(did) == target_dids.end()) continue;
                    _unacked->push(request->user_id(), did,
                                   request->user_seq(), payload_b64, now_ts);
                }
            }

            response->set_online_device_count(delivered);
        } catch (const ::chatnow::ServiceError& e) {
            response->mutable_header()->set_success(false);
            response->mutable_header()->set_error_code(e.code());
            response->mutable_header()->set_error_message(e.message());
            cntl->SetFailed(e.message());
            LOG_WARN("rpc_failed code={} msg={}", e.code(), e.message());
        } catch (const std::exception& e) {
            response->mutable_header()->set_success(false);
            response->mutable_header()->set_error_code(::chatnow::error::kSystemInternalError);
            response->mutable_header()->set_error_message("internal error");
            cntl->SetFailed("internal error");
            LOG_ERROR("rpc_exception what={}", e.what());
        }
    }

    void PushBatch(google::protobuf::RpcController* base_cntl,
                   const PushBatchReq* request,
                   PushBatchRsp* response,
                   google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        std::unordered_map<std::string, unsigned long> uid2seq;
        for (const auto &p : request->user_seqs()) uid2seq[p.user_id()] = p.user_seq();
        try {
            response->mutable_header()->set_success(true);
            response->mutable_header()->set_error_code(::chatnow::error::kOK);
            response->mutable_header()->set_request_id(request->request_id());

            const auto &base_notify = request->notify();
            bool is_chat_msg = (base_notify.notify_type() == NotifyType::CHAT_MESSAGE_NOTIFY) &&
                               base_notify.has_new_message_info();

            int total = 0;
            long long now_ts = static_cast<long long>(time(nullptr));
            for (const auto &uid : request->user_id_list()) {
                auto route = resolve_route(uid);
                auto it = uid2seq.find(uid);

                std::string payload;
                if (is_chat_msg && it != uid2seq.end()) {
                    NotifyMessage per_user = base_notify;
                    per_user.mutable_new_message_info()->mutable_message_info()
                        ->set_user_seq(it->second);
                    payload = per_user.SerializeAsString();
                } else {
                    payload = base_notify.SerializeAsString();
                }

                for (const auto &did : route.device_ids) {
                    if (_local_send(uid, did, payload) > 0) ++total;
                    if (it != uid2seq.end() && _unacked) {
                        _unacked->push(uid, did, it->second,
                                       _utils_base64_encode(payload), now_ts);
                    }
                }
            }
            response->set_online_count(total);
        } catch (const ::chatnow::ServiceError& e) {
            response->mutable_header()->set_success(false);
            response->mutable_header()->set_error_code(e.code());
            response->mutable_header()->set_error_message(e.message());
            cntl->SetFailed(e.message());
            LOG_WARN("rpc_failed code={} msg={}", e.code(), e.message());
        } catch (const std::exception& e) {
            response->mutable_header()->set_success(false);
            response->mutable_header()->set_error_code(::chatnow::error::kSystemInternalError);
            response->mutable_header()->set_error_message("internal error");
            cntl->SetFailed("internal error");
            LOG_ERROR("rpc_exception what={}", e.what());
        }
    }

    ConsumeAction onPushMessage(const char *body, size_t sz, bool redelivered) {
        chatnow::message::internal::InternalMessage internal_msg;
        if (!internal_msg.ParseFromArray(body, sz)) {
            LOG_ERROR("Push-Consumer: 反序列化 InternalMessage 失败");
            return ConsumeAction::NackDiscard;
        }
        const auto &msg_info = internal_msg.message();

        std::unordered_map<std::string, unsigned long> uid2seq;
        for (const auto &p : internal_msg.user_seqs()) uid2seq[p.user_id()] = p.user_seq();

        NotifyMessage notify_template;
        notify_template.set_notify_type(NotifyType::CHAT_MESSAGE_NOTIFY);
        notify_template.mutable_new_message_info()->mutable_message_info()->CopyFrom(msg_info);
        const auto &_ctx_trace = chatnow::log::LogContext::current().trace_id;
        if (!_ctx_trace.empty()) {
            notify_template.set_trace_id(_ctx_trace);
        }

        // 1) 写 unacked + 构建远程 uid 列表
        long long now_ts = static_cast<long long>(time(nullptr));
        std::vector<std::string> remote_uids;
        remote_uids.reserve(internal_msg.member_id_list_size());
        for (const auto &uid : internal_msg.member_id_list()) {
            auto route = resolve_route(uid);
            if (route.device_ids.empty()) { remote_uids.push_back(uid); continue; }

            auto itu = uid2seq.find(uid);

            // Pre-serialize payload per-user instead of per-device
            std::string user_payload;
            if (itu != uid2seq.end()) {
                NotifyMessage per_user = notify_template;
                per_user.mutable_new_message_info()->mutable_message_info()
                    ->set_user_seq(itu->second);
                user_payload = per_user.SerializeAsString();
            }

            bool any_local = false;
            for (const auto &did : route.device_ids) {
                auto it = route.device_to_instance.find(did);
                std::string inst = (it != route.device_to_instance.end()) ? it->second : "";
                if (inst == _instance_id) {
                    if (itu != uid2seq.end()) {
                        if (_local_send(uid, did, user_payload) > 0) any_local = true;
                        if (_unacked) {
                            _unacked->push(uid, did, itu->second,
                                           _utils_base64_encode(user_payload), now_ts);
                        }
                    } else {
                        // 大群读扩散：无 user_seq，仅下发
                        _local_send(uid, did, notify_template.SerializeAsString());
                        any_local = true;
                    }
                }
            }
            if (!any_local) remote_uids.push_back(uid);
        }

        if (remote_uids.empty()) return ConsumeAction::Ack;

        // 2) 跨实例：按 Push 实例 ID 分组
        std::unordered_map<std::string, std::unordered_set<std::string>> peer_to_uids;
        for (const auto &uid : remote_uids) {
            auto route = resolve_route(uid);
            for (const auto &did : route.device_ids) {
                auto it = route.device_to_instance.find(did);
                std::string peer = (it != route.device_to_instance.end()) ? it->second : "";
                if (peer.empty() || peer == _instance_id) continue;
                peer_to_uids[peer].insert(uid);
            }
        }

        // 3) 每个对端一次 PushBatch（异步 brpc::DoNothing）
        std::string internal_b64 = _utils_base64_encode(internal_msg.SerializeAsString());
        for (auto &kv : peer_to_uids) {
            const std::string &peer = kv.first;
            std::vector<std::string> uids(kv.second.begin(), kv.second.end());
            auto channel = _mm_channels->choose(peer);
            if (!channel) {
                LOG_WARN("Push-Consumer: 对端 {} 不可达", peer);
                for (const auto &u : uids)
                    if (_online_route) _online_route->unbind(u, "", peer);
                if (_cross_outbox) {
                    _cross_outbox->enqueue(internal_b64, uids, peer, now_ts);
                }
                continue;
            }
            PushService_Stub stub(channel.get());
            auto *closure = new SelfDeleteRpcClosure<PushBatchReq, PushBatchRsp>();
            closure->req.set_request_id(msg_info.client_msg_id());
            for (const auto &u : uids) closure->req.add_user_id_list(u);
            closure->req.mutable_notify()->CopyFrom(notify_template);
            for (const auto &u : uids) {
                auto it = uid2seq.find(u);
                if (it == uid2seq.end()) continue;
                auto *p = closure->req.add_user_seqs();
                p->set_user_id(u);
                p->set_user_seq(it->second);
            }
            std::string peer_id = peer;
            closure->on_done = [peer_id, uids, outbox = _cross_outbox,
                                online = _online_route, internal_b64, now_ts]
                (brpc::Controller *c, const PushBatchRsp &) {
                if (c->Failed()) {
                    LOG_WARN("PushBatch 跨实例失败 peer={}: {}", peer_id, c->ErrorText());
                    for (const auto &u : uids)
                        if (online) online->unbind(u, "", peer_id);
                    if (outbox) outbox->enqueue(internal_b64, uids, peer_id, now_ts);
                }
            };
            stub.PushBatch(&closure->cntl, &closure->req, &closure->rsp, closure);
        }
        return ConsumeAction::Ack;
    }

    void onClientNotify(const NotifyMessage &notify, server_t::connection_ptr conn) {
        if (notify.notify_type() == NotifyType::CLIENT_AUTH) {
            _handle_client_auth_(notify.client_auth(), conn);
        } else if (notify.notify_type() == NotifyType::MSG_PUSH_ACK) {
            const auto &ack = notify.msg_push_ack();
            if (ack.user_seq() == 0 || ack.user_id().empty() ||
                ack.conversation_id().empty() || ack.device_id().empty()) {
                LOG_WARN("MSG_PUSH_ACK: invalid fields uid={} did={} seq={}",
                         ack.user_id(), ack.device_id(), ack.user_seq());
                return;
            }

            std::string conn_uid, conn_did, conn_jti;
            if (!_connections->client(conn, conn_uid, conn_did, conn_jti)) {
                LOG_WARN("MSG_PUSH_ACK: no connection identity");
                return;
            }
            if (conn_uid != ack.user_id() || conn_did != ack.device_id()) {
                LOG_WARN("MSG_PUSH_ACK: identity mismatch ack_uid={} ack_did={} conn_uid={} conn_did={}",
                         ack.user_id(), ack.device_id(), conn_uid, conn_did);
                return;
            }
            if (_unacked) _unacked->ack(ack.user_id(), ack.device_id(), ack.user_seq());

            // 异步上报 UpdateReadAck（无入站 RPC context，需手动设置 auth metadata）
            auto channel = _mm_channels->choose(_message_service_name);
            if (!channel) {
                LOG_WARN("UpdateReadAck: message service 不可达 uid={}", ack.user_id());
                return;
            }
            chatnow::message::MessageService_Stub stub(channel.get());
            auto *closure = new SelfDeleteRpcClosure<
                chatnow::message::UpdateReadAckReq,
                chatnow::message::UpdateReadAckRsp>();
            closure->req.set_request_id(ack.user_id());
            closure->req.set_conversation_id(ack.conversation_id());
            closure->req.set_seq_id(ack.user_seq());
            // 手动设置 auth metadata：WS handler 无入站 RPC context，需自行构造 RpcMetadata
            ::chatnow::rpc::RpcMetadata meta;
            meta.set_user_id(conn_uid);
            meta.set_device_id(conn_did);
            meta.set_trace_id(::chatnow::utils::gen_trace_id());
            std::string data;
            meta.SerializeToString(&data);
            closure->cntl.request_attachment().append(data);
            closure->on_done = [uid = ack.user_id(), seq = ack.user_seq()]
                (brpc::Controller *c, const chatnow::message::UpdateReadAckRsp &r) {
                if (c->Failed()) {
                    LOG_WARN("UpdateReadAck RPC 失败 uid={} seq={}: {}", uid, seq, c->ErrorText());
                }
            };
            stub.UpdateReadAck(&closure->cntl, &closure->req, &closure->rsp, closure);
        } else if (notify.notify_type() == NotifyType::CLIENT_HEARTBEAT) {
            const auto &hb = notify.heartbeat();
            _on_heartbeat_resend(hb);
        }
    }

    void shutdown_cleanup() {
        LOG_INFO("Push shutdown: cleaning OnlineRoute...");
        // SCAN all online keys and unbind those belonging to this instance.
        // Cluster mode: for_each traverses all nodes via RedisClient::scan().
        // OPTIMIZE: batch hgetall per SCAN page via pipeline to reduce shutdown latency
        long long cursor = 0;
        do {
            std::vector<std::string> keys;
            cursor = _redis->scan(cursor, "im:online:*", 100, std::back_inserter(keys));
            for (const auto &key : keys) {
                std::string uid = key.substr(std::string("im:online:").size());
                std::unordered_map<std::string, std::string> device_map;
                _redis->hgetall(key, std::inserter(device_map, device_map.end()));
                for (const auto &[did, instance] : device_map) {
                    if (instance == _instance_id) {
                        _online_route->unbind(uid, did, _instance_id);
                    }
                }
                if (_local_route_cache) _local_route_cache->invalidate("route:" + uid);
            }
        } while (cursor != 0);
        LOG_INFO("Push shutdown: OnlineRoute + L1 cache cleaned");
    }

    /* brief: 给特定设备推送 KICKED 通知 */
    void publish_kicked(const std::string &uid, const std::string &device_id,
                        NotifyType reason, const std::string &msg) {
        NotifyMessage notify;
        notify.set_notify_type(reason);
        auto *kicked = notify.mutable_kicked();
        kicked->set_reason(reason);
        kicked->set_message(msg);
        _local_send(uid, device_id, notify.SerializeAsString());
    }

private:
    void _handle_client_auth_(const NotifyClientAuth &auth,
                              server_t::connection_ptr conn) {
        if (auth.access_token().empty() || auth.device_id().empty()) {
            LOG_WARN("WS CLIENT_AUTH missing fields");
            try { conn->close(websocketpp::close::status::unsupported_data,
                              "access_token/device_id required"); } catch (std::exception &e) { LOG_WARN("WS close failed: {}", e.what()); }
            return;
        }

        // JWT 验签
        chatnow::auth::JwtClaims claims;
        try {
            claims = _jwt_codec->verify(auth.access_token());
        } catch (const chatnow::ServiceError &e) {
            LOG_WARN("WS JWT verify failed: {}", e.what());
            try { conn->close(websocketpp::close::status::unsupported_data,
                              "auth failed"); } catch (std::exception &e) { LOG_WARN("WS close failed: {}", e.what()); }
            return;
        }

        std::string uid = claims.sub;
        std::string did = claims.did;
        std::string jti = claims.jti;

        _connections->insert(conn, uid, did, jti);
        if (_online_route) _online_route->bind(uid, did, _instance_id);

        // 写 Presence（Push 为写入端）
        _write_presence_online_(uid, did);

        LOG_INFO("WS auth success uid={} device={}", uid, did);

        // 携带 last_user_seq 时立即触发补送
        if (auth.has_last_user_seq() && auth.last_user_seq() > 0) {
            NotifyMessage hb;
            hb.set_notify_type(NotifyType::CLIENT_HEARTBEAT);
            hb.mutable_heartbeat()->set_user_id(uid);
            hb.mutable_heartbeat()->set_last_user_seq(auth.last_user_seq());
            onClientNotify(hb, conn);
        }
    }

    void _on_heartbeat_resend(const NotifyHeartbeat &hb) {
        if (!_unacked) return;
        const std::string uid = hb.user_id();
        if (uid.empty()) return;

        auto route = resolve_route(uid);
        for (const auto &did : route.device_ids) {
            auto pending = _unacked->peek_due(uid, did, _resend_batch, _resend_max_age_sec);
            if (pending.empty()) continue;

            int sent = 0;
            std::vector<unsigned long> seqs;
            for (const auto &[user_seq, payload_b64] : pending) {
                std::string payload = _utils_base64_decode(payload_b64);
                if (!payload.empty()) {
                    _local_send(uid, did, payload);
                    ++sent;
                }
                seqs.push_back(user_seq);
            }

            if (!seqs.empty() && _unacked) {
                _unacked->bump_score(uid, did, seqs);
            }
            LOG_INFO("Heartbeat-补送 uid={} did={} 取出 {} 条 发送 {} 条",
                     uid, did, pending.size(), sent);
        }
    }

    void _write_presence_online_(const std::string &uid, const std::string &did) {
        try {
            std::string k = std::string("im:presence:device:{") + uid + "}:" + did;
            auto pipe = _redis->pipeline();
            pipe.hset(k, "state", "ONLINE");
            pipe.hset(k, "last_active_at_ms", std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
            pipe.expire(k, std::chrono::seconds(kPresenceTtlSec));
            pipe.exec();
        } catch (std::exception &e) {
            LOG_WARN("Presence write failed uid={} did={}: {}", uid, did, e.what());
        }
    }

    void _write_presence_offline_(const std::string &uid, const std::string &did) {
        try {
            std::string k = std::string("im:presence:device:{") + uid + "}:" + did;
            auto pipe = _redis->pipeline();
            pipe.hset(k, "state", "OFFLINE");
            pipe.hset(k, "last_active_at_ms", std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
            pipe.expire(k, std::chrono::seconds(kPresenceTtlSec));
            pipe.exec();
        } catch (std::exception &e) {
            LOG_WARN("Presence offline write failed uid={} did={}: {}", uid, did, e.what());
        }
    }

    void _refresh_presence_ttl_(const std::string &uid, const std::string &did) {
        try {
            std::string k = std::string("im:presence:device:{") + uid + "}:" + did;
            _redis->expire(k, std::chrono::seconds(kPresenceTtlSec));
        } catch (std::exception &e) {
            LOG_WARN("Presence TTL refresh failed uid={} did={}: {}", uid, did, e.what());
        }
    }

    RouteEntry resolve_route(const std::string &uid) {
        if (!_online_route) return RouteEntry{};
        std::string cache_key = "route:" + uid;

        //  L1 hit → fast path (~ns)
        if (_local_route_cache) {
            auto cached = _local_route_cache->get(cache_key);
            if (cached.has_value()) return *cached;
        }

        //  L1 miss → acquire InflightRegistry per-key lock
        auto guard = _inflight_registry ? _inflight_registry->acquire(cache_key)
                                        : InflightRegistry::Guard{};
        std::shared_ptr<std::mutex> lock_mu = guard.mu;
        std::unique_lock<std::mutex> lk(lock_mu ? *lock_mu : _dummy_mu_);

        //  Double-check L1 (another thread may have just finished warm)
        if (_local_route_cache) {
            auto cached = _local_route_cache->get(cache_key);
            if (cached.has_value()) {
                lk.unlock();
                guard = InflightRegistry::Guard{};
                return *cached;
            }
        }

        //  L2 Redis: hgetall + build RouteEntry
        RouteEntry route;
        auto dmap = _online_route->device_instances_map(uid);
        route.device_ids.reserve(dmap.size());
        for (const auto &[did, inst] : dmap) {
            route.device_ids.push_back(did);
            route.device_to_instance[did] = inst;
        }
        if (_local_route_cache) {
            _local_route_cache->set(cache_key, route, randomized_ttl(std::chrono::seconds(2)));
        }

        lk.unlock();
        guard = InflightRegistry::Guard{};
        return route;
    }

    /* brief: 本实例直接通过 WS 下发；返回送达连接数（批量取 mutex 减少全局锁争用） */
    int _local_send(const std::string &uid, const std::string &device_id,
                    const std::string &payload) {
        auto conns = _connections->connections(uid, device_id);
        auto mutexes = _connections->send_mutexes(uid, device_id);
        int sent = 0;
        for (size_t i = 0; i < conns.size() && i < mutexes.size(); ++i) {
            try {
                auto &c = conns[i];
                if (!c || c->get_state() != websocketpp::session::state::value::open) continue;
                if (!mutexes[i]) continue;
                std::lock_guard<std::mutex> lock(*mutexes[i]);
                c->send(payload, websocketpp::frame::opcode::value::binary);
                ++sent;
            } catch (std::exception &e) {
                LOG_WARN("WS send 失败 uid={} did={}: {}", uid, device_id, e.what());
            }
        }
        return sent;
    }

public:
    void start_cross_outbox_reaper(const std::string &owner) {
        if (!_cross_outbox || !_mm_channels) return;
        constexpr int kReapIntervalSec = 5;
        constexpr int kBatchLimit = 50;
        _cross_reaper_running.store(true);
        _cross_reaper_owner = owner;
        _cross_reaper_thread = std::thread([this, kReapIntervalSec, kBatchLimit]() {
            while (_cross_reaper_running.load()) {
                try {
                    if (!_cross_reaper_election || !_cross_reaper_election->is_leader()) {
                        std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                        continue;
                    }
                    auto batch = _cross_outbox->peek(kBatchLimit);
                    if (batch.empty()) {
                        std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                        continue;
                    }
                    for (const auto &member : batch) {
                        std::string b64, peer;
                        std::vector<std::string> uids;
                        if (!_parse_outbox_member(member, b64, uids, peer)) {
                            LOG_WARN("CrossInstanceOutbox: skip malformed member");
                            _cross_outbox->remove(member);
                            continue;
                        }

                        chatnow::message::internal::InternalMessage internal_msg;
                        if (!internal_msg.ParseFromString(_utils_base64_decode(b64))) {
                            LOG_ERROR("CrossInstanceOutbox: 反序列化失败，丢弃");
                            _cross_outbox->remove(member);
                            continue;
                        }

                        // 按实例分组重发
                        std::unordered_map<std::string, std::vector<std::string>> peer_to_uids;
                        for (const auto &uid : uids) {
                            auto route = resolve_route(uid);
                            for (const auto &did : route.device_ids) {
                                auto it = route.device_to_instance.find(did);
                                std::string inst = (it != route.device_to_instance.end()) ? it->second : "";
                                if (inst == _instance_id) continue;
                                peer_to_uids[inst].push_back(uid);
                                break;
                            }
                        }

                        NotifyMessage notify_template;
                        notify_template.set_notify_type(NotifyType::CHAT_MESSAGE_NOTIFY);
                        notify_template.mutable_new_message_info()
                            ->mutable_message_info()->CopyFrom(internal_msg.message());

                        for (auto &kv : peer_to_uids) {
                            auto channel = _mm_channels->choose(kv.first);
                            if (!channel) { continue; }
                            PushService_Stub stub(channel.get());
                            auto *closure = new SelfDeleteRpcClosure<PushBatchReq, PushBatchRsp>();
                            closure->req.set_request_id(
                                internal_msg.message().client_msg_id());
                            for (const auto &u : kv.second) closure->req.add_user_id_list(u);
                            closure->req.mutable_notify()->CopyFrom(notify_template);
                            for (const auto &up : internal_msg.user_seqs()) {
                                if (std::find(kv.second.begin(), kv.second.end(),
                                              up.user_id()) != kv.second.end()) {
                                    auto *seq = closure->req.add_user_seqs();
                                    seq->set_user_id(up.user_id());
                                    seq->set_user_seq(up.user_seq());
                                }
                            }
                            stub.PushBatch(&closure->cntl, &closure->req,
                                           &closure->rsp, closure);
                        }
                        // 在所有 PushBatch RPC 发起之后才移除，避免崩溃导致数据丢失
                        _cross_outbox->remove(member);
                    }
                } catch (std::exception &e) {
                    LOG_ERROR("CrossInstanceOutbox reaper 异常: {}", e.what());
                }
                std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
            }
            LOG_INFO("CrossInstanceOutbox reaper 已停止");
        });
    }

    void stop_cross_outbox_reaper() {
        _cross_reaper_running.store(false);
        if (_cross_reaper_thread.joinable()) _cross_reaper_thread.join();
        if (_cross_reaper_election) _cross_reaper_election->stop();
    }

    const auto& connections() const { return _connections; }

    void reap_stale_routes_(const std::string &push_service_dir,
                            std::shared_ptr<etcd::Client> etcd_client,
                            LeaderElection::ptr stale_reaper_election,
                            std::shared_ptr<std::atomic<bool>> running) {
        while (running && running->load()) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!stale_reaper_election || !stale_reaper_election->is_leader())
                continue;

            std::vector<std::string> online_instances;
            try {
                auto resp = etcd_client->ls(push_service_dir).get();
                if (resp.is_ok()) {
                    for (size_t i = 0; i < resp.keys().size(); ++i)
                        online_instances.push_back(resp.key(i));
                }
            } catch (std::exception &e) {
                LOG_WARN("StaleRoute reaper: etcd ls 失败: {}", e.what());
                continue;
            }

            if (online_instances.empty()) {
                LOG_WARN("StaleRoute reaper: empty instance list, skip cleanup");
                continue;
            }

            std::vector<std::pair<std::string, std::string>> stale_entries;
            // Cluster mode: for_each traverses all nodes via RedisClient::scan().
            long long cursor = 0;
            do {
                std::vector<std::string> keys;
                cursor = _redis->scan(cursor, "im:online:*", 100, std::back_inserter(keys));
                for (const auto &key : keys) {
                    std::string uid = key.substr(std::string("im:online:").size());
                    std::unordered_map<std::string, std::string> device_map;
                    _redis->hgetall(key, std::inserter(device_map, device_map.end()));
                    for (const auto &[did, instance] : device_map) {
                        if (std::find(online_instances.begin(), online_instances.end(), instance)
                            == online_instances.end()) {
                            stale_entries.emplace_back(uid, did);
                        }
                    }
                }
            } while (cursor != 0);

            for (const auto &[uid, did] : stale_entries) {
                _online_route->unbind(uid, did, "");
                if (_local_route_cache) _local_route_cache->invalidate("route:" + uid);
            }
            if (!stale_entries.empty())
                LOG_INFO("StaleRoute reaper: 移除 {} 条僵死路由", stale_entries.size());
        }
    }

private:
    bool _parse_outbox_member(const std::string &member,
                               std::string &b64,
                               std::vector<std::string> &uids,
                               std::string &peer) {
        picojson::value j;
        std::string err = picojson::parse(j, member);
        if (!err.empty()) {
            LOG_WARN("CrossInstanceOutbox JSON parse failed: {}", err);
            return false;
        }
        if (!j.is<picojson::object>()) return false;

        const auto &obj = j.get<picojson::object>();
        auto it_k = obj.find("k");
        if (it_k != obj.end() && it_k->second.is<std::string>()) {
            b64 = it_k->second.get<std::string>();
        }
        auto it_p = obj.find("p");
        if (it_p != obj.end() && it_p->second.is<std::string>()) {
            peer = it_p->second.get<std::string>();
        }
        auto it_u = obj.find("u");
        if (it_u != obj.end() && it_u->second.is<picojson::array>()) {
            const auto &arr = it_u->second.get<picojson::array>();
            for (const auto &elem : arr) {
                if (elem.is<std::string>()) uids.push_back(elem.get<std::string>());
            }
        }
        return !b64.empty();
    }

    static std::string _utils_base64_encode(const std::string &in) {
        int cap = ((in.size() + 2) / 3) * 4;
        std::string out(cap, '\0');
        int n = EVP_EncodeBlock(
            reinterpret_cast<unsigned char*>(out.data()),
            reinterpret_cast<const unsigned char*>(in.data()),
            static_cast<int>(in.size()));
        out.resize(static_cast<size_t>(n));
        return out;
    }
    static std::string _utils_base64_decode(const std::string &in) {
        if (in.empty()) return "";
        int cap = (static_cast<int>(in.size()) / 4) * 3 + 1;
        std::string out(cap, '\0');
        int n = EVP_DecodeBlock(
            reinterpret_cast<unsigned char*>(out.data()),
            reinterpret_cast<const unsigned char*>(in.data()),
            static_cast<int>(in.size()));
        if (n < 0) return "";
        int pads = static_cast<int>(std::count(in.begin(), in.end(), '='));
        if (n > pads) n -= pads;
        out.resize(static_cast<size_t>(n));
        return out;
    }

    Connection::ptr _connections;
    std::shared_ptr<chatnow::auth::JwtCodec> _jwt_codec;
    RedisClient::ptr _redis;
    OnlineRoute::ptr _online_route;
    UnackedPush::ptr _unacked;
    CrossInstanceOutbox::ptr _cross_outbox;
    std::string _instance_id;
    std::string _message_service_name;
    ServiceManager::ptr _mm_channels;
    long _resend_batch{50};
    long _resend_max_age_sec{5};
    std::atomic<bool> _cross_reaper_running{false};
    std::thread _cross_reaper_thread;
    std::string _cross_reaper_owner;
    LeaderElection::ptr _cross_reaper_election;
    LocalCache<RouteEntry>::ptr _local_route_cache;
    InflightRegistry::ptr _inflight_registry;
    std::mutex _dummy_mu_;
};

class PushServer
{
public:
    using ptr = std::shared_ptr<PushServer>;
    PushServer(const Discovery::ptr &disc,
               const Registry::ptr &reg,
               const std::shared_ptr<brpc::Server> &rpc,
               std::unique_ptr<server_t> ws_server,
               const MQClient::ptr &mq_client,
               const Subscriber::ptr &push_subscriber,
               PushServiceImpl *push_service = nullptr,
               std::thread *stale_reaper_thread = nullptr,
               std::shared_ptr<std::atomic<bool>> stale_reaper_running = nullptr)
        : _service_discover(disc), _reg_client(reg), _rpc_server(rpc), _ws_server(std::move(ws_server)),
          _mq_client(mq_client), _push_subscriber(push_subscriber), _push_service(push_service),
          _stale_reaper_thread(stale_reaper_thread), _stale_reaper_running(stale_reaper_running) {}
    virtual ~PushServer() = default;

    void start() {
        _ws_thread = std::thread([this]() {
            try {
                _ws_server->run();
                LOG_INFO("Push WS 线程正常退出");
            } catch (std::exception &e) {
                LOG_ERROR("Push WS 线程异常退出: {}", e.what());
            }
            _rpc_server->Stop(0);
        });
        _rpc_server->RunUntilAskedToQuit();
        // IMPORTANT: MQ subscriber MUST stop before brpc server shutdown.
        // The MQ callback captures a raw _push_service pointer (owned by brpc via SERVER_OWNS_SERVICE).
        _push_subscriber.reset();
        _mq_client.reset();
        _ws_server->stop();

        // 关停清理：遍历连接，主动清理 OnlineRoute 和 L1 缓存
        if (_push_service) {
            _push_service->shutdown_cleanup();
        }

        // 停止 StaleRoute reaper
        if (_stale_reaper_running) {
            _stale_reaper_running->store(false);
            if (_stale_reaper_thread && _stale_reaper_thread->joinable())
                _stale_reaper_thread->join();
        }

        if (_ws_thread.joinable()) _ws_thread.join();
        _rpc_server->Join();
        LOG_INFO("Push shutdown complete");
    }

private:
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
    std::unique_ptr<server_t> _ws_server;
    MQClient::ptr _mq_client;
    Subscriber::ptr _push_subscriber;
    PushServiceImpl *_push_service{nullptr};
    std::thread _ws_thread;
    std::thread *_stale_reaper_thread{nullptr};
    std::shared_ptr<std::atomic<bool>> _stale_reaper_running;
};

class PushServerBuilder
{
public:
    void make_jwt_object(const std::string &auth_config_path) {
        auto cfg = ::chatnow::auth::load_jwt_config_from_file(auth_config_path);
        _jwt_codec = std::make_shared<chatnow::auth::JwtCodec>(cfg);
    }

    void set_redis_seeds(const std::string &seeds) { _redis_seeds = seeds; }

    void make_redis_object(const std::string &host, uint16_t port, int db,
                           bool keep_alive, int pool_size)
    {
        if (!_redis_seeds.empty()) {
            auto cluster = RedisClusterFactory::create(_redis_seeds, pool_size, keep_alive);
            _redis_client = std::make_shared<RedisClient>(cluster);
        } else {
            auto redis = RedisClientFactory::create(host, port, db, keep_alive, pool_size);
            _redis_client = std::make_shared<RedisClient>(redis);
        }
        _online_route = std::make_shared<OnlineRoute>(_redis_client);
        _unacked      = std::make_shared<UnackedPush>(_redis_client);
        _cross_outbox = std::make_shared<CrossInstanceOutbox>(_redis_client);
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
        _instance_id = service_name;
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

    void make_ws_object(uint16_t ws_port) {
        _ws_server = std::make_unique<server_t>();
        _ws_server->set_access_channels(websocketpp::log::alevel::none);
        _ws_server->clear_error_channels(websocketpp::log::elevel::none);
        _ws_server->init_asio();
        _ws_server->set_max_message_size(65536);  // 64KB limit
        _ws_server->set_reuse_addr(true);
        _ws_server->set_open_handler([this](websocketpp::connection_hdl hdl) {
            LOG_DEBUG("WS 连接建立 {}", (size_t)_ws_server->get_con_from_hdl(hdl).get());
        });
        _ws_server->set_close_handler([this](websocketpp::connection_hdl hdl) {
            auto conn = _ws_server->get_con_from_hdl(hdl);
            std::string uid, did, jti;
            if (_connections && _connections->client(conn, uid, did, jti)) {
                _connections->remove(conn);
                if (_online_route) _online_route->unbind(uid, did, _instance_id);
                if (_local_route_cache) _local_route_cache->invalidate("route:" + uid);
                LOG_DEBUG("WS 关闭 uid={} did={}", uid, did);
            }
        });
        _ws_server->set_message_handler([this](websocketpp::connection_hdl hdl, server_t::message_ptr msg) {
            auto conn = _ws_server->get_con_from_hdl(hdl);
            NotifyMessage notify;
            if (!notify.ParseFromString(msg->get_payload())) {
                LOG_WARN("WS payload 反序列化失败，关闭连接");
                _ws_server->close(hdl, websocketpp::close::status::unsupported_data,
                                 "payload invalid");
                return;
            }

            // 路径 A：未鉴权连接的首条消息必须是 CLIENT_AUTH
            std::string uid_known, did_known, jti_known;
            if (!_connections->client(conn, uid_known, did_known, jti_known)) {
                if (notify.notify_type() != NotifyType::CLIENT_AUTH || !notify.has_client_auth()) {
                    LOG_WARN("WS 首条非 CLIENT_AUTH，关闭连接");
                    _ws_server->close(hdl, websocketpp::close::status::unsupported_data,
                                     "auth required");
                    return;
                }
                if (_push_service) {
                    _push_service->onClientNotify(notify, conn);
                }
                return;
            }

            // 路径 B：已鉴权连接的后续消息
            _connections->touch(conn);
            if (_push_service) _push_service->onClientNotify(notify, conn);
            if (notify.notify_type() == NotifyType::CLIENT_HEARTBEAT) {
                _online_route->touch(uid_known);
            }
        });
    }

    void set_resend_params(int batch, int max_age_sec) {
        _resend_batch = batch;
        _resend_max_age_sec = max_age_sec;
    }
    void set_reaper_owner(const std::string &owner) { _reaper_owner = owner; }
    void set_etcd_client(std::shared_ptr<etcd::Client> etcd) { _etcd_client = etcd; }

    void make_cross_reaper_election() {
        if (!_etcd_client) return;
        _cross_reaper_election = std::make_shared<LeaderElection>(
            _etcd_client, "/chatnow/reaper/cross_outbox", _instance_id, 30,
            []() { LOG_INFO("CrossOutbox reaper 成为 leader"); },
            []() { LOG_INFO("CrossOutbox reaper 失去 leader"); });
    }

    void make_local_cache() {
        _local_route_cache = std::make_shared<LocalCache<RouteEntry>>(16384);
        _inflight_registry = std::make_shared<InflightRegistry>();
    }

    void set_push_service_dir(const std::string &dir) { _push_service_dir = dir; }

    void make_stale_reaper_election() {
        if (!_etcd_client) return;
        _stale_reaper_election = std::make_shared<LeaderElection>(
            _etcd_client, "/chatnow/reaper/stale_routes", _instance_id, 30,
            []() { LOG_INFO("StaleRoute reaper 成为 leader"); },
            []() { LOG_INFO("StaleRoute reaper 失去 leader"); });
    }

    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads, uint16_t ws_port) {
        if (!_redis_client) { LOG_ERROR("Push: Redis 未初始化"); abort(); }
        if (!_mm_channels) { LOG_ERROR("Push: 信道管理未初始化"); abort(); }
        if (port == ws_port) {
            LOG_WARN("Push: rpc_port and ws_port are both {}, may conflict", port);
        }
        _connections = std::make_shared<Connection>();
        _rpc_server = std::make_shared<brpc::Server>();
        _push_service = new PushServiceImpl(
            _connections, _jwt_codec, _redis_client, _online_route, _unacked, _cross_outbox,
            _instance_id, _message_service_name, _mm_channels,
            _cross_reaper_election, _local_route_cache, _inflight_registry);
        _push_service->set_resend_params(_resend_batch, _resend_max_age_sec);
        int ret = _rpc_server->AddService(_push_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
        if (ret == -1) { LOG_ERROR("Push: AddService 失败"); abort(); }

        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads = num_threads;
        if (_rpc_server->Start(port, &options) == -1) {
            LOG_ERROR("Push: brpc 启动失败");
            abort();
        }
        // WS server — 先于 MQ 订阅
        make_ws_object(ws_port);
        std::error_code ec;
        _ws_server->listen(ws_port, ec);
        if (ec) { LOG_ERROR("Push: WS 监听失败 {}", ec.message()); abort(); }
        _ws_server->start_accept();

        // MQ 订阅
        auto callback_inner = std::bind(&PushServiceImpl::onPushMessage, _push_service,
                                  std::placeholders::_1, std::placeholders::_2,
                                  std::placeholders::_3);
        chatnow::MessageCallbackWithHeaders callback = [callback_inner](const char* body, size_t sz, bool redeliv,
                                                                        const std::map<std::string, std::string>& headers) -> chatnow::ConsumeAction {
            std::string _trace_id = chatnow::mq::mq_extract_trace_id(headers);
            chatnow::log::LogContext::set(_trace_id, "", "");
            struct _Scope { ~_Scope() { chatnow::log::LogContext::clear(); } } _Scope;
            return callback_inner(body, sz, redeliv);
        };
        _push_subscriber->consume(std::move(callback));

        if (_cross_reaper_election) _cross_reaper_election->start();
        std::string owner = _reaper_owner.empty()
            ? std::to_string(::getpid()) : _reaper_owner;
        _push_service->start_cross_outbox_reaper(owner);

        // Stale route reaper
        if (_stale_reaper_election) {
            _stale_reaper_election->start();
            _stale_reaper_running = std::make_shared<std::atomic<bool>>(true);
            _stale_reaper_thread = std::thread([this, running = _stale_reaper_running]() {
                _push_service->reap_stale_routes_(_push_service_dir, _etcd_client,
                                                  _stale_reaper_election, running);
            });
        }

        LOG_INFO("Push 服务启动: rpc_port={} ws_port={}", port, ws_port);
    }

    PushServer::ptr build() {
        return std::make_shared<PushServer>(std::move(_service_discover),
                                            std::move(_reg_client),
                                            std::move(_rpc_server),
                                            std::move(_ws_server),
                                            std::move(_mq_client),
                                            std::move(_push_subscriber),
                                            _push_service,
                                            std::move(_stale_reaper_thread),
                                            _stale_reaper_running);
    }

private:
    std::string _redis_seeds;
    RedisClient::ptr _redis_client;
    std::shared_ptr<chatnow::auth::JwtCodec> _jwt_codec;
    OnlineRoute::ptr _online_route;
    UnackedPush::ptr _unacked;
    CrossInstanceOutbox::ptr _cross_outbox;

    std::string _message_service_name;
    std::string _push_service_name;
    std::string _instance_id;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;

    declare_settings _push_settings;
    MQClient::ptr _mq_client;
    Subscriber::ptr _push_subscriber;

    int _resend_batch{50};
    int _resend_max_age_sec{5};
    std::string _reaper_owner;
    std::shared_ptr<etcd::Client> _etcd_client;
    LeaderElection::ptr _cross_reaper_election;
    LocalCache<RouteEntry>::ptr _local_route_cache;
    InflightRegistry::ptr _inflight_registry;
    std::string _push_service_dir;
    LeaderElection::ptr _stale_reaper_election;
    std::thread _stale_reaper_thread;
    std::shared_ptr<std::atomic<bool>> _stale_reaper_running;

    Connection::ptr _connections;
    std::unique_ptr<server_t> _ws_server;
    PushServiceImpl *_push_service{nullptr};
    std::shared_ptr<brpc::Server> _rpc_server;
};

} // namespace chatnow::push
