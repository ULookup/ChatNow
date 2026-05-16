#pragma once

#include <brpc/server.h>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "infra/etcd.hpp"
#include "mq/channel.hpp"
#include "infra/logger.hpp"
#include "utils/utils.hpp"
#include "dao/data_es.hpp"
#include "dao/data_redis.hpp"
#include "dao/mysql.hpp"
#include "dao/mysql_conversation.hpp"
#include "dao/mysql_conversation_member.hpp"
#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "identity/identity_service.pb.h"
#include "message/message_service.pb.h"
#include "message/message_types.pb.h"
#include "conversation/conversation_service.pb.h"

#include "auth/auth_context.hpp"
#include "auth/forward_auth.hpp"
#include "error/error_codes.hpp"
#include "error/handle_rpc.hpp"
#include "error/service_error.hpp"
#include "log/log_context.hpp"

namespace chatnow {

struct ConversationServiceConfig {
    std::string public_url_prefix;
};

class ConversationServiceImpl : public ::chatnow::conversation::ConversationService
{
public:
    ConversationServiceImpl(const std::shared_ptr<elasticlient::Client> &es_client,
                            const std::shared_ptr<odb::core::database> &mysql_client,
                            const Members::ptr &members_cache,
                            const ServiceManager::ptr &channel_manager,
                            const std::string &identity_service_name,
                            const std::string &media_service_name,
                            const std::string &message_service_name,
                            const ConversationServiceConfig &cfg)
        : _es_conv(std::make_shared<ESConversation>(es_client)),
          _mysql_conv(std::make_shared<ConversationTable>(mysql_client)),
          _mysql_member(std::make_shared<ConversationMemberTable>(mysql_client)),
          _members_cache(members_cache),
          _identity_service_name(identity_service_name),
          _media_service_name(media_service_name),
          _message_service_name(message_service_name),
          _mm_channels(channel_manager),
          _cfg(cfg) {}

    ~ConversationServiceImpl() override = default;

    // —— 18 个 RPC 占位实现（T10–T14 逐步替换） ——
    // T10 已替换：ListConversations / GetConversation / ListMembers /
    //              SearchConversations / GetMemberIds
    // T11 已替换：CreateConversation / UpdateConversation /
    //              DismissConversation / QuitConversation
    // T12 已替换：AddMembers / RemoveMembers / TransferOwner / ChangeMemberRole
    // T13 已替换：SetMute / SetPin / SetVisible / SaveDraft
    #define _PLACEHOLDER_RPC(Method, Req, Rsp) \
        void Method(::google::protobuf::RpcController* base_cntl, \
                    const ::chatnow::conversation::Req* req, \
                    ::chatnow::conversation::Rsp* rsp, \
                    ::google::protobuf::Closure* done) override { \
            brpc::ClosureGuard done_guard(done); \
            (void)base_cntl; \
            rsp->mutable_header()->set_request_id(req->request_id()); \
            rsp->mutable_header()->set_success(false); \
            rsp->mutable_header()->set_error_code( \
                ::chatnow::error::kSystemInternalError); \
            rsp->mutable_header()->set_error_message("not implemented"); \
        }
    _PLACEHOLDER_RPC(MarkRead,           MarkReadReq,           MarkReadRsp)
    #undef _PLACEHOLDER_RPC

    // —— 4 个会话生命周期 RPC（T11） ——

    void CreateConversation(::google::protobuf::RpcController* base_cntl,
                            const ::chatnow::conversation::CreateConversationReq* req,
                            ::chatnow::conversation::CreateConversationRsp* rsp,
                            ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            const auto type_p = req->type();
            using ::chatnow::conversation::ConversationType;
            if (type_p == ConversationType::PRIVATE && req->member_ids_size() != 1)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "private requires exactly 1 peer");
            if (type_p == ConversationType::GROUP && req->member_ids_size() == 0)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "group needs initial members");

            std::string cid;
            if (type_p == ConversationType::PRIVATE) {
                const auto& peer = req->member_ids(0);
                cid = private_id_of_(auth.user_id, peer);
                if (_mysql_conv->exists(cid)) {
                    rsp->mutable_conversation()->set_conversation_id(cid);
                    rsp->mutable_conversation()->set_type(type_p);
                    return;        // 幂等
                }
            } else {
                cid = std::string("g_") + chatnow::uuid();
            }

            ::chatnow::ConversationType ent_type =
                (type_p == ConversationType::PRIVATE) ? ::chatnow::ConversationType::PRIVATE
              : (type_p == ConversationType::GROUP)   ? ::chatnow::ConversationType::GROUP
              :                                          ::chatnow::ConversationType::CHANNEL;

