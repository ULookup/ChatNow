#pragma once

#include "connection.hpp"
#include "infra/etcd.hpp"
#include "infra/logger.hpp"
#include "mq/channel.hpp"
#include "mq/rabbitmq.hpp"
#include "dao/data_redis.hpp"
#include "utils/brpc_closure.hpp"
#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "presence/presence_service.pb.h"
#include "push/notify.pb.h"
#include "push/push_service.pb.h"
#include "message/message_types.pb.h"
#include "message/message_service.pb.h"
#include <brpc/server.h>
#include <thread>
#include <chrono>
#include <limits>
#include <unordered_set>

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
                    const CrossInstanceOutbox::ptr &cross_outbox,
                    const std::string &instance_id,
                    const std::string &message_service_name,
                    const ServiceManager::ptr &channels)
        : _connections(connections),
          _redis_session(redis_session),
          _redis_status(redis_status),
          _online_route(online_route),
          _unacked(unacked),
          _cross_outbox(cross_outbox),
          _instance_id(instance_id),
          _message_service_name(message_service_name),
          _mm_channels(channels) {}

    /* M5: 重发参数注入（gflags 来源） */
    void set_resend_params(long batch, long max_age_sec) {
        _resend_batch = batch;
        _resend_max_age_sec = max_age_sec;
    }
    ~PushServiceImpl() { stop_cross_outbox_reaper(); }

    // brpc: 单用户推送（其它服务调用）
    void PushToUser(google::protobuf::RpcController* controller,
                    const ::chatnow::PushToUserReq* request,
                    ::chatnow::PushToUserRsp* response,
                    ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard rpc_guard(done);
        const std::string &rid = request->request_id();
        response->set_request_id(rid);

        // 若调用方带了 user_seq 且为聊天消息：覆写到 MessageInfo.user_seq，
        // 让客户端能据此正确填 NotifyMsgPushAck（B1）。
        std::string payload;
        const NotifyMessage &notify = request->notify();
        if(request->has_user_seq() &&
           notify.notify_type() == NotifyType::CHAT_MESSAGE_NOTIFY &&
           notify.has_new_message_info()) {
            NotifyMessage per_user = notify;
            per_user.mutable_new_message_info()->mutable_message_info()
                ->set_user_seq(request->user_seq());
            payload = per_user.SerializeAsString();
        } else {
            payload = notify.SerializeAsString();
        }

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
        std::unordered_map<std::string, unsigned long> uid2seq;
        for(const auto &p : request->user_seqs()) uid2seq[p.user_id()] = p.user_seq();

        const NotifyMessage &base_notify = request->notify();
        // 仅消息推送类型才需要 per-uid 覆写 user_seq；其它通知（好友 / 会话）走原 payload
        bool is_chat_msg = (base_notify.notify_type() == NotifyType::CHAT_MESSAGE_NOTIFY) &&
                          base_notify.has_new_message_info();
        std::string broadcast_payload;
        if(!is_chat_msg) broadcast_payload = base_notify.SerializeAsString();

        int total = 0;
        long long now_ts = static_cast<long long>(time(nullptr));
        for(const auto &uid : request->user_id_list()) {
            std::string payload;
            if(is_chat_msg) {
                NotifyMessage per_user = base_notify;
                auto it = uid2seq.find(uid);
                if(it != uid2seq.end()) {
                    per_user.mutable_new_message_info()->mutable_message_info()->set_user_seq(it->second);
                }
                payload = per_user.SerializeAsString();
            } else {
                payload = broadcast_payload;
            }
            int n = _local_send(uid, payload);
            if(n > 0) total++;
            auto it = uid2seq.find(uid);
            if(it != uid2seq.end() && _unacked) {
                _unacked->push(uid, it->second, now_ts);
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

        std::unordered_map<std::string, unsigned long> uid2seq;
        for(const auto &p : internal_msg.user_seqs()) uid2seq[p.user_id()] = p.user_seq();

        // B1: 推送前必须为每个收件人填好 user_seq —— 客户端按此字段回 ACK，
        //     这里需要 per-uid 重新序列化，不能广播同一份 payload。
        // 跨实例转发使用不带 user_seq 的模板（对端 PushBatch 收到后会按 user_seqs 注入）。
        NotifyMessage notify_template;
        notify_template.set_notify_type(NotifyType::CHAT_MESSAGE_NOTIFY);
        notify_template.mutable_new_message_info()->mutable_message_info()->CopyFrom(msg_info);
        auto build_payload_for = [&](const std::string &uid) -> std::string {
            NotifyMessage notify = notify_template;
            auto it = uid2seq.find(uid);
            if(it != uid2seq.end()) {
                notify.mutable_new_message_info()->mutable_message_info()->set_user_seq(it->second);
            }
            // 大群读扩散无 user_seq → 不下发 ACK 链路（客户端按 (session_id, seq_id) 增量补漏）
            return notify.SerializeAsString();
        };

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
            std::string payload = build_payload_for(uid);
            int n = _local_send(uid, payload);
            if(n == 0) remote_uids.push_back(uid);
            else {
                auto it = uid2seq.find(uid);
                if(it != uid2seq.end()) {
                    std::string cache_key = uid + ":" + std::to_string(it->second);
                    std::lock_guard<std::mutex> lock(_msg_cache_mu);
                    auto cache_it = _msg_cache.find(cache_key);
                    if(cache_it != _msg_cache.end()) {
                        (*cache_it->second)->payload = std::move(payload);
                    } else {
                        _msg_evict_list.push_back({cache_key, std::move(payload)});
                        auto new_it = std::prev(_msg_evict_list.end());
                        _msg_cache[cache_key] = new_it;
                        if(_msg_evict_list.size() > _msg_cache_max_entries) {
                            _msg_cache.erase(_msg_evict_list.front().key);
                            _msg_evict_list.pop_front();
                        }
                    }
                }
            }
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
                LOG_WARN("Push-Consumer: 对端 {} 不可达，{} 个用户入 CrossInstanceOutbox", peer, uids.size());
                for(const auto &u : uids)
                    if(_online_route) _online_route->unbind(u, peer);
                if(_cross_outbox) {
                    std::string b64 = _utils_base64_encode(internal_msg.SerializeAsString());
                    _cross_outbox->enqueue(b64, uids, peer, now_ts);
                }
                continue;
            }
            PushService_Stub stub(channel.get());
            // 自删 Closure：cntl/req/rsp 与回调上下文一同生命周期管理
            auto *closure = new SelfDeleteRpcClosure<PushBatchReq, PushBatchRsp>();
            closure->req.set_request_id(msg_info.client_msg_id());
            for(const auto &u : uids) closure->req.add_user_id_list(u);
            closure->req.mutable_notify()->CopyFrom(notify_template);
            for(const auto &u : uids) {
                auto it = uid2seq.find(u);
                if(it == uid2seq.end()) continue;
                auto *p = closure->req.add_user_seqs();
                p->set_user_id(u);
                p->set_user_seq(it->second);
            }
            std::string peer_id = peer;
            std::string payload_b64 = _utils_base64_encode(internal_msg.SerializeAsString());
            closure->on_done = [peer_id, uids, outbox = _cross_outbox, online = _online_route,
                                payload_b64, now_ts]
                (brpc::Controller *c, const PushBatchRsp &) {
                if(c->Failed()) {
                    LOG_WARN("PushBatch 跨实例失败 peer={}: {}，入 CrossInstanceOutbox",
                             peer_id, c->ErrorText());
                    for(const auto &u : uids)
                        if(online) online->unbind(u, peer_id);
                    if(outbox) {
                        outbox->enqueue(payload_b64, uids, peer_id, now_ts);
                    }
                }
            };
            stub.PushBatch(&closure->cntl, &closure->req, &closure->rsp, closure);
        }
        return ConsumeAction::Ack;
    }

    /* brief: WebSocket 入口 — 客户端 ACK / 心跳处理 */
    void onClientNotify(const NotifyMessage &notify) {
        if(notify.notify_type() == NotifyType::MSG_PUSH_ACK) {
            const auto &ack = notify.msg_push_ack();
            // 防御：大群读扩散场景客户端不应回 ACK；user_seq=0 视为非法包丢弃，避免污染 last_ack_seq
            if(ack.user_seq() == 0) {
                LOG_WARN("收到非法 MSG_PUSH_ACK user_seq=0 uid={}", ack.user_id());
                return;
            }
            if(ack.user_id().empty() || ack.chat_session_id().empty()) {
                LOG_WARN("收到非法 MSG_PUSH_ACK 缺字段 uid={} ssid={}",
                         ack.user_id(), ack.chat_session_id());
                return;
            }
            if(_unacked) _unacked->ack(ack.user_id(), ack.user_seq());
            // 异步上报 last_ack_seq；失败 → LOG_WARN（DAO 单调推进，下次 ACK 会带更新的 seq 自动 catchup）
            auto channel = _mm_channels->choose(_message_service_name);
            if(!channel) {
                LOG_WARN("UpdateAckSeq: message service 不可达 uid={} seq={}",
                         ack.user_id(), ack.user_seq());
                return;
            }
            MsgStorageService_Stub stub(channel.get());
            auto *closure = new SelfDeleteRpcClosure<UpdateAckSeqReq, UpdateAckSeqRsp>();
            closure->req.set_user_id(ack.user_id());
            closure->req.set_chat_session_id(ack.chat_session_id());
            closure->req.set_user_seq(ack.user_seq());
            std::string uid = ack.user_id();
            uint64_t seq = ack.user_seq();
            closure->on_done = [uid, seq](brpc::Controller *c, const UpdateAckSeqRsp &r) {
                if(c->Failed()) {
                    LOG_WARN("UpdateAckSeq RPC 失败 uid={} seq={}: {}", uid, seq, c->ErrorText());
                } else if(!r.success()) {
                    LOG_WARN("UpdateAckSeq 业务失败 uid={} seq={}: {}", uid, seq, r.errmsg());
                }
            };
            stub.UpdateAckSeq(&closure->cntl, &closure->req, &closure->rsp, closure);
        } else if(notify.notify_type() == NotifyType::CLIENT_HEARTBEAT) {
            const auto &hb = notify.heartbeat();
            _on_heartbeat_resend(hb);
        }
    }

    /* M5: 心跳触发未 ack 重传 —
     *  - peek_due：拿到一批入队超过 max_age 的成熟 user_seq（不删除）
     *  - 异步调 message.GetOfflineMsg(uid, last_user_seq, msg_count)；不阻塞 WS asio 单线程
     *  - 回调里按 message_id->user_seq 映射配对（不依赖列表下标，规避 select_by_ids 跨会话排序）
     *  - bump_score：把这批 score 推到 now，并续期 7 天 TTL，避免老 unacked 整 key 过期消失
     */
    void _on_heartbeat_resend(const ::chatnow::NotifyHeartbeat &hb) {
        if(!_unacked) return;
        const std::string uid = hb.user_id();
        if(uid.empty()) return;
        auto pending = _unacked->peek_due(uid, _resend_batch, _resend_max_age_sec);
        if(pending.empty()) return;
        // 解析 user_seq 数值；构 set 给回调过滤；同时算 last_user_seq 起点
        std::unordered_set<uint64_t> pending_set;
        pending_set.reserve(pending.size());
        uint64_t min_seq = std::numeric_limits<uint64_t>::max();
        for(const auto &s : pending) {
            try {
                uint64_t v = std::stoull(s);
                pending_set.insert(v);
                if(v < min_seq) min_seq = v;
            } catch(...) { LOG_WARN("Heartbeat-补送 非法 user_seq={}", s); }
        }
        if(pending_set.empty()) return;

        // 先查本地缓存
        std::vector<uint64_t> cache_hits;
        std::vector<uint64_t> cache_misses;
        {
            std::lock_guard<std::mutex> lock(_msg_cache_mu);
            for(uint64_t us : pending_set) {
                std::string key = uid + ":" + std::to_string(us);
                if(_msg_cache.find(key) != _msg_cache.end()) {
                    cache_hits.push_back(us);
                } else {
                    cache_misses.push_back(us);
                }
            }
        }

        // 缓存命中：直接 _local_send
        int sent_from_cache = 0;
        for(uint64_t us : cache_hits) {
            std::string key = uid + ":" + std::to_string(us);
            std::string payload;
            {
                std::lock_guard<std::mutex> lock(_msg_cache_mu);
                auto it = _msg_cache.find(key);
                if(it != _msg_cache.end()) payload = (*it->second)->payload;
            }
            if(!payload.empty()) {
                _local_send(uid, payload);
                ++sent_from_cache;
            }
        }

        // 全部命中：跳过 RPC
        if(cache_misses.empty()) {
            if(_unacked) _unacked->bump_score(uid, pending);
            LOG_INFO("Heartbeat-补送 uid={} 取出 {} 条 全部命中缓存 (sent={})",
                     uid, pending.size(), sent_from_cache);
            return;
        }

        // 仅对未命中的走 RPC
        uint64_t min_miss = *std::min_element(cache_misses.begin(), cache_misses.end());
        uint64_t last_user_seq = min_miss > 0 ? min_miss - 1 : 0;
        std::unordered_set<uint64_t> miss_set(cache_misses.begin(), cache_misses.end());

        auto channel = _mm_channels->choose(_message_service_name);
        if(!channel) {
            LOG_WARN("Heartbeat-补送：message 服务不可达 uid={}", uid);
            return;
        }
        MsgStorageService_Stub stub(channel.get());
        auto *closure = new SelfDeleteRpcClosure<GetOfflineMsgReq, GetOfflineMsgRsp>();
        closure->req.set_request_id(uid);
        closure->req.set_user_id(uid);
        closure->req.set_last_message_id(static_cast<int64_t>(last_user_seq));
        closure->req.set_msg_count(static_cast<int32_t>(pending.size()));

        std::string uid_copy = uid;
        auto unacked = _unacked;
        auto pending_copy = pending;
        auto miss_set_copy = std::move(miss_set);
        int sent_cache = sent_from_cache;
        PushServiceImpl *self = this;
        closure->on_done = [self, uid_copy, unacked, pending_copy, miss_set_copy, sent_cache](
            brpc::Controller *c, const GetOfflineMsgRsp &rsp)
        {
            if(c->Failed() || !rsp.success()) {
                LOG_WARN("Heartbeat-补送 GetOfflineMsg 失败 uid={}: {}",
                         uid_copy, c->Failed() ? c->ErrorText() : rsp.errmsg());
                return;
            }
            int sent = sent_cache;
            for(int i = 0; i < rsp.msg_list_size(); ++i) {
                const auto &mi = rsp.msg_list(i);
                if(!mi.has_user_seq()) continue;
                uint64_t us = mi.user_seq();
                if(miss_set_copy.find(us) == miss_set_copy.end()) continue;
                ::chatnow::NotifyMessage notify;
                notify.set_notify_type(NotifyType::CHAT_MESSAGE_NOTIFY);
                notify.mutable_new_message_info()->mutable_message_info()->CopyFrom(mi);
                self->_local_send(uid_copy, notify.SerializeAsString());
                ++sent;
            }
            if(unacked) unacked->bump_score(uid_copy, pending_copy);
            LOG_INFO("Heartbeat-补送 uid={} 取出 {} 条 实际重发 {} 条 (缓存命中 {} 条)",
                     uid_copy, pending_copy.size(), sent, sent_cache);
        };
        stub.GetOfflineMsg(&closure->cntl, &closure->req, &closure->rsp, closure);
    }

    void start_cross_outbox_reaper(const std::string &owner) {
        if(!_cross_outbox || !_mm_channels) {
            LOG_WARN("CrossInstanceOutbox reaper 未启动：outbox / channels 未注入");
            return;
        }
        constexpr int kReapIntervalSec = 5;
        constexpr int kLeaseTtlSec     = 30;
        constexpr int kBatchLimit      = 50;
        _cross_reaper_running.store(true);
        _cross_reaper_owner = owner;
        _cross_reaper_thread = std::thread([this, kReapIntervalSec, kLeaseTtlSec, kBatchLimit]() {
            while(_cross_reaper_running.load()) {
                try {
                    if(!_cross_outbox->try_acquire_reaper_lease(_cross_reaper_owner, kLeaseTtlSec)) {
                        std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                        continue;
                    }
                    auto batch = _cross_outbox->peek(kBatchLimit);
                    if(batch.empty()) {
                        std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                        continue;
                    }
                    LOG_INFO("CrossInstanceOutbox reaper: 取出 {} 条待重试", batch.size());
                    for(const auto &member : batch) _cross_outbox->remove(member);
                    for(const auto &member : batch) {
                        std::string b64, peer;
                        std::vector<std::string> uids;
                        _parse_outbox_member(member, b64, uids, peer);
                        std::string payload = _utils_base64_decode(b64);

                        InternalMessage internal_msg;
                        if(!internal_msg.ParseFromString(payload)) {
                            LOG_ERROR("CrossInstanceOutbox: 反序列化失败，丢弃");
                            continue;
                        }

                        std::unordered_map<std::string, std::vector<std::string>> peer_to_uids;
                        for(const auto &uid : uids) {
                            auto instances = _online_route ? _online_route->instances(uid)
                                                           : std::vector<std::string>{};
                            for(const auto &inst : instances) {
                                if(inst == _instance_id) continue;
                                peer_to_uids[inst].push_back(uid);
                                break;
                            }
                        }

                        NotifyMessage notify_template;
                        notify_template.set_notify_type(NotifyType::CHAT_MESSAGE_NOTIFY);
                        notify_template.mutable_new_message_info()
                            ->mutable_message_info()->CopyFrom(internal_msg.message_info());

                        for(auto &kv : peer_to_uids) {
                            const std::string &p = kv.first;
                            auto channel = _mm_channels->choose(p);
                            if(!channel) {
                                _cross_outbox->enqueue_raw(member,
                                    static_cast<long long>(time(nullptr)) + 5);
                                continue;
                            }
                            PushService_Stub stub(channel.get());
                            auto *closure = new SelfDeleteRpcClosure<PushBatchReq, PushBatchRsp>();
                            closure->req.set_request_id(
                                internal_msg.message_info().client_msg_id());
                            for(const auto &u : kv.second)
                                closure->req.add_user_id_list(u);
                            closure->req.mutable_notify()->CopyFrom(notify_template);
                            for(const auto &up : internal_msg.user_seqs()) {
                                if(std::find(kv.second.begin(), kv.second.end(),
                                             up.user_id()) != kv.second.end()) {
                                    auto *seq = closure->req.add_user_seqs();
                                    seq->set_user_id(up.user_id());
                                    seq->set_user_seq(up.user_seq());
                                }
                            }
                            std::string peer_id = p;
                            std::string member_copy = member;
                            auto outbox_ref = _cross_outbox;
                            closure->on_done = [peer_id, member_copy, outbox_ref](
                                brpc::Controller *c, const PushBatchRsp &) {
                                if(c->Failed()) {
                                    LOG_WARN("CrossInstanceOutbox reaper 重试失败 peer={}: {}",
                                             peer_id, c->ErrorText());
                                    if(outbox_ref) outbox_ref->enqueue_raw(member_copy,
                                        static_cast<long long>(time(nullptr)) + 5);
                                }
                            };
                            stub.PushBatch(&closure->cntl, &closure->req,
                                           &closure->rsp, closure);
                        }
                    }
                } catch(std::exception &e) {
                    LOG_ERROR("CrossInstanceOutbox reaper 异常: {}", e.what());
                }
                std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
            }
            if(_cross_outbox) _cross_outbox->release_reaper_lease(_cross_reaper_owner);
            LOG_INFO("CrossInstanceOutbox reaper 已停止");
        });
    }

    void stop_cross_outbox_reaper() {
        _cross_reaper_running.store(false);
        if(_cross_reaper_thread.joinable()) _cross_reaper_thread.join();
    }

