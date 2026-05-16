#pragma once

/**
 * MessageServiceImpl —— chatnow::message::MessageService 实现
 * ---
 * 见 docs/superpowers/specs/2026-05-16-message-service-migration-design.md
 */

#include <brpc/server.h>
#include <brpc/channel.h>
#include <brpc/closure_guard.h>
#include <google/protobuf/service.h>

#include "message/message_service.pb.h"
#include "message/message_internal.pb.h"
#include "message/message_types.pb.h"
#include "push/notify.pb.h"
#include "push/push_service.pb.h"
#include "identity/identity_service.pb.h"
#include "media/media_service.pb.h"

#include "common/auth/auth_context.hpp"
#include "common/auth/forward_auth.hpp"
#include "common/error/handle_rpc.hpp"
#include "common/error/service_error.hpp"
#include "common/error/error_codes.hpp"
#include "common/log/log_context.hpp"
#include "common/infra/logger.hpp"
#include "common/infra/etcd.hpp"
#include "common/infra/channels.hpp"
#include "common/dao/mysql_message.hpp"
#include "common/dao/mysql_user_timeline.hpp"
#include "common/dao/mysql_conversation_member.hpp"
#include "common/dao/mysql_message_reaction.hpp"
#include "common/dao/mysql_message_pin.hpp"
#include "common/dao/data_es.hpp"
#include "common/dao/data_redis.hpp"
#include "common/mq/rabbitmq.hpp"

#include <chrono>
#include <map>
#include <set>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>

namespace chatnow::message {

inline constexpr int64_t kRecallTimeoutMs = 120 * 1000;
inline constexpr int     kPinLimit        = 10;
inline constexpr int     kMaxLimit        = 100;
inline constexpr int     kMaxSearchLimit  = 50;
inline constexpr int     kMaxEmojiBytes   = 16;
inline constexpr const char *kSystemUserId = "__system__";

class MessageServiceImpl : public chatnow::message::MessageService {
public:
    MessageServiceImpl(const std::string &identity_service_name,
                       const std::string &media_service_name,
                       const ServiceManager::ptr &mm_channels,
                       const MessageTable::ptr &mysql_msg,
                       const UserTimelineTable::ptr &mysql_user_timeline,
                       const ConversationMemberTable::ptr &mysql_member,
                       const MessageReactionTable::ptr &mysql_reaction,
                       const MessagePinTable::ptr &mysql_pin,
                       const ESMessage::ptr &es_msg,
                       const SeqGen::ptr &seq_gen,
                       const Publisher::ptr &push_publisher,
                       const PushOutbox::ptr &push_outbox,
                       const Publisher::ptr &es_publisher,
                       const ESOutbox::ptr &es_outbox)
        : _identity_service_name(identity_service_name),
          _media_service_name(media_service_name),
          _mm_channels(mm_channels),
          _mysql_msg(mysql_msg),
          _mysql_user_timeline(mysql_user_timeline),
          _mysql_member(mysql_member),
          _mysql_reaction(mysql_reaction),
          _mysql_pin(mysql_pin),
          _es_msg(es_msg),
          _seq_gen(seq_gen),
          _push_publisher(push_publisher),
          _push_outbox(push_outbox),
          _es_publisher(es_publisher),
          _es_outbox(es_outbox) {}

    ~MessageServiceImpl() override = default;

    // ====== 15 个 RPC：T10 仅占位 throw kSystemInternalError；T11-T15 替换 ======

    void GetHistory(::google::protobuf::RpcController* base_cntl,
                    const GetHistoryReq* req, GetHistoryRsp* rsp,
                    ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if (req->limit() <= 0 || req->limit() > kMaxLimit)
                throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                              "limit out of range");
            if (req->before_seq() == 0)
                throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                              "before_seq must > 0");
            require_member_(req->conversation_id(), auth.user_id);

            auto db_msgs = _mysql_msg->select_history(req->conversation_id(),
                                                      req->before_seq(),
                                                      req->limit() + 1);
            bool has_more = (static_cast<int>(db_msgs.size()) > req->limit());
            if (has_more) db_msgs.pop_back();