            int member_total = 1 + req->member_ids_size();
            ::chatnow::Conversation ent(
                cid,
                req->has_name() ? req->name() : std::string(),
                ent_type,
                boost::posix_time::microsec_clock::universal_time(),
                member_total,
                ::chatnow::ConversationStatus::NORMAL);
            if (type_p == ConversationType::PRIVATE)
                ent.peer_user_id(req->member_ids(0));
            if (type_p == ConversationType::GROUP) ent.owner_id(auth.user_id);
            // avatar_url 字段语义为 avatar_file_id：直接落库存 file_id，
            // 出口才通过 avatar_url_of_ 拼成 URL。
            if (req->has_avatar_url())
                ent.avatar_id(req->avatar_url());
            if (req->has_description()) ent.description(req->description());

            if(!_mysql_conv->insert(ent))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "insert conversation failed");

            // 成员行（OWNER + 其它）
            auto now = boost::posix_time::microsec_clock::universal_time();
            std::vector<::chatnow::ConversationMember> rows;
            rows.reserve(member_total);
            // caller
            ::chatnow::MemberRole owner_role = (type_p == ConversationType::GROUP)
                ? ::chatnow::MemberRole::OWNER
                : ::chatnow::MemberRole::NORMAL;
            rows.emplace_back(cid, auth.user_id, /*muted=*/false, /*visible=*/true, owner_role, now);
            // peers
            for (int i = 0; i < req->member_ids_size(); ++i) {
                rows.emplace_back(cid, req->member_ids(i),
                                  /*muted=*/false, /*visible=*/true,
                                  ::chatnow::MemberRole::NORMAL, now);
            }
            // 用 append_after_create：批量入群且不重复刷 member_count
            if(!_mysql_member->append_after_create(rows))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "insert members failed");
            invalidate_members_cache_(cid);
            (void)_es_conv->append_data(ent);

            // 回填响应
            auto* out = rsp->mutable_conversation();
            out->set_conversation_id(cid);
            out->set_type(type_p);
            out->set_name(ent.conversation_name());
            if (!ent.avatar_id().empty())
                out->set_avatar_url(avatar_url_of_(ent.avatar_id()));
            out->set_member_count(member_total);
            out->set_status(::chatnow::conversation::ConversationStatus::CONVERSATION_NORMAL);
            out->set_created_at_ms(_to_ms(ent.create_time()));
        });
    }

    void UpdateConversation(::google::protobuf::RpcController* base_cntl,
                            const ::chatnow::conversation::UpdateConversationReq* req,
                            ::chatnow::conversation::UpdateConversationRsp* rsp,
                            ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto role = role_of_(req->conversation_id(), auth.user_id);
            if (role != ::chatnow::MemberRole::OWNER && role != ::chatnow::MemberRole::ADMIN)
                throw ServiceError(::chatnow::error::kConversationNoPermission,
                                   "owner/admin only");
            auto c = _mysql_conv->select(req->conversation_id());
            if (!c)
                throw ServiceError(::chatnow::error::kConversationNotFound,
                                   "not found");
            if (req->has_name())          c->conversation_name(req->name());
            // avatar_url 字段语义为 avatar_file_id：原样落库 file_id
            if (req->has_avatar_url())    c->avatar_id(req->avatar_url());
            if (req->has_description())   c->description(req->description());
            if (req->has_announcement())  c->announcement(req->announcement());
            if(!_mysql_conv->update(c))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "update failed");
            (void)_es_conv->append_data(*c);

            auto* out = rsp->mutable_conversation();
            out->set_conversation_id(c->conversation_id());
            out->set_type(static_cast<::chatnow::conversation::ConversationType>(c->conversation_type()));
            out->set_name(c->conversation_name());
            if (!c->avatar_id().empty())
                out->set_avatar_url(avatar_url_of_(c->avatar_id()));
            if (!c->description().empty()) out->set_description(c->description());
            out->set_member_count(c->member_count());
            out->set_status(static_cast<::chatnow::conversation::ConversationStatus>(c->status()));
            out->set_created_at_ms(_to_ms(c->create_time()));
        });
    }

    void DismissConversation(::google::protobuf::RpcController* base_cntl,
                             const ::chatnow::conversation::DismissConversationReq* req,
                             ::chatnow::conversation::DismissConversationRsp* rsp,
                             ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if (role_of_(req->conversation_id(), auth.user_id) != ::chatnow::MemberRole::OWNER)
                throw ServiceError(::chatnow::error::kConversationNoPermission, "owner only");
            if(!_mysql_conv->update_status(req->conversation_id(),
                                           ::chatnow::ConversationStatus::DISMISSED))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "update_status failed");
            (void)_es_conv->remove(req->conversation_id());
            invalidate_members_cache_(req->conversation_id());
            // 推送 CONVERSATION_DISMISSED_NOTIFY 留待 Push 接入；本期 fail-soft 不推
        });
    }

    void QuitConversation(::google::protobuf::RpcController* base_cntl,
                          const ::chatnow::conversation::QuitConversationReq* req,
                          ::chatnow::conversation::QuitConversationRsp* rsp,
                          ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto role = role_of_(req->conversation_id(), auth.user_id);
            if (role == ::chatnow::MemberRole::OWNER) {
                throw ServiceError(::chatnow::error::kConversationNoPermission,
                                   "owner must transfer first");
            }
            if (!require_member_(req->conversation_id(), auth.user_id))
                throw ServiceError(::chatnow::error::kConversationNotMember,
                                   "not a member");
            if(!_mysql_member->set_quit(req->conversation_id(), auth.user_id))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "set_quit failed");
            invalidate_members_cache_(req->conversation_id());
            // set_quit 内部已经维护 member_count（_update_session_member_count(-1)），
            // 不需要再 _mysql_conv->update。
        });
    }

    // —— 4 个成员管理 RPC（T12） ——

    void AddMembers(::google::protobuf::RpcController* base_cntl,
                    const ::chatnow::conversation::AddMembersReq* req,
                    ::chatnow::conversation::AddMembersRsp* rsp,
                    ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto c = _mysql_conv->select(req->conversation_id());
            if (!c)
                throw ServiceError(::chatnow::error::kConversationNotFound, "not found");
            if (c->conversation_type() != ::chatnow::ConversationType::GROUP)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "only GROUP allows AddMembers");

            auto role = role_of_(req->conversation_id(), auth.user_id);
            if (role != ::chatnow::MemberRole::OWNER && role != ::chatnow::MemberRole::ADMIN)
                throw ServiceError(::chatnow::error::kConversationNoPermission,
                                   "owner/admin only");

            constexpr int kGroupMemberLimit = 500;
            // 注意：实际新增数无法精确预估（已是成员/已 quit/全新），
            // 这里用最严格的上界估计：当前 member_count + 全部待加都视为新成员
            if (c->member_count() + req->member_ids_size() > kGroupMemberLimit)
                throw ServiceError(::chatnow::error::kConversationMemberLimit, "member limit");

            auto now = boost::posix_time::microsec_clock::universal_time();
            for (int i = 0; i < req->member_ids_size(); ++i) {
                const auto& uid = req->member_ids(i);
                auto m = _mysql_member->select_self(req->conversation_id(), uid);
                if (m && !m->is_quit()) continue;     // 已是活跃成员，跳过
                if (m && m->is_quit()) {
                    // 二次入群
                    _mysql_member->rejoin(req->conversation_id(), uid,
                                          ::chatnow::MemberRole::NORMAL,
                                          auth.user_id,
                                          ::chatnow::JoinSource::ADMIN_ADD);
                } else {
                    // 全新入群
                    ::chatnow::ConversationMember row(req->conversation_id(), uid,
                        /*muted=*/false, /*visible=*/true,
                        ::chatnow::MemberRole::NORMAL, now);
                    row.inviter_id(auth.user_id);
                    row.join_source(::chatnow::JoinSource::ADMIN_ADD);
                    _mysql_member->append(row);
                }
            }
            invalidate_members_cache_(req->conversation_id());
        });
    }

    void RemoveMembers(::google::protobuf::RpcController* base_cntl,
                       const ::chatnow::conversation::RemoveMembersReq* req,
                       ::chatnow::conversation::RemoveMembersRsp* rsp,
                       ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto role = role_of_(req->conversation_id(), auth.user_id);
            if (role != ::chatnow::MemberRole::OWNER && role != ::chatnow::MemberRole::ADMIN)
                throw ServiceError(::chatnow::error::kConversationNoPermission,
                                   "owner/admin only");
            int removed = 0;
            for (int i = 0; i < req->member_ids_size(); ++i) {
                const auto& uid = req->member_ids(i);
                if (uid == auth.user_id)
                    throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                       "cannot remove self");
                auto target = _mysql_member->select_self(req->conversation_id(), uid);
                if (!target || target->is_quit()) continue;
                if (target->role() == ::chatnow::MemberRole::OWNER)
                    throw ServiceError(::chatnow::error::kConversationNoPermission,
                                       "owner cannot be removed");
                _mysql_member->set_quit(req->conversation_id(), uid);
                ++removed;
            }
            if (removed > 0) invalidate_members_cache_(req->conversation_id());
        });
    }

    void TransferOwner(::google::protobuf::RpcController* base_cntl,
                       const ::chatnow::conversation::TransferOwnerReq* req,
                       ::chatnow::conversation::TransferOwnerRsp* rsp,
                       ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if (role_of_(req->conversation_id(), auth.user_id) != ::chatnow::MemberRole::OWNER)
                throw ServiceError(::chatnow::error::kConversationNoPermission, "owner only");
            auto target = _mysql_member->select_self(req->conversation_id(), req->new_owner_id());
            if (!target || target->is_quit())
                throw ServiceError(::chatnow::error::kConversationNotMember,
                                   "new owner must be a member");
            _mysql_member->update_role(req->conversation_id(), req->new_owner_id(),
                                       ::chatnow::MemberRole::OWNER);
            _mysql_member->update_role(req->conversation_id(), auth.user_id,
                                       ::chatnow::MemberRole::ADMIN);
            auto c = _mysql_conv->select(req->conversation_id());
            if (c) { c->owner_id(req->new_owner_id()); _mysql_conv->update(c); }
        });
    }

    void ChangeMemberRole(::google::protobuf::RpcController* base_cntl,
                          const ::chatnow::conversation::ChangeMemberRoleReq* req,
                          ::chatnow::conversation::ChangeMemberRoleRsp* rsp,
                          ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if (role_of_(req->conversation_id(), auth.user_id) != ::chatnow::MemberRole::OWNER)
                throw ServiceError(::chatnow::error::kConversationNoPermission, "owner only");
            using ProtoRole = ::chatnow::conversation::MemberRole;
            if (req->role() == ProtoRole::OWNER)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "use TransferOwner to assign OWNER");
            auto target = _mysql_member->select_self(req->conversation_id(), req->target_user_id());
            if (!target || target->is_quit())
                throw ServiceError(::chatnow::error::kConversationNotMember, "target not in conversation");
            if (target->role() == ::chatnow::MemberRole::OWNER)
                throw ServiceError(::chatnow::error::kConversationNoPermission,
                                   "cannot change OWNER's role");
            // proto MemberRole: MEMBER=0, ADMIN=1, OWNER=2
            // ODB MemberRole:    NORMAL=0, ADMIN=1, OWNER=2
            // 数值对应，但 proto MEMBER 与 ODB NORMAL 名字不同
            ::chatnow::MemberRole target_role =
                (req->role() == ProtoRole::ADMIN) ? ::chatnow::MemberRole::ADMIN
                                                  : ::chatnow::MemberRole::NORMAL;
            _mysql_member->update_role(req->conversation_id(), req->target_user_id(), target_role);
        });
    }

    // —— 5 个读取类 RPC 实现（T10） ——

    void ListConversations(::google::protobuf::RpcController* base_cntl,
                           const ::chatnow::conversation::ListConversationsReq* req,
                           ::chatnow::conversation::ListConversationsRsp* rsp,
                           ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            // 1. 取我的活跃会话视图（按 pin_time / max_seq / update_time 排序）
            auto views = _mysql_member->list_ordered_by_user(auth.user_id);

            // 2. PRIVATE 单聊用 peer_user_id 兜底名字 / avatar，先批量取 UserInfo
            std::vector<std::string> peer_uids;
            for (auto &v : views) {
                if (v.peer_user_id) peer_uids.push_back(*v.peer_user_id);
            }
            std::unordered_map<std::string, ::chatnow::common::UserInfo> peer_map;
            (void)fetch_user_infos_(cntl, req->request_id(), peer_uids, peer_map);

            // 3. 转 proto
            for (auto &v : views) {
                if (!v.visible) continue;                         // 隐藏会话不返回
                if (v.status == ConversationStatus::DISMISSED) continue;
                auto* c = rsp->add_conversations();
                c->set_conversation_id(v.conversation_id);
                c->set_type(static_cast<::chatnow::conversation::ConversationType>(v.conversation_type));
                if (v.conversation_name) {
                    c->set_name(*v.conversation_name);
                } else if (v.peer_user_id) {
                    auto it = peer_map.find(*v.peer_user_id);
                    if (it != peer_map.end()) c->set_name(it->second.nickname());
                }
                if (v.avatar_id) c->set_avatar_url(avatar_url_of_(*v.avatar_id));
                c->set_created_at_ms(_to_ms(v.create_time));
                c->set_member_count(v.member_count);
                c->set_status(static_cast<::chatnow::conversation::ConversationStatus>(v.status));

                // last_message：fail-soft（Message 不可达就留空）
                ::chatnow::message::MessagePreview preview;
                if (fetch_last_message_(cntl, req->request_id(), v.conversation_id,
                                        v.last_read_seq, preview)) {
                    c->mutable_last_message()->CopyFrom(preview);
                }

                // self：用 view 字段拼一个临时 ConversationMember 复用 fill_self
                ConversationMember m;
                m.user_id(auth.user_id);
                m.conversation_id(v.conversation_id);
                m.role(v.role);
                m.muted(v.muted);
                m.visible(v.visible);
                if (v.pin_time) m.pin_time(*v.pin_time);
                m.last_read_seq(v.last_read_seq);
                m.last_ack_seq(v.last_ack_seq);
                m.join_time(v.join_time);
                if (v.draft) m.draft(*v.draft);
                fill_self_member_info_(m, v.max_seq, c->mutable_self());
            }
            rsp->mutable_page()->set_has_more(false);
            rsp->mutable_page()->set_total_count(static_cast<int32_t>(rsp->conversations_size()));
        });
    }

    void GetConversation(::google::protobuf::RpcController* base_cntl,
                         const ::chatnow::conversation::GetConversationReq* req,
                         ::chatnow::conversation::GetConversationRsp* rsp,
                         ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto c = _mysql_conv->select(req->conversation_id());
            if (!c)
                throw ServiceError(::chatnow::error::kConversationNotFound,
                                   "conversation not found");
            if (!require_member_(req->conversation_id(), auth.user_id))
                throw ServiceError(::chatnow::error::kConversationNotMember,
                                   "not a member");

            auto* out = rsp->mutable_conversation();
            out->set_conversation_id(c->conversation_id());
            out->set_type(static_cast<::chatnow::conversation::ConversationType>(c->conversation_type()));
            out->set_name(c->conversation_name());
            if (!c->avatar_id().empty()) out->set_avatar_url(avatar_url_of_(c->avatar_id()));
            if (!c->description().empty()) out->set_description(c->description());
            out->set_created_at_ms(_to_ms(c->create_time()));
            out->set_member_count(c->member_count());
            out->set_status(static_cast<::chatnow::conversation::ConversationStatus>(c->status()));

            // last_message：fail-soft
            auto self = _mysql_member->select_self(req->conversation_id(), auth.user_id);
            unsigned long after = self ? self->last_read_seq() : 0UL;
            ::chatnow::message::MessagePreview preview;
            if (fetch_last_message_(cntl, req->request_id(),
                                    req->conversation_id(), after, preview)) {
                out->mutable_last_message()->CopyFrom(preview);
            }
            if (self) fill_self_member_info_(*self, c->max_seq(), out->mutable_self());
        });
    }

    void ListMembers(::google::protobuf::RpcController* base_cntl,
                     const ::chatnow::conversation::ListMembersReq* req,
                     ::chatnow::conversation::ListMembersRsp* rsp,
                     ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if (!require_member_(req->conversation_id(), auth.user_id))
                throw ServiceError(::chatnow::error::kConversationNotMember,
                                   "not a member");
            // DAO 没有 list_active_members(cid)；用 members(cid) + 批量 select(cid, uids)
            // 拼出活跃成员的全行数据（members() 已经过滤 is_quit=true）。
            auto uids = _mysql_member->members(req->conversation_id());
            auto rows = _mysql_member->select(req->conversation_id(), uids);

            std::unordered_map<std::string, ::chatnow::common::UserInfo> umap;
            (void)fetch_user_infos_(cntl, req->request_id(), uids, umap);

            for (auto &m : rows) {
                if (m.is_quit()) continue;            // 防御：批量 select 含已退群行
                auto* item = rsp->add_members();
                auto it = umap.find(m.user_id());
                if (it != umap.end()) item->mutable_user_info()->CopyFrom(it->second);
                else                  item->mutable_user_info()->set_user_id(m.user_id());
                item->set_role(static_cast<::chatnow::conversation::MemberRole>(m.role()));
                item->set_join_time_ms(_to_ms(m.join_time()));
            }
            rsp->mutable_page()->set_has_more(false);
            rsp->mutable_page()->set_total_count(rsp->members_size());
        });
    }

    void SearchConversations(::google::protobuf::RpcController* base_cntl,
                             const ::chatnow::conversation::SearchConversationsReq* req,
                             ::chatnow::conversation::SearchConversationsRsp* rsp,
                             ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto cid_hits = _es_conv->search(req->search_key(), std::nullopt, 50);
            if (cid_hits.empty()) return;
            // 仅返回 caller 是成员的会话
            auto convs = _mysql_conv->select(cid_hits);
            for (auto &c : convs) {
                if (!require_member_(c.conversation_id(), auth.user_id)) continue;
                if (c.status() == ConversationStatus::DISMISSED)         continue;
                auto* out = rsp->add_conversations();
                out->set_conversation_id(c.conversation_id());
                out->set_type(static_cast<::chatnow::conversation::ConversationType>(c.conversation_type()));
                out->set_name(c.conversation_name());
                if (!c.avatar_id().empty()) out->set_avatar_url(avatar_url_of_(c.avatar_id()));
                out->set_member_count(c.member_count());
                out->set_status(static_cast<::chatnow::conversation::ConversationStatus>(c.status()));
            }
        });
    }

    void GetMemberIds(::google::protobuf::RpcController* base_cntl,
                      const ::chatnow::conversation::GetMemberIdsReq* req,
                      ::chatnow::conversation::GetMemberIdsRsp* rsp,
                      ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            // 1. 优先走 Redis 缓存（list 返回 vector<string>）
            auto cached = _members_cache->list(req->conversation_id());
            if (!cached.empty()) {
                if (auth.user_id != "__system__"
                    && std::find(cached.begin(), cached.end(), auth.user_id) == cached.end())
                    throw ServiceError(::chatnow::error::kConversationNotMember,
                                       "not a member");
                for (auto &uid : cached) rsp->add_member_ids(uid);
                return;
            }
            // 2. 缓存未命中：查 DB + warm
            auto uids = _mysql_member->members(req->conversation_id());
            _members_cache->warm(req->conversation_id(), uids);

            if (auth.user_id != "__system__"
                && std::find(uids.begin(), uids.end(), auth.user_id) == uids.end())
                throw ServiceError(::chatnow::error::kConversationNotMember,
                                   "not a member");
            for (auto &uid : uids) rsp->add_member_ids(uid);
        });
    }

    // —— 4 个自身偏好 RPC（T13） ——

    void SetMute(::google::protobuf::RpcController* base_cntl,
                 const ::chatnow::conversation::SetMuteReq* req,
                 ::chatnow::conversation::SetMuteRsp* rsp,
                 ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto m = _mysql_member->select_self(req->conversation_id(), auth.user_id);
            if (!m || m->is_quit())
                throw ServiceError(::chatnow::error::kConversationNotMember, "not member");
            m->muted(req->mute());
            if(!_mysql_member->update(m))
                throw ServiceError(::chatnow::error::kSystemInternalError, "update failed");
            auto c = _mysql_conv->select(req->conversation_id());
            fill_self_member_info_(*m, c ? c->max_seq() : 0, rsp->mutable_self());
        });
    }

    void SetPin(::google::protobuf::RpcController* base_cntl,
                const ::chatnow::conversation::SetPinReq* req,
                ::chatnow::conversation::SetPinRsp* rsp,
                ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto m = _mysql_member->select_self(req->conversation_id(), auth.user_id);
            if (!m || m->is_quit())
                throw ServiceError(::chatnow::error::kConversationNotMember, "not member");
            if (req->pin())
                m->pin_time(boost::posix_time::microsec_clock::universal_time());
            else
                m->unpin();
            if(!_mysql_member->update(m))
                throw ServiceError(::chatnow::error::kSystemInternalError, "update failed");
            auto c = _mysql_conv->select(req->conversation_id());
            fill_self_member_info_(*m, c ? c->max_seq() : 0, rsp->mutable_self());
        });
    }

    void SetVisible(::google::protobuf::RpcController* base_cntl,
                    const ::chatnow::conversation::SetVisibleReq* req,
                    ::chatnow::conversation::SetVisibleRsp* rsp,
                    ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto m = _mysql_member->select_self(req->conversation_id(), auth.user_id);
            if (!m || m->is_quit())
                throw ServiceError(::chatnow::error::kConversationNotMember, "not member");
            m->visible(req->visible());
            if(!_mysql_member->update(m))
                throw ServiceError(::chatnow::error::kSystemInternalError, "update failed");
            auto c = _mysql_conv->select(req->conversation_id());
            fill_self_member_info_(*m, c ? c->max_seq() : 0, rsp->mutable_self());
        });
    }

    void SaveDraft(::google::protobuf::RpcController* base_cntl,
                   const ::chatnow::conversation::SaveDraftReq* req,
                   ::chatnow::conversation::SaveDraftRsp* rsp,
                   ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if(!_mysql_member->update_draft(req->conversation_id(), auth.user_id, req->draft()))
                throw ServiceError(::chatnow::error::kConversationNotMember, "not member or draft update failed");
            auto m = _mysql_member->select_self(req->conversation_id(), auth.user_id);
            auto c = _mysql_conv->select(req->conversation_id());
            if (m) fill_self_member_info_(*m, c ? c->max_seq() : 0, rsp->mutable_self());
        });
    }