private:
    void _parse_outbox_member(const std::string &member,
                               std::string &b64,
                               std::vector<std::string> &uids,
                               std::string &peer) {
        auto pos_k = member.find("\"k\":\"");
        auto pos_u = member.find("\"u\":[");
        auto pos_p = member.find("\"p\":\"");
        if(pos_k != std::string::npos && pos_u != std::string::npos) {
            b64 = member.substr(pos_k + 5, pos_u - pos_k - 8);
        }
        if(pos_p != std::string::npos) {
            peer = member.substr(pos_p + 5, member.size() - pos_p - 7);
        }
        if(pos_u != std::string::npos) {
            size_t arr_end = member.find(']', pos_u);
            if(arr_end != std::string::npos) {
                std::string arr = member.substr(pos_u + 5, arr_end - pos_u - 5);
                size_t start = 0;
                while((start = arr.find('"', start)) != std::string::npos) {
                    size_t end = arr.find('"', start + 1);
                    if(end == std::string::npos) break;
                    uids.push_back(arr.substr(start + 1, end - start - 1));
                    start = end + 1;
                }
            }
        }
    }

    /* brief: 本实例直接通过 WS 下发；返回送达的连接数
     * M2: per-conn send 串行化 — 取连接关联的 send_mutex 后再 send，
     *     防止 MQ 消费线程 / brpc IO 线程 / WS asio 线程并发 send 同一 conn 撕帧 / crash。
     */
    int _local_send(const std::string &uid, const std::string &payload) {
        auto conns = _connections->connections(uid);
        int sent = 0;
        for(auto &c : conns) {
            try {
                if(!c || c->get_state() != websocketpp::session::state::value::open) continue;
                auto mu = _connections->send_mutex(c);
                if(!mu) continue;  // conn 已被 close handler / reaper 清理
                std::lock_guard<std::mutex> lock(*mu);
                c->send(payload, websocketpp::frame::opcode::value::binary);
                ++sent;
            } catch(std::exception &e) {
                LOG_WARN("WS send 失败 uid={}: {}", uid, e.what());
            }
        }
        return sent;
    }

    static std::string _utils_base64_encode(const std::string &in) {
        static const char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        for(size_t i = 0; i < in.size(); i += 3) {
            unsigned long val = (unsigned char)in[i] << 16;
            if(i + 1 < in.size()) val |= (unsigned char)in[i + 1] << 8;
            if(i + 2 < in.size()) val |= (unsigned char)in[i + 2];
            out += kTbl[(val >> 18) & 0x3F];
            out += kTbl[(val >> 12) & 0x3F];
            out += (i + 1 < in.size()) ? kTbl[(val >> 6) & 0x3F] : '=';
            out += (i + 2 < in.size()) ? kTbl[val & 0x3F] : '=';
        }
        return out;
    }
    static std::string _utils_base64_decode(const std::string &in) {
        static const unsigned char kDec[128] = {
            64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
            64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,52,53,54,55,56,57,58,59,60,61,64,64,64,64,64,64,
            64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
            64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64
        };
        std::string out;
        out.reserve((in.size() / 4) * 3);
        for(size_t i = 0; i < in.size(); i += 4) {
            unsigned long val = 0;
            for(int j = 0; j < 4; ++j) {
                if(in[i+j] != '=') val = (val << 6) | kDec[(unsigned char)in[i+j]];
            }
            out += (char)((val >> 16) & 0xFF);
            if(in[i+2] != '=') out += (char)((val >> 8) & 0xFF);
            if(in[i+3] != '=') out += (char)(val & 0xFF);
        }
        return out;
    }

    Connection::ptr _connections;
    Session::ptr _redis_session;
    Status::ptr _redis_status;
    OnlineRoute::ptr _online_route;
    UnackedPush::ptr _unacked;
    CrossInstanceOutbox::ptr _cross_outbox;
    std::string _instance_id;
    std::string _message_service_name;
    ServiceManager::ptr _mm_channels;
    // M5: 心跳触发重发的可调参数（gflag 注入；默认值在 conf 缺省时使用）
    long _resend_batch        {50};
    long _resend_max_age_sec  {5};
    // CrossInstanceOutbox reaper 状态
    std::atomic<bool> _cross_reaper_running {false};
    std::thread _cross_reaper_thread;
    std::string _cross_reaper_owner;
    // 本地消息缓存（心跳重传优先命中）
    struct MsgCacheEntry {
        std::string key;
        std::string payload;
    };
    std::deque<MsgCacheEntry> _msg_evict_list;
    std::unordered_map<std::string, decltype(_msg_evict_list)::iterator> _msg_cache;
    std::mutex _msg_cache_mu;
    size_t _msg_cache_max_entries = 5000;
};