            std::vector<unsigned long> mids;
            mids.reserve(db_msgs.size());
            for (auto &m : db_msgs) {
                auto *out = rsp->add_messages();
                convert_db_message_to_proto_(m, out);
                mids.push_back(m.message_id());
            }
            fill_reactions_for_messages_(mids, auth.user_id, rsp->mutable_messages());
            fill_pin_flag_for_messages_(req->conversation_id(), mids, rsp->mutable_messages());

            rsp->set_has_more(has_more);
        });
    }

    void SyncMessages(::google::protobuf::RpcController* base_cntl,
                      const SyncMessagesReq* req, SyncMessagesRsp* rsp,
                      ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if (req->limit() <= 0 || req->limit() > kMaxLimit)
                throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                              "limit out of range");
            require_member_(req->conversation_id(), auth.user_id);

            auto db_msgs = _mysql_msg->select_after(req->conversation_id(),
                                                     req->after_seq(),
                                                     req->limit() + 1);
            bool has_more = (static_cast<int>(db_msgs.size()) > req->limit());
            if (has_more) db_msgs.pop_back();

            std::vector<unsigned long> mids;
            mids.reserve(db_msgs.size());
            for (auto &m : db_msgs) {
                auto *out = rsp->add_messages();
                convert_db_message_to_proto_(m, out);
                mids.push_back(m.message_id());
            }
            fill_reactions_for_messages_(mids, auth.user_id, rsp->mutable_messages());
            fill_pin_flag_for_messages_(req->conversation_id(), mids, rsp->mutable_messages());

            rsp->set_has_more(has_more);
            rsp->set_latest_seq(_mysql_msg->select_max_seq_by_conversation(req->conversation_id()));
        });
    }

    void GetMessagesById(::google::protobuf::RpcController* base_cntl,
                         const GetMessagesByIdReq* req, GetMessagesByIdRsp* rsp,
                         ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if (req->message_ids_size() == 0 || req->message_ids_size() > kMaxLimit)
                throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                              "message_ids size out of range");
            std::vector<unsigned long> mids;
            for (int i = 0; i < req->message_ids_size(); ++i)
                mids.push_back(static_cast<unsigned long>(req->message_ids(i)));
            auto db_msgs = _mysql_msg->select_by_ids(mids);

            for (auto &m : db_msgs) {
                auto self = _mysql_member->select_self(m.session_id(), auth.user_id);
                if (!self || self->is_quit()) continue;
                auto *out = rsp->add_messages();
                convert_db_message_to_proto_(m, out);
            }
            std::vector<unsigned long> out_mids;
            for (auto &m : rsp->messages()) out_mids.push_back(m.message_id());
            fill_reactions_by_mids_(out_mids, auth.user_id, rsp->mutable_messages());
        });
    }

    void SearchMessages(::google::protobuf::RpcController* base_cntl,
                        const SearchMessagesReq* req, SearchMessagesRsp* rsp,
                        ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if (req->keyword().empty())
                throw ::chatnow::ServiceError(::chatnow::error::kMessageContentInvalid,
                                              "keyword empty");
            if (req->limit() <= 0 || req->limit() > kMaxSearchLimit)
                throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                              "limit out of range");
            require_member_(req->conversation_id(), auth.user_id);

            auto es_results = _es_msg->search(req->keyword(), req->conversation_id(),
                                              req->limit());
            for (auto &m : es_results) {
                auto *out = rsp->add_messages();
                convert_db_message_to_proto_(m, out);
            }
            rsp->set_has_more(false);
            rsp->set_next_cursor("");
        });
    }

    void RecallMessage(::google::protobuf::RpcController* base_cntl,
                       const RecallMessageReq* req, RecallMessageRsp* rsp,
                       ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto msg = _mysql_msg->select_by_id(static_cast<unsigned long>(req->message_id()));
            if (!msg)
                throw ::chatnow::ServiceError(::chatnow::error::kMessageNotFound, "mid not found");
            if (msg->session_id() != req->conversation_id())
                throw ::chatnow::ServiceError(::chatnow::error::kMessageNotFound, "cid mismatch");
            if (msg->status() == MessageStatus::REVOKED)
                throw ::chatnow::ServiceError(::chatnow::error::kMessageAlreadyRecalled,
                                              "already recalled");
            if (msg->status() == MessageStatus::DELETED)
                throw ::chatnow::ServiceError(::chatnow::error::kMessageNotFound, "deleted");

            auto role = conv_role_(req->conversation_id(), auth.user_id);
            bool is_admin = (role == MemberRole::OWNER || role == MemberRole::ADMIN);
            bool is_self  = (msg->user_id() == auth.user_id);
            namespace pt = boost::posix_time;
            pt::ptime epoch(boost::gregorian::date(1970, 1, 1));
            int64_t created_ms = (msg->create_time() - epoch).total_milliseconds();
            int64_t age_ms = now_ms_() - created_ms;
            if (!is_admin) {
                if (!is_self)
                    throw ::chatnow::ServiceError(::chatnow::error::kConversationNoPermission,
                                                  "not msg author");
                if (age_ms >= kRecallTimeoutMs)
                    throw ::chatnow::ServiceError(::chatnow::error::kMessageRecallTimeout,
                                                  "exceed 120s window");
            }

            if (!_mysql_msg->update_status_to_recalled(static_cast<unsigned long>(req->message_id())))
                throw ::chatnow::ServiceError(::chatnow::error::kMessageAlreadyRecalled,
                                              "race lost or not recallable");

            publish_recalled_notify_(req->conversation_id(), req->message_id());
        });
    }

    void AddReaction(::google::protobuf::RpcController* base_cntl,
                     const AddReactionReq* req, AddReactionRsp* rsp,
                     ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "AddReaction not implemented");
        });
    }

    void RemoveReaction(::google::protobuf::RpcController* base_cntl,
                        const RemoveReactionReq* req, RemoveReactionRsp* rsp,
                        ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "RemoveReaction not implemented");
        });
    }

    void GetReactions(::google::protobuf::RpcController* base_cntl,
                      const GetReactionsReq* req, GetReactionsRsp* rsp,
                      ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "GetReactions not implemented");
        });
    }

    void PinMessage(::google::protobuf::RpcController* base_cntl,
                    const PinMessageReq* req, PinMessageRsp* rsp,
                    ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "PinMessage not implemented");
        });
    }

    void UnpinMessage(::google::protobuf::RpcController* base_cntl,
                      const UnpinMessageReq* req, UnpinMessageRsp* rsp,
                      ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "UnpinMessage not implemented");
        });
    }

    void ListPinnedMessages(::google::protobuf::RpcController* base_cntl,
                            const ListPinnedReq* req, ListPinnedRsp* rsp,
                            ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "ListPinnedMessages not implemented");
        });
    }

    void DeleteMessages(::google::protobuf::RpcController* base_cntl,
                        const DeleteMessagesReq* req, DeleteMessagesRsp* rsp,
                        ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "DeleteMessages not implemented");
        });
    }

    void ClearConversation(::google::protobuf::RpcController* base_cntl,
                           const ClearConversationReq* req, ClearConversationRsp* rsp,
                           ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "ClearConversation not implemented");
        });
    }

    void SelectByClientMsgId(::google::protobuf::RpcController* base_cntl,
                             const SelectByClientMsgIdReq* req, SelectByClientMsgIdRsp* rsp,
                             ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "SelectByClientMsgId not implemented");
        });
    }

    void UpdateReadAck(::google::protobuf::RpcController* base_cntl,
                       const UpdateReadAckReq* req, UpdateReadAckRsp* rsp,
                       ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "UpdateReadAck not implemented");
        });
    }

    // ====== MQ consumer：T10 占位，T18 替换为真实实现 ======

    ConsumeAction onDBMessage(const char *body, size_t sz, bool redelivered) {
        (void)body; (void)sz; (void)redelivered;
        LOG_WARN("onDBMessage placeholder; will be replaced in T18");
        return ConsumeAction::Ack;
    }

    ConsumeAction onESIndexMessage(const char *body, size_t sz, bool redelivered) {
        (void)body; (void)sz; (void)redelivered;
        LOG_WARN("onESIndexMessage placeholder; will be replaced in T18");
        return ConsumeAction::Ack;
    }