private:
    // —— 内部辅助（T10 引入；T11–T14 复用） ——

    static int64_t _to_ms(const boost::posix_time::ptime &t) {
        static const boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
        if (t.is_not_a_date_time()) return 0;
        return (t - epoch).total_milliseconds();
    }

    /* brief: 批量取 UserInfo（identity 不可达时 fail-soft：返回 false，调用方按需降级） */
    bool fetch_user_infos_(brpc::Controller* in_cntl, const std::string& rid,
                           const std::vector<std::string>& uids,
                           std::unordered_map<std::string, ::chatnow::common::UserInfo>& out)
    {
        if (uids.empty()) return true;
        auto channel = _mm_channels->choose(_identity_service_name);
        if (!channel) {
            LOG_ERROR("rid={} identity 子服务节点不可达 svc={}", rid, _identity_service_name);
            return false;
        }
        ::chatnow::identity::IdentityService_Stub stub(channel.get());
        ::chatnow::identity::GetMultiUserInfoReq  ireq;
        ::chatnow::identity::GetMultiUserInfoRsp  irsp;
        ireq.set_request_id(rid);
        for (auto &u : uids) ireq.add_users_id(u);
        brpc::Controller out_cntl;
        ::chatnow::auth::forward_auth_metadata(in_cntl, &out_cntl);
        stub.GetMultiUserInfo(&out_cntl, &ireq, &irsp, nullptr);
        if (out_cntl.Failed()) {
            LOG_ERROR("rid={} GetMultiUserInfo brpc 失败: {}", rid, out_cntl.ErrorText());
            return false;
        }
        if (!irsp.header().success()) {
            LOG_ERROR("rid={} GetMultiUserInfo 业务失败: code={} msg={}",
                      rid, irsp.header().error_code(), irsp.header().error_message());
            return false;
        }
        for (auto &kv : irsp.users_info()) out.insert({kv.first, kv.second});
        return true;
    }

    /* brief: 调 MessageService.SyncMessages(after_seq, limit=1) 取 last_message。
     *  - fail-soft：失败 / 不可达 / 0 条返回 false，调用方留空 last_message
     *  - Message 服务未迁前编译期会因 SyncMessages stub 缺生成代码失败 —
     *    spec §7 验收 #2 已记录为预期阻塞 */
    bool fetch_last_message_(brpc::Controller* in_cntl, const std::string& rid,
                             const std::string& cid, unsigned long after_seq,
                             ::chatnow::message::MessagePreview& out)
    {
        auto channel = _mm_channels->choose(_message_service_name);
        if (!channel) return false;
        ::chatnow::message::MessageService_Stub stub(channel.get());
        ::chatnow::message::SyncMessagesReq  mreq;
        ::chatnow::message::SyncMessagesRsp  mrsp;
        mreq.set_request_id(rid);
        mreq.set_conversation_id(cid);
        mreq.set_after_seq(after_seq);
        mreq.set_limit(1);
        brpc::Controller out_cntl;
        ::chatnow::auth::forward_auth_metadata(in_cntl, &out_cntl);
        stub.SyncMessages(&out_cntl, &mreq, &mrsp, nullptr);
        if (out_cntl.Failed() || !mrsp.header().success() || mrsp.messages_size() == 0) {
            return false;
        }
        // Message → MessagePreview 字段映射（content_preview 由 Message 服务生成，
        // 本服务这一路只返回结构化字段，preview 文本留空由前端兜底）。
        const auto& m = mrsp.messages(0);
        out.set_message_id(m.message_id());
        out.set_sender_id(m.sender_id());
        out.set_message_type(m.content().type());
        out.set_sent_at_ms(m.created_at_ms());
        out.set_status(m.status());
        return true;
    }

    /* brief: 是否为活跃成员（已退群视为非成员） */
    bool require_member_(const std::string& cid, const std::string& uid) {
        auto m = _mysql_member->select_self(cid, uid);
        return m && !m->is_quit();
    }

    /* brief: 取角色；已退群 / 不存在统一返回 NORMAL（调用方再做权限判定） */
    MemberRole role_of_(const std::string& cid, const std::string& uid) {
        auto m = _mysql_member->select_self(cid, uid);
        if (!m || m->is_quit()) return MemberRole::NORMAL;
        return m->role();
    }

    /* brief: 失效成员缓存（DAO 写后调用，确保下次 GetMemberIds 重建）
     *  - Members 缓存 API 是 invalidate(ssid)，不是 del() */
    void invalidate_members_cache_(const std::string& cid) {
        _members_cache->invalidate(cid);
    }

    /* brief: 单聊会话 ID 规则：p_<min(uid,pid)>_<max(uid,pid)> */
    static std::string private_id_of_(const std::string& a, const std::string& b) {
        const std::string& lo = (a < b) ? a : b;
        const std::string& hi = (a < b) ? b : a;
        return std::string("p_") + lo + "_" + hi;
    }

    /* brief: avatar_file_id → public URL */
    std::string avatar_url_of_(const std::string& file_id) const {
        return _cfg.public_url_prefix + "/group_avatar/" + file_id;
    }

    /* brief: ConversationMember + max_seq → SelfMemberInfo */
    void fill_self_member_info_(const ConversationMember& m,
                                unsigned long max_seq,
                                ::chatnow::conversation::SelfMemberInfo* out)
    {
        out->set_role(static_cast<::chatnow::conversation::MemberRole>(m.role()));
        out->set_joined_at_ms(_to_ms(m.join_time()));
        out->set_is_muted(m.muted());
        out->set_is_pinned(m.is_pinned());
        if (m.is_pinned()) out->set_pin_time_ms(_to_ms(m.pin_time()));
        out->set_is_visible(m.visible());
        out->set_last_read_seq(m.last_read_seq());
        out->set_unread_count(max_seq > m.last_read_seq()
                              ? max_seq - m.last_read_seq() : 0);
        if (m.has_draft()) out->set_draft(m.draft());
    }