class PushServer
{
public:
    using ptr = std::shared_ptr<PushServer>;
    PushServer(const Discovery::ptr &disc,
               const Registry::ptr &reg,
               const std::shared_ptr<brpc::Server> &rpc,
               server_t *ws_server,
               const MQClient::ptr &mq_client,
               const Subscriber::ptr &push_subscriber)
        : _service_discover(disc), _reg_client(reg), _rpc_server(rpc), _ws_server(ws_server),
          _mq_client(mq_client), _push_subscriber(push_subscriber) {}
    ~PushServer() = default;

    /* M1: 关停顺序（消除 UAF）—
     *   1) 主动停 MQ 消费：清空 _push_subscriber 与 _mq_client（MQClient 析构关闭 channel + join 线程）
     *      → onPushMessage 不再调度，PushService 不再被外部触发
     *   2) 停 WS：服务端 stop，等待 ws_thread join
     *   3) brpc Stop + Join：等待所有进行中的 PushToUser/PushBatch RPC 真正完成
     *      → 此后 brpc::Server 析构 SERVER_OWNS_SERVICE 才能安全 delete PushServiceImpl
     */
    void start() {
        // RPC + WebSocket 同进程跑；任意一边异常退出立即通知另一边停服
        _ws_thread = std::thread([this]() {
            try {
                _ws_server->run();
                LOG_INFO("Push WS 线程正常退出");
            } catch(std::exception &e) {
                LOG_ERROR("Push WS 线程异常退出: {}", e.what());
            }
            _rpc_server->Stop(0);
        });
        _rpc_server->RunUntilAskedToQuit();
        // 关停顺序：MQ 消费 → WS → brpc Join → brpc::Server 析构 delete impl
        _push_subscriber.reset();
        _mq_client.reset();
        _ws_server->stop();
        if(_ws_thread.joinable()) _ws_thread.join();
        _rpc_server->Join();
        LOG_INFO("Push 关停完成");
    }
private:
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
    server_t *_ws_server;
    MQClient::ptr _mq_client;
    Subscriber::ptr _push_subscriber;
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
        _cross_outbox  = std::make_shared<CrossInstanceOutbox>(_redis);
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

