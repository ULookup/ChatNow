#pragma once

#include <brpc/server.h>
#include <butil/logging.h>

#include "dao/data_redis.hpp"
#include "infra/etcd.hpp"
#include "infra/logger.hpp"
#include "mq/channel.hpp"
#include "log/log_context.hpp"
#include "auth/auth_context.hpp"
#include "auth/forward_auth.hpp"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "utils/brpc_closure.hpp"

#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "presence/presence_service.pb.h"
#include "push/push_service.pb.h"
#include "push/notify.pb.h"

#include <sw/redis++/redis++.h>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <chrono>
#include <algorithm>

namespace chatnow::presence {

class PresenceAggregator {
public:
    using ptr = std::shared_ptr<PresenceAggregator>;

    explicit PresenceAggregator(std::shared_ptr<sw::redis::Redis> redis)
        : _redis(std::move(redis)) {}

    Presence aggregate(const std::string& uid) {
        Presence p;
        p.set_user_id(uid);

        // SCAN 找出该 uid 的所有 device key
        std::vector<std::string> device_keys;
        auto cursor = 0LL;
        while (true) {
            std::vector<std::string> batch;
            std::tie(cursor, std::ignore) = _redis->scan(
                cursor, "im:presence:device:" + uid + ":*", 100,
                std::back_inserter(batch));
            device_keys.insert(device_keys.end(), batch.begin(), batch.end());
            if (cursor == 0) break;
        }

        if (device_keys.empty()) {
            p.set_aggregated_state(PresenceState::OFFLINE);
            p.set_last_active_at_ms(0);
            return p;
        }

        // 取当前时间用于 TTL 检查
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // pipeline 批量取每个 device 的 state / platform / last_active
        auto pipe = _redis->pipeline();
        for (auto& k : device_keys) {
            pipe.hget(k, "state");
            pipe.hget(k, "platform");
            pipe.hget(k, "last_active_at_ms");
        }
        auto results = pipe.exec();

        int best_rank = 999;
        int64_t best_active_ms = 0;
        for (size_t i = 0; i < device_keys.size(); ++i) {
            auto state_opt = results[i * 3 + 0].template get<sw::redis::OptionalString>();
            auto plat_opt  = results[i * 3 + 1].template get<sw::redis::OptionalString>();
            auto last_opt  = results[i * 3 + 2].template get<sw::redis::OptionalString>();

            if (!state_opt) continue;

            int state_val = std::stoi(*state_opt);
            PresenceState dev_state = static_cast<PresenceState>(state_val);

            // TTL 检查
            if (last_opt) {
                int64_t last_ms = std::stoll(*last_opt);
                if (now_ms - last_ms > 125000) continue;
            }

            // 提取 device_id
            std::string did = device_keys[i];
            auto pos = did.rfind(':');
            if (pos != std::string::npos) did = did.substr(pos + 1);

            DevicePresence* dp = p.add_devices();
            dp->set_device_id(did);
            dp->set_state(dev_state);
            if (last_opt) dp->set_last_active_at_ms(std::stoll(*last_opt));
            if (plat_opt) {
                dp->set_platform(static_cast<chatnow::common::DevicePlatform>(std::stoi(*plat_opt)));
            }

            // 计算聚合状态: INVISIBLE 对外显示 OFFLINE
            int rank;
            switch (dev_state) {
                case PresenceState::ONLINE:    rank = 0; break;
                case PresenceState::BUSY:      rank = 1; break;
                case PresenceState::AWAY:      rank = 2; break;
                case PresenceState::INVISIBLE: rank = 3; break;  // 对外等同 OFFLINE
                default:                       rank = 3; break;
            }
            if (rank < best_rank || (rank == best_rank && dev_state == PresenceState::ONLINE)) {
                best_rank = rank;
            }

            if (last_opt) {
                int64_t ms = std::stoll(*last_opt);
                if (ms > best_active_ms) best_active_ms = ms;
            }
        }

        if (best_rank >= 3 && !device_keys.empty()) {
            p.set_aggregated_state(PresenceState::OFFLINE);
        } else if (best_rank == 0) {
            p.set_aggregated_state(PresenceState::ONLINE);
        } else if (best_rank == 1) {
            p.set_aggregated_state(PresenceState::BUSY);
        } else if (best_rank == 2) {
            p.set_aggregated_state(PresenceState::AWAY);
        } else {
            p.set_aggregated_state(PresenceState::OFFLINE);
        }
        p.set_last_active_at_ms(best_active_ms);
        return p;
    }