private:
    ESConversation::ptr           _es_conv;
    ConversationTable::ptr        _mysql_conv;
    ConversationMemberTable::ptr  _mysql_member;
    Members::ptr                  _members_cache;
    std::string                   _identity_service_name;
    std::string                   _media_service_name;
    std::string                   _message_service_name;
    ServiceManager::ptr           _mm_channels;
    ConversationServiceConfig     _cfg;
};

class ConversationServer
{
public:
    using ptr = std::shared_ptr<ConversationServer>;
    ConversationServer(const Discovery::ptr &service_discover,
                       const Registry::ptr &reg_client,
                       const std::shared_ptr<odb::core::database> &mysql_client,
                       const std::shared_ptr<brpc::Server> &server)
        : _service_discover(service_discover),
          _reg_client(reg_client),
          _mysql_client(mysql_client),
          _rpc_server(server) {}

    ~ConversationServer() = default;
    void start() { _rpc_server->RunUntilAskedToQuit(); }

private:
    Discovery::ptr _service_discover;
    Registry::ptr  _reg_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<brpc::Server>        _rpc_server;
};

class ConversationServerBuilder
{
public:
    void make_es_object(const std::vector<std::string> host_list) {
        _es_client = ESClientFactory::create(host_list);
    }
    void make_redis_object(const std::string &host, uint16_t port, int db,
                           bool keep_alive, int pool_size) {
        _redis_client  = RedisClientFactory::create(host, port, db, keep_alive, pool_size);
        _members_cache = std::make_shared<Members>(_redis_client);
    }
    void make_mysql_object(const std::string &user, const std::string &password,
                           const std::string &host, const std::string &dbname,
                           const std::string &cset, uint16_t port, int pool)
    {
        _mysql_client = ODBFactory::create(user, password, host, dbname, cset, port, pool);
    }
    void make_discovery_object(const std::string &reg_host,
                               const std::string &base_service_name,
                               const std::string &identity_service_name,
                               const std::string &media_service_name,
                               const std::string &message_service_name)
    {
        _identity_service_name = identity_service_name;
        _media_service_name    = media_service_name;
        _message_service_name  = message_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_identity_service_name);
        _mm_channels->declared(_media_service_name);
        _mm_channels->declared(_message_service_name);

        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    void make_config(const std::string &public_url_prefix) {
        _cfg.public_url_prefix = public_url_prefix;
    }
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        if(!_es_client)    { LOG_ERROR("还未初始化ES模块");      abort(); }
        if(!_mysql_client) { LOG_ERROR("还未初始化MySQL模块");   abort(); }
        if(!_mm_channels)  { LOG_ERROR("还未初始化信道管理模块"); abort(); }
        if(!_members_cache){ LOG_ERROR("还未初始化Members缓存");  abort(); }