    /* M5: 设置心跳重发参数（应在 make_rpc_object 之前调用） */
    void set_resend_params(int batch, int max_age_sec) {
        _resend_batch = batch;
        _resend_max_age_sec = max_age_sec;
    }
    void set_reaper_owner(const std::string &owner) { _reaper_owner = owner; }

    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads, uint16_t ws_port) {
        if(!_redis) { LOG_ERROR("Push: Redis 未初始化"); abort(); }
        if(!_mm_channels) { LOG_ERROR("Push: 信道管理未初始化"); abort(); }
        _connections = std::make_shared<Connection>();
        _rpc_server = std::make_shared<brpc::Server>();
        _push_service = new PushServiceImpl(
            _connections, _redis_session, _redis_status,
            _online_route, _unacked, _cross_outbox, _instance_id,
            _message_service_name, _mm_channels);
        _push_service->set_resend_params(_resend_batch, _resend_max_age_sec);
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
        // 启动 CrossInstanceOutbox reaper
        std::string owner = _reaper_owner.empty()
            ? std::to_string(::getpid()) : _reaper_owner;
        _push_service->start_cross_outbox_reaper(owner);
        LOG_INFO("Push 服务启动: rpc_port={} ws_port={}", port, ws_port);
    }

    /* M1: build() 把 brpc / MQClient / Subscriber 等的 shared_ptr 全部 move 到 PushServer，
     *     之后 builder 内部持有的全部置空。这样 main 函数销毁 builder 时不会拖住
     *     这些对象的生命周期，PushServer::start() 末尾对它们的 reset 才能真正触发析构，
     *     使 MQClient ev 线程在 brpc::Server 析构（delete PushServiceImpl）之前停下，
     *     消除 review 报告中的 UAF 路径。
     */
    PushServer::ptr build() {
        return std::make_shared<PushServer>(std::move(_service_discover),
                                            std::move(_reg_client),
                                            std::move(_rpc_server),
                                            &_ws_server,
                                            std::move(_mq_client),
                                            std::move(_push_subscriber));
    }
private:
    std::shared_ptr<sw::redis::Redis> _redis;
    Session::ptr _redis_session;
    Status::ptr _redis_status;
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

    // M5: 心跳重发参数
    int _resend_batch       {50};
    int _resend_max_age_sec {5};
    std::string _reaper_owner;

    Connection::ptr _connections;
    server_t _ws_server;
    PushServiceImpl *_push_service {nullptr};
    std::shared_ptr<brpc::Server> _rpc_server;
};

} // namespace chatnow