private:
    // ====== 权限校验 ======

    /* 要求 auth.user_id 是 cid 的成员，否则抛 kConversationNotMember */
    void require_member_(const std::string &cid, const std::string &uid) {
        if (uid == kSystemUserId) return;
        auto self = _mysql_member->select_self(cid, uid);
        if (!self || self->is_quit()) {
            throw ::chatnow::ServiceError(
                ::chatnow::error::kConversationNotMember,
                "user not member of conversation");
        }
    }

    /* 查 uid 在 cid 中的角色（OWNER/ADMIN/NORMAL）；非成员抛异常 */
    MemberRole conv_role_(const std::string &cid, const std::string &uid) {
        if (uid == kSystemUserId) return MemberRole::OWNER;
        auto self = _mysql_member->select_self(cid, uid);
        if (!self || self->is_quit()) {
            throw ::chatnow::ServiceError(
                ::chatnow::error::kConversationNotMember,
                "user not member of conversation");
        }
        return self->role();
    }

    // ====== 数据转换 ======

    void convert_db_message_to_proto_(const Message &db, chatnow::message::Message *out) {
        out->set_message_id(db.message_id());
        out->set_conversation_id(db.session_id());
        out->set_sender_id(db.user_id());
        out->set_seq_id(db.seq_id());
        if (!db.client_msg_id().empty()) out->set_client_msg_id(db.client_msg_id());
        out->set_status(static_cast<chatnow::message::MessageStatus>(static_cast<int>(db.status())));
        out->set_message_type(static_cast<chatnow::message::MessageType>(static_cast<int>(db.message_type())));

        namespace pt = boost::posix_time;
        pt::ptime epoch(boost::gregorian::date(1970, 1, 1));
        int64_t ms = (db.create_time() - epoch).total_milliseconds();
        if (ms < 0) ms = 0;
        out->set_created_at_ms(ms);

        // content 反序列化：根据 message_type 填充 oneof Content
        auto *content = out->mutable_content();
        switch (db.message_type()) {
            case MessageType::TEXT:
            case MessageType::STRING:
                content->set_type(chatnow::message::TEXT);
                content->mutable_text()->set_text(db.content());
                break;
            case MessageType::IMAGE:
                content->set_type(chatnow::message::IMAGE);
                content->mutable_image()->set_file_id(db.file_id());
                break;
            case MessageType::FILE:
                content->set_type(chatnow::message::FILE);
                content->mutable_file()->set_file_id(db.file_id());
                content->mutable_file()->set_file_name(db.file_name());
                if (db.file_size() > 0) content->mutable_file()->set_file_size(db.file_size());
                break;
            case MessageType::SPEECH:
                content->set_type(chatnow::message::AUDIO);
                content->mutable_audio()->set_file_id(db.file_id());
                break;
            default:
                content->set_type(chatnow::message::TEXT);
                content->mutable_text()->set_text(db.content());
                break;
        }
    }

    // ====== reaction / pin 辅助 ======

    void fill_reactions_for_messages_(const std::vector<unsigned long> &mids,
                                       const std::string &caller_uid,
                                       ::google::protobuf::RepeatedPtrField<chatnow::message::Message> *msgs) {
        if (mids.empty() || !_mysql_reaction) return;
        auto rows = _mysql_reaction->select_by_messages(mids);
        std::map<unsigned long, std::map<std::string, std::vector<std::string>>> grouped;
        for (auto &r : rows) grouped[r.message_id][r.emoji].push_back(r.user_id);
        for (auto &m : *msgs) {
            auto it = grouped.find(m.message_id());
            if (it == grouped.end()) continue;
            for (auto &[emoji, uids] : it->second) {
                auto *g = m.add_reactions();
                g->set_emoji(emoji);
                g->set_count(static_cast<int>(uids.size()));
                bool self = false;
                for (size_t i = 0; i < uids.size(); ++i) {
                    if (uids[i] == caller_uid) self = true;
                    if (i < 3) g->add_recent_user_ids(uids[i]);
                }
                g->set_self_reacted(self);
            }
        }
    }

    void fill_reactions_by_mids_(const std::vector<unsigned long> &mids,
                                  const std::string &caller_uid,
                                  ::google::protobuf::RepeatedPtrField<chatnow::message::Message> *msgs) {
        fill_reactions_for_messages_(mids, caller_uid, msgs);
    }

    void fill_pin_flag_for_messages_(const std::string &cid,
                                      const std::vector<unsigned long> &mids,
                                      ::google::protobuf::RepeatedPtrField<chatnow::message::Message> *msgs) {
        if (mids.empty() || !_mysql_pin) return;
        auto pinned = _mysql_pin->list_pinned_in(cid, mids);
        std::set<unsigned long> pset(pinned.begin(), pinned.end());
        for (auto &m : *msgs) m.set_is_pinned(pset.count(m.message_id()) > 0);
    }

    int64_t now_ms_() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // ====== notify 发布辅助（fail-soft） ======

    void publish_recalled_notify_(const std::string &cid, int64_t mid) {
        if (!_push_publisher) return;
        chatnow::push::NotifyMessage nm;
        nm.set_notify_type(chatnow::push::MESSAGE_RECALLED_NOTIFY);
        nm.mutable_message_recalled()->set_conversation_id(cid);
        nm.mutable_message_recalled()->set_message_id(mid);
        std::string payload = nm.SerializeAsString();
        auto outbox = _push_outbox;
        try {
            _push_publisher->publish_confirm(payload, {},
                [payload, outbox](PublishStatus st, const std::string &err) {
                    if (st != PublishStatus::Acked && outbox)
                        outbox->enqueue(payload, static_cast<long long>(time(nullptr)));
                });
        } catch (std::exception &e) {
            LOG_ERROR("publish_recalled_notify exception cid={} mid={}: {}", cid, mid, e.what());
            if (outbox) outbox->enqueue(payload, static_cast<long long>(time(nullptr)));
        }
    }

    void publish_reaction_notify_(const std::string &target_uid,
                                   const std::string &cid, int64_t mid,
                                   const std::string &actor_uid,
                                   const std::string &emoji, bool added) {
        if (!_push_publisher || target_uid == actor_uid) return;
        chatnow::push::NotifyMessage nm;
        nm.set_notify_type(chatnow::push::REACTION_CHANGED_NOTIFY);
        auto *r = nm.mutable_reaction_changed();
        r->set_conversation_id(cid);
        r->set_message_id(mid);
        r->set_actor_user_id(actor_uid);
        r->set_emoji(emoji);
        r->set_added(added);
        // Reaction 仅推消息发送者，直接调 PushService.PushToUser RPC
        try {
            auto channel = _mm_channels ? _mm_channels->choose("push_service") : nullptr;
            if (!channel) { LOG_WARN("push channel unavailable; skip reaction notify"); return; }
            chatnow::push::PushService_Stub stub(channel.get());
            chatnow::push::PushToUserReq preq;
            preq.set_request_id("reaction-notify");
            preq.set_user_id(target_uid);
            *preq.mutable_notify() = nm;
            chatnow::push::PushToUserRsp prsp;
            brpc::Controller pcntl;
            pcntl.set_timeout_ms(500);
            stub.PushToUser(&pcntl, &preq, &prsp, brpc::DoNothing());
        } catch (std::exception &e) {
            LOG_ERROR("publish_reaction_notify exception target={} mid={}: {}", target_uid, mid, e.what());
        }
    }

    void publish_pin_notify_(const std::string &cid, int64_t mid,
                              const std::string &actor_uid, bool is_pinned) {
        if (!_push_publisher) return;
        chatnow::push::NotifyMessage nm;
        nm.set_notify_type(chatnow::push::PIN_CHANGED_NOTIFY);
        auto *p = nm.mutable_pin_changed();
        p->set_conversation_id(cid);
        p->set_message_id(mid);
        p->set_actor_user_id(actor_uid);
        p->set_is_pinned(is_pinned);
        std::string payload = nm.SerializeAsString();
        auto outbox = _push_outbox;
        try {
            _push_publisher->publish_confirm(payload, {},
                [payload, outbox](PublishStatus st, const std::string &err) {
                    if (st != PublishStatus::Acked && outbox)
                        outbox->enqueue(payload, static_cast<long long>(time(nullptr)));
                });
        } catch (std::exception &e) {
            LOG_ERROR("publish_pin_notify exception cid={} mid={}: {}", cid, mid, e.what());
            if (outbox) outbox->enqueue(payload, static_cast<long long>(time(nullptr)));
        }
    }

    // ====== 注入字段 ======

    std::string _identity_service_name;
    std::string _media_service_name;
    ServiceManager::ptr _mm_channels;

    MessageTable::ptr _mysql_msg;
    UserTimelineTable::ptr _mysql_user_timeline;
    ConversationMemberTable::ptr _mysql_member;
    MessageReactionTable::ptr _mysql_reaction;
    MessagePinTable::ptr _mysql_pin;

    ESMessage::ptr _es_msg;
    SeqGen::ptr _seq_gen;

    Publisher::ptr _push_publisher;
    PushOutbox::ptr _push_outbox;
    Publisher::ptr _es_publisher;
    ESOutbox::ptr _es_outbox;
};