    // 获取所有有订阅关系的在线 uid（从 sub 集合中提取）
    std::vector<std::string> subscribed_uids() {
        std::vector<std::string> result;
        auto cursor = 0LL;
        while (true) {
            std::vector<std::string> batch;
            std::tie(cursor, std::ignore) = _redis->scan(
                cursor, "im:presence:sub:*", 1000, std::back_inserter(batch));
            for (auto& k : batch) {
                auto pos = k.find("im:presence:sub:");
                if (pos == 0) {
                    result.push_back(k.substr(std::string("im:presence:sub:").size()));
                }
            }
            if (cursor == 0) break;
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

private:
    std::shared_ptr<sw::redis::Redis> _redis;
};

class PresenceServiceImpl : public PresenceService {
public:
    PresenceServiceImpl(std::shared_ptr<sw::redis::Redis> redis,
                        const ServiceManager::ptr& channels,
                        const std::string& push_service_name)
        : _redis(std::move(redis)),
          _aggregator(std::make_shared<PresenceAggregator>(_redis)),
          _channels(channels),
          _push_service_name(push_service_name) {}

    void GetPresence(::google::protobuf::RpcController* base_cntl,
                     const GetPresenceReq* req, GetPresenceRsp* rsp,
                     ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        try {
            auto auth = ::chatnow::auth::extract_auth(cntl);
            auto* h = rsp->mutable_header();
            h->set_success(true);
            h->set_error_code(::chatnow::error::kOK);
            h->set_request_id(req->request_id());

            auto p = _aggregator->aggregate(req->user_id());
            *rsp->mutable_presence() = p;
        } catch (const ServiceError& e) {
            rsp->mutable_header()->set_success(false);
            rsp->mutable_header()->set_error_code(e.code());
            rsp->mutable_header()->set_error_message(e.message());
            rsp->mutable_header()->set_request_id(req->request_id());
        } catch (const std::exception& e) {
            rsp->mutable_header()->set_success(false);
            rsp->mutable_header()->set_error_code(::chatnow::error::kSystemInternalError);
            rsp->mutable_header()->set_error_message("internal error");
            rsp->mutable_header()->set_request_id(req->request_id());
            LOG_ERROR("GetPresence 异常 uid={}: {}", req->user_id(), e.what());
        }
    }

    void BatchGetPresence(::google::protobuf::RpcController* base_cntl,
                          const BatchGetPresenceReq* req, BatchGetPresenceRsp* rsp,
                          ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        try {
            auto auth = ::chatnow::auth::extract_auth(cntl);
            auto* h = rsp->mutable_header();
            h->set_success(true);
            h->set_error_code(::chatnow::error::kOK);
            h->set_request_id(req->request_id());

            for (const auto& uid : req->user_ids()) {
                (*rsp->mutable_presences())[uid] = _aggregator->aggregate(uid);
            }
        } catch (const ServiceError& e) {
            rsp->mutable_header()->set_success(false);
            rsp->mutable_header()->set_error_code(e.code());
            rsp->mutable_header()->set_error_message(e.message());
            rsp->mutable_header()->set_request_id(req->request_id());
        } catch (const std::exception& e) {
            rsp->mutable_header()->set_success(false);
            rsp->mutable_header()->set_error_code(::chatnow::error::kSystemInternalError);
            rsp->mutable_header()->set_error_message("internal error");
            rsp->mutable_header()->set_request_id(req->request_id());
            LOG_ERROR("BatchGetPresence 异常: {}", e.what());
        }
    }

    void SubscribePresence(::google::protobuf::RpcController* base_cntl,
                           const SubscribeReq* req, SubscribeRsp* rsp,
                           ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        try {
            auto auth = ::chatnow::auth::extract_auth(cntl);
            auto* h = rsp->mutable_header();
            h->set_success(true);
            h->set_error_code(::chatnow::error::kOK);
            h->set_request_id(req->request_id());

            for (const auto& target_uid : req->subscribe_user_ids()) {
                _redis->sadd("im:presence:sub:" + target_uid, auth.user_id);
            }
        } catch (const ServiceError& e) {
            rsp->mutable_header()->set_success(false);
            rsp->mutable_header()->set_error_code(e.code());
            rsp->mutable_header()->set_error_message(e.message());
            rsp->mutable_header()->set_request_id(req->request_id());
        }
    }

    void UnsubscribePresence(::google::protobuf::RpcController* base_cntl,
                             const UnsubscribeReq* req, UnsubscribeRsp* rsp,
                             ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        try {
            auto auth = ::chatnow::auth::extract_auth(cntl);
            auto* h = rsp->mutable_header();
            h->set_success(true);
            h->set_error_code(::chatnow::error::kOK);
            h->set_request_id(req->request_id());

            for (const auto& target_uid : req->unsubscribe_user_ids()) {
                _redis->srem("im:presence:sub:" + target_uid, auth.user_id);
            }
        } catch (const ServiceError& e) {
            rsp->mutable_header()->set_success(false);
            rsp->mutable_header()->set_error_code(e.code());
            rsp->mutable_header()->set_error_message(e.message());
            rsp->mutable_header()->set_request_id(req->request_id());
        }
    }

    void SendTyping(::google::protobuf::RpcController* base_cntl,
                    const TypingReq* req, TypingRsp* rsp,
                    ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        try {
            auto auth = ::chatnow::auth::extract_auth(cntl);
            auto* h = rsp->mutable_header();
            h->set_success(true);
            h->set_error_code(::chatnow::error::kOK);
            h->set_request_id(req->request_id());

            if (req->is_typing()) {
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                _redis->sadd("im:presence:typing:" + req->conversation_id(),
                             auth.user_id + ":" + std::to_string(now_ms));
                _redis->expire("im:presence:typing:" + req->conversation_id(), 10);
            } else {
                std::vector<std::string> members;
                _redis->smembers("im:presence:typing:" + req->conversation_id(),
                                 std::inserter(members, members.end()));
                for (const auto& m : members) {
                    if (m.find(auth.user_id + ":") == 0) {
                        _redis->srem("im:presence:typing:" + req->conversation_id(), m);
                    }
                }
            }
        } catch (const ServiceError& e) {
            rsp->mutable_header()->set_success(false);
            rsp->mutable_header()->set_error_code(e.code());
            rsp->mutable_header()->set_error_message(e.message());
            rsp->mutable_header()->set_request_id(req->request_id());
        }
    }

    void start_change_scanner(int interval_sec = 5) {
        _scan_running = true;
        _scan_thread = std::thread([this, interval_sec]() {
            std::unordered_map<std::string, PresenceState> last_state;
            while (_scan_running) {
                std::this_thread::sleep_for(std::chrono::seconds(interval_sec));
                if (!_scan_running) break;

                auto subscribed = _aggregator->subscribed_uids();
                for (const auto& uid : subscribed) {
                    auto p = _aggregator->aggregate(uid);
                    auto it = last_state.find(uid);
                    if (it == last_state.end() || it->second != p.aggregated_state()) {
                        last_state[uid] = p.aggregated_state();
                        notify_subscribers(uid, p);
                    }
                }
            }
        });
    }

    void stop_change_scanner() {
        _scan_running = false;
        if (_scan_thread.joinable()) _scan_thread.join();
    }

    ~PresenceServiceImpl() { stop_change_scanner(); }

private:
    void notify_subscribers(const std::string& uid, const Presence& p) {
        std::vector<std::string> subs;
        _redis->smembers("im:presence:sub:" + uid, std::inserter(subs, subs.end()));
        if (subs.empty()) return;

        auto channel = _channels->choose(_push_service_name);
        if (!channel) {
            LOG_WARN("Presence 变化推送失败: Push 不可用 uid={}", uid);
            return;
        }

        NotifyMessage notify;
        notify.set_notify_type(NotifyType::PRESENCE_CHANGE_NOTIFY);
        auto* pc = notify.mutable_presence_change();
        pc->set_user_id(uid);
        pc->set_state(PresenceState_Name(p.aggregated_state()));

        for (const auto& sub_uid : subs) {
            PushService_Stub stub(channel.get());
            auto* closure = new SelfDeleteRpcClosure<PushToUserReq, PushToUserRsp>();
            closure->req.set_user_id(sub_uid);
            closure->req.mutable_notify()->CopyFrom(notify);
            stub.PushToUser(&closure->cntl, &closure->req, &closure->rsp, closure);
        }
    }

    std::shared_ptr<sw::redis::Redis> _redis;
    PresenceAggregator::ptr _aggregator;
    ServiceManager::ptr _channels;
    std::string _push_service_name;

    std::thread _scan_thread;
    bool _scan_running = false;
};

}  // namespace chatnow::presence