        auto *impl = new ConversationServiceImpl(
            _es_client, _mysql_client, _members_cache, _mm_channels,
            _identity_service_name, _media_service_name, _message_service_name, _cfg);
        if(_rpc_server->AddService(impl, brpc::ServiceOwnership::SERVER_OWNS_SERVICE) == -1) {
            LOG_ERROR("添加RPC服务失败!"); abort();
        }
        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads      = num_threads;
        if(_rpc_server->Start(port, &options) == -1) {
            LOG_ERROR("服务启动失败!"); abort();
        }
    }
    void make_registry_object(const std::string &reg_host,
                              const std::string &service_name,
                              const std::string &access_host)
    {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    ConversationServer::ptr build() {
        if(!_service_discover) { LOG_ERROR("还未初始化服务发现模块"); abort(); }
        if(!_reg_client)       { LOG_ERROR("还未初始化服务注册模块"); abort(); }
        if(!_rpc_server)       { LOG_ERROR("还未初始化RPC模块");      abort(); }
        return std::make_shared<ConversationServer>(_service_discover, _reg_client,
                                                    _mysql_client, _rpc_server);
    }

private:
    std::shared_ptr<elasticlient::Client>   _es_client;
    std::shared_ptr<sw::redis::Redis>       _redis_client;
    Members::ptr                            _members_cache;
    std::shared_ptr<odb::core::database>    _mysql_client;
    Discovery::ptr                          _service_discover;
    Registry::ptr                           _reg_client;
    ServiceManager::ptr                     _mm_channels;
    std::string                             _identity_service_name;
    std::string                             _media_service_name;
    std::string                             _message_service_name;
    ConversationServiceConfig               _cfg;
    std::shared_ptr<brpc::Server>           _rpc_server;
};

} // namespace chatnow