// ===== Server 与 Builder =====

class MessageServer {
public:
    using ptr = std::shared_ptr<MessageServer>;
    MessageServer(const std::shared_ptr<brpc::Server> &server,
                  MessageServiceImpl *impl,
                  const Registry::ptr &registry,
                  const MQClient::ptr &mq_client)
        : _rpc_server(server), _service_impl(impl),
          _registry(registry), _mq_client(mq_client) {}

    ~MessageServer() {
        if (_rpc_server) { _rpc_server->Stop(0); _rpc_server->Join(); }
        _mq_client.reset();
    }

    void start() { _rpc_server->RunUntilAskedToQuit(); }

private:
    std::shared_ptr<brpc::Server> _rpc_server;
    MessageServiceImpl *_service_impl {nullptr};
    Registry::ptr _registry;
    MQClient::ptr _mq_client;
};

class MessageServerBuilder {
public:
    void make_mysql_object(const std::string &user, const std::string &pwd,
                           const std::string &host, const std::string &db,
                           const std::string &cset, uint16_t port, int pool) {
        _odb_db = ODBFactory::create(user, pwd, host, db, cset, port, pool);
        _mysql_msg = std::make_shared<MessageTable>(_odb_db);
        _mysql_user_timeline = std::make_shared<UserTimelineTable>(_odb_db);
        _mysql_member = std::make_shared<ConversationMemberTable>(_odb_db);
        _mysql_reaction = std::make_shared<MessageReactionTable>(_odb_db);
        _mysql_pin = std::make_shared<MessagePinTable>(_odb_db);
    }
    void make_redis_object(const std::string &host, uint16_t port, int db,
                           bool keep_alive, int pool_size) {
        _redis = RedisClientFactory::create(host, port, db, keep_alive, pool_size);
        _seq_gen = std::make_shared<SeqGen>(_redis);
        _push_outbox = std::make_shared<PushOutbox>(_redis);
        _es_outbox = std::make_shared<ESOutbox>(_redis);
    }
    void make_es_object(const std::vector<std::string> &hosts) {
        _es_client = ESClientFactory::create(hosts);
        _es_msg = std::make_shared<ESMessage>(_es_client);
    }
    void make_mq_object(const std::string &user, const std::string &pwd,
                        const std::string &host,
                        const std::string &exchange_name,
                        const std::string &queue_name_db,
                        const std::string &queue_name_es,
                        const std::string &binding_key_db,
                        const std::string &binding_key_es) {
        if (exchange_name.empty()) {
            LOG_ERROR("Message MQ exchange 不能为空");
            abort();
        }
        std::string amqp_url = "amqp://" + user + ":" + pwd + "@" + host + ":5672/";
        _mq_client = std::make_shared<MQClient>(amqp_url);
        _db_queue_settings = {
            .exchange = exchange_name,
            .exchange_type = chatnow::FANOUT,
            .queue = queue_name_db,
            .binding_key = binding_key_db
        };
        _es_queue_settings = {
            .exchange = exchange_name,
            .exchange_type = chatnow::FANOUT,
            .queue = queue_name_es,
            .binding_key = binding_key_es
        };
        auto dummy_cb = [](const char*, size_t, bool) -> chatnow::ConsumeAction {
            return chatnow::ConsumeAction::Ack;
        };
        _subscriber_db = chatnow::MQFactory::create<chatnow::Subscriber>(
            _mq_client, _db_queue_settings, dummy_cb);
        _subscriber_es = chatnow::MQFactory::create<chatnow::Subscriber>(
            _mq_client, _es_queue_settings, dummy_cb);
    }
    void make_discovery_object(const std::string &reg_host,
                               const std::string &base_dir,
                               const std::string &identity_service_name,
                               const std::string &media_service_name) {
        _identity_service_name = identity_service_name;
        _media_service_name = media_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_identity_service_name);
        _mm_channels->declared(_media_service_name);
        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        _service_discovery = std::make_shared<Discovery>(reg_host, base_dir, put_cb, del_cb);
    }
    void make_registry_object(const std::string &reg_host,
                              const std::string &service_name,
                              const std::string &access_host) {
        _registry = std::make_shared<Registry>(reg_host);
        _registry->registry(service_name, access_host);
    }
    void make_push_publisher(const std::string &exchange,
                             const std::string &queue,
                             const std::string &binding_key) {
        if (!_mq_client) {
            LOG_WARN("MQ 未初始化，跳过 push publisher");
            return;
        }
        _push_settings = {
            .exchange = exchange,
            .exchange_type = chatnow::DIRECT,
            .queue = queue,
            .binding_key = binding_key
        };
        _push_publisher = std::make_shared<Publisher>(_mq_client, _push_settings);
    }
    void make_es_publisher(const std::string &exchange,
                           const std::string &queue,
                           const std::string &binding_key) {
        if (!_mq_client) {
            LOG_WARN("MQ 未初始化，跳过 ES publisher");
            return;
        }
        _es_pub_settings = {
            .exchange = exchange,
            .exchange_type = chatnow::DIRECT,
            .queue = queue,
            .binding_key = binding_key
        };
        _es_publisher = std::make_shared<Publisher>(_mq_client, _es_pub_settings);
    }
    void make_es_index_subscriber(const std::string &exchange,
                                   const std::string &queue,
                                   const std::string &binding_key) {
        if (!_mq_client) {
            LOG_WARN("MQ 未初始化，跳过 ES index subscriber");
            return;
        }
        _es_index_settings = {
            .exchange = exchange,
            .exchange_type = chatnow::DIRECT,
            .queue = queue,
            .binding_key = binding_key
        };
        auto dummy_cb = [](const char*, size_t, bool) -> chatnow::ConsumeAction {
            return chatnow::ConsumeAction::Ack;
        };
        _subscriber_es_index = chatnow::MQFactory::create<chatnow::Subscriber>(
            _mq_client, _es_index_settings, dummy_cb);
    }
    void set_reaper_owner(const std::string &owner) { _reaper_owner = owner; }

    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        MessageServiceImpl *impl = new MessageServiceImpl(
            _identity_service_name, _media_service_name, _mm_channels,
            _mysql_msg, _mysql_user_timeline, _mysql_member,
            _mysql_reaction, _mysql_pin, _es_msg, _seq_gen,
            _push_publisher, _push_outbox, _es_publisher, _es_outbox);
        _service_impl = impl;
        int ret = _rpc_server->AddService(
            impl, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
        if (ret == -1) { LOG_ERROR("AddService failed"); abort(); }
        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads = num_threads;
        if (_rpc_server->Start(port, &options) != 0) {
            LOG_ERROR("brpc Start failed"); abort();
        }
        backfill_seq_from_db_();

        // MQ subscribe
        auto callback_db_inner = std::bind(&MessageServiceImpl::onDBMessage,
            impl, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
        chatnow::MessageCallbackWithHeaders callback_db =
            [callback_db_inner](const char* body, size_t sz, bool redeliv,
                                const std::map<std::string, std::string>& headers)
            -> chatnow::ConsumeAction {
            std::string _trace_id = ::chatnow::mq::mq_extract_trace_id(headers);
            ::chatnow::log::LogContext::set(_trace_id, "", "");
            struct _Scope { ~_Scope() { ::chatnow::log::LogContext::clear(); } } _scope;
            return callback_db_inner(body, sz, redeliv);
        };
        _subscriber_db->consume(std::move(callback_db));

        auto callback_es_inner = std::bind(&MessageServiceImpl::onESIndexMessage,
            impl, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
        chatnow::MessageCallbackWithHeaders callback_es =
            [callback_es_inner](const char* body, size_t sz, bool redeliv,
                                const std::map<std::string, std::string>& headers)
            -> chatnow::ConsumeAction {
            std::string _trace_id = ::chatnow::mq::mq_extract_trace_id(headers);
            ::chatnow::log::LogContext::set(_trace_id, "", "");
            struct _Scope { ~_Scope() { ::chatnow::log::LogContext::clear(); } } _scope;
            return callback_es_inner(body, sz, redeliv);
        };
        _subscriber_es->consume(std::move(callback_es));

        LOG_INFO("MQ 订阅完成，消息服务启动！");
    }

    MessageServer::ptr build() {
        return std::make_shared<MessageServer>(
            _rpc_server, _service_impl, _registry, _mq_client);
    }

private:
    void backfill_seq_from_db_() {
        if (!_seq_gen || !_odb_db) {
            LOG_WARN("SeqGen / MySQL 未初始化，跳过 seq 回填");
            return;
        }
        LOG_INFO("开始从 DB 回填 seq 到 Redis...");
        auto msg_table = std::make_shared<MessageTable>(_odb_db);
        auto timeline_table = std::make_shared<UserTimelineTable>(_odb_db);

        auto session_seqs = msg_table->select_max_seq_by_session();
        for (const auto &[ssid, max_seq] : session_seqs) {
            if (max_seq > 0) _seq_gen->backfill_session(ssid, max_seq + 1);
        }
        LOG_INFO("回填 session_seq 完成: {} 个会话", session_seqs.size());

        auto user_seqs = timeline_table->select_max_user_seq();
        for (const auto &[uid, max_seq] : user_seqs) {
            if (max_seq > 0) _seq_gen->backfill_user(uid, max_seq + 1);
        }
        LOG_INFO("回填 user_seq 完成: {} 个用户", user_seqs.size());
    }

private:
    std::shared_ptr<odb::core::database> _odb_db;
    std::shared_ptr<sw::redis::Redis> _redis;
    std::shared_ptr<elasticlient::Client> _es_client;
    MQClient::ptr _mq_client;
    Registry::ptr _registry;
    Discovery::ptr _service_discovery;

    MessageTable::ptr _mysql_msg;
    UserTimelineTable::ptr _mysql_user_timeline;
    ConversationMemberTable::ptr _mysql_member;
    MessageReactionTable::ptr _mysql_reaction;
    MessagePinTable::ptr _mysql_pin;
    ESMessage::ptr _es_msg;
    SeqGen::ptr _seq_gen;
    PushOutbox::ptr _push_outbox;
    ESOutbox::ptr _es_outbox;
    Publisher::ptr _push_publisher;
    Publisher::ptr _es_publisher;

    declare_settings _db_queue_settings;
    declare_settings _es_queue_settings;
    declare_settings _push_settings;
    declare_settings _es_pub_settings;
    declare_settings _es_index_settings;
    Subscriber::ptr _subscriber_db;
    Subscriber::ptr _subscriber_es;
    Subscriber::ptr _subscriber_es_index;

    ServiceManager::ptr _mm_channels;
    std::string _identity_service_name;
    std::string _media_service_name;
    std::string _reaper_owner;

    std::shared_ptr<brpc::Server> _rpc_server;
    MessageServiceImpl *_service_impl {nullptr};
};

} // namespace chatnow::message
