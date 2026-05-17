#pragma once

#include <brpc/server.h>
#include <butil/logging.h>
#include "httplib.h"

#include "dao/data_redis.hpp"
#include "infra/etcd.hpp"
#include "infra/logger.hpp"
#include "mq/channel.hpp"
#include "log/log_context.hpp"
#include "gateway_auth.hpp"
#include "gateway_trace.hpp"
#include "auth/jwt_codec.hpp"
#include "auth/jwt_store.hpp"
#include "auth/auth_config_loader.hpp"
#include "utils/brpc_closure.hpp"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"

#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "identity/identity_service.pb.h"
#include "relationship/relationship_service.pb.h"
#include "conversation/conversation_service.pb.h"
#include "message/message_types.pb.h"
#include "message/message_service.pb.h"
#include "media/media_service.pb.h"
#include "presence/presence_service.pb.h"
#include "push/notify.pb.h"
#include "push/push_service.pb.h"
#include "transmite/transmite_service.pb.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace chatnow {

enum class GatewayAuth { WHITELISTED, JWT_REQUIRED };

struct GatewayRoute {
    std::string path;
    std::string service_name;
    GatewayAuth auth = GatewayAuth::JWT_REQUIRED;
    int timeout_ms = 3000;
    std::function<void(const httplib::Request&, httplib::Response&,
                       const ::chatnow::gateway::AuthInfo&,
                       const ServiceManager::ptr&, int, brpc::Controller&)> handler;
};

class GatewayServer {
public:
    using ptr = std::shared_ptr<GatewayServer>;

    GatewayServer(int http_port,
                  const ServiceManager::ptr &channels,
                  const std::shared_ptr<::chatnow::auth::JwtCodec> &jwt_codec,
                  const std::shared_ptr<::chatnow::auth::JwtStore> &jwt_store,
                  const std::string &identity_service_name,
                  const std::string &relationship_service_name,
                  const std::string &conversation_service_name,
                  const std::string &message_service_name,
                  const std::string &transmite_service_name,
                  const std::string &media_service_name,
                  const std::string &presence_service_name,
                  const std::string &push_service_name)
        : _jwt_codec(jwt_codec), _jwt_store(jwt_store),
          _channels(channels), _http_port(http_port),
          _identity_svc(identity_service_name),
          _relationship_svc(relationship_service_name),
          _conversation_svc(conversation_service_name),
          _message_svc(message_service_name),
          _transmite_svc(transmite_service_name),
          _media_svc(media_service_name),
          _presence_svc(presence_service_name),
          _push_svc(push_service_name)
    {
        register_routes();
    }

    void start() {
        LOG_INFO("Gateway 启动: http_port={}", _http_port);
        if (!_http_server.listen("0.0.0.0", _http_port)) {
            LOG_ERROR("HTTP 服务器监听失败 port={}", _http_port);
            abort();
        }
    }

private:
    // ====== 泛型转发器 ======

    template<typename Stub, typename Req, typename Rsp, typename Method>
    void forward(const httplib::Request& httpreq, httplib::Response& httpres,
                 const ::chatnow::gateway::AuthInfo& auth, const std::string& svc_name,
                 int timeout_ms, Method method)
    {
        Req pb_req;
        Rsp pb_rsp;
        auto write_err = [&pb_rsp, &httpres](int32_t code, const std::string& msg) {
            auto* h = pb_rsp.mutable_header();
            h->set_success(false);
            h->set_error_code(code);
            h->set_error_message(msg);
            httpres.set_content(pb_rsp.SerializeAsString(), "application/x-protobuf");
        };

        if (!pb_req.ParseFromString(httpreq.body)) {
            LOG_ERROR("Gateway 反序列化失败 path={}", httpreq.path);
            return write_err(::chatnow::error::kSystemInvalidArgument, "parse request body failed");
        }

        auto ch = _channels->choose(svc_name);
        if (!ch) {
            return write_err(::chatnow::error::kSystemUnavailable, "no backend available");
        }

        Stub stub(ch.get());
        brpc::Controller cntl;
        cntl.set_timeout_ms(timeout_ms);
        ::chatnow::gateway::apply_auth_to_brpc(httpreq, cntl, auth);
        (stub.*method)(&cntl, &pb_req, &pb_rsp, nullptr);

        if (cntl.Failed()) {
            int32_t code = (cntl.ErrorCode() == brpc::ERPCTIMEDOUT)
                               ? ::chatnow::error::kSystemTimeout
                               : ::chatnow::error::kSystemUnavailable;
            return write_err(code, cntl.ErrorText());
        }
        httpres.set_content(pb_rsp.SerializeAsString(), "application/x-protobuf");
    }

    // ====== 路由注册快捷模板 ======

    template<typename Stub, typename Req, typename Rsp, typename Method>
    void route(const std::string& path, const std::string& svc_name,
               GatewayAuth auth, Method method, int timeout_ms = 3000)
    {
        GatewayRoute r;
        r.path = path;
        r.service_name = svc_name;
        r.auth = auth;
        r.timeout_ms = timeout_ms;
        r.handler = [this, svc_name, method](const httplib::Request& req, httplib::Response& res,
                                              const ::chatnow::gateway::AuthInfo& a,
                                              const ServiceManager::ptr& /*ch*/, int to,
                                              brpc::Controller& /*cntl*/) {
            forward<Stub, Req, Rsp>(req, res, a, svc_name, to, method);
        };
        _routes.push_back(r);
    }

    // ====== 统一 HTTP 入口 ======

    void handle_request(const httplib::Request& req, httplib::Response& res) {
        ::chatnow::gateway::LogContextScope _trace_scope;

        // 查路由表
        const GatewayRoute* matched = nullptr;
        for (auto& r : _routes) {
            if (req.path == r.path) { matched = &r; break; }
        }
        if (!matched) {
            res.status = 404;
            res.set_content("{\"error\":\"not found\"}", "application/json");
            return;
        }

        // JWT 鉴权
        ::chatnow::gateway::AuthInfo a;
        bool whitelisted = (matched->auth == GatewayAuth::WHITELISTED);
        if (!::chatnow::gateway::jwt_authenticate(req, res, _jwt_codec, _jwt_store,
                                                   whitelisted, a)) {
            return;  // 401 已写
        }

        // 写 X-Trace-Id 响应头
        brpc::Controller dummy_cntl;
        std::string trace_id = ::chatnow::gateway::gateway_setup_trace(
            req, dummy_cntl, a.user_id, a.device_id);
        res.set_header("X-Trace-Id", trace_id);

        // 转发
        matched->handler(req, res, a, _channels, matched->timeout_ms, dummy_cntl);
    }

    // ====== 路由注册 ======

    void register_routes();

    // ====== Push 通知辅助 ======

    void push_notify(const std::string& target_uid, const ::chatnow::push::NotifyMessage& notify);

    // ====== 成员变量 ======

    httplib::Server _http_server;
    std::vector<GatewayRoute> _routes;

    std::shared_ptr<::chatnow::auth::JwtCodec> _jwt_codec;
    std::shared_ptr<::chatnow::auth::JwtStore> _jwt_store;
    ServiceManager::ptr _channels;
    int _http_port;

    std::string _identity_svc;
    std::string _relationship_svc;
    std::string _conversation_svc;
    std::string _message_svc;
    std::string _transmite_svc;
    std::string _media_svc;
    std::string _presence_svc;
    std::string _push_svc;
};

// ====== register_routes 实现 ======

inline void GatewayServer::register_routes() {
    namespace id = ::chatnow::identity;
    namespace rel = ::chatnow::relationship;
    namespace conv = ::chatnow::conversation;
    namespace msg = ::chatnow::message;
    namespace tx = ::chatnow::transmite;
    namespace med = ::chatnow::media;
    namespace pres = ::chatnow::presence;

    // ====== Identity (白名单) ======
    route<id::IdentityService_Stub, id::RegisterReq, id::RegisterRsp>(
        "/service/identity/register", _identity_svc, GatewayAuth::WHITELISTED,
        &id::IdentityService_Stub::Register);
    route<id::IdentityService_Stub, id::LoginReq, id::LoginRsp>(
        "/service/identity/login", _identity_svc, GatewayAuth::WHITELISTED,
        &id::IdentityService_Stub::Login);
    route<id::IdentityService_Stub, id::SendVerifyCodeReq, id::SendVerifyCodeRsp>(
        "/service/identity/send_verify_code", _identity_svc, GatewayAuth::WHITELISTED,
        &id::IdentityService_Stub::SendVerifyCode);
    route<id::IdentityService_Stub, id::RefreshTokenReq, id::RefreshTokenRsp>(
        "/service/identity/refresh_token", _identity_svc, GatewayAuth::WHITELISTED,
        &id::IdentityService_Stub::RefreshToken);

    // ====== Identity (需鉴权) ======
    route<id::IdentityService_Stub, id::LogoutReq, id::LogoutRsp>(
        "/service/identity/logout", _identity_svc, GatewayAuth::JWT_REQUIRED,
        &id::IdentityService_Stub::Logout);
    route<id::IdentityService_Stub, id::GetProfileReq, id::GetProfileRsp>(
        "/service/identity/get_profile", _identity_svc, GatewayAuth::JWT_REQUIRED,
        &id::IdentityService_Stub::GetProfile);
    route<id::IdentityService_Stub, id::UpdateProfileReq, id::UpdateProfileRsp>(
        "/service/identity/update_profile", _identity_svc, GatewayAuth::JWT_REQUIRED,
        &id::IdentityService_Stub::UpdateProfile);
    route<id::IdentityService_Stub, id::SearchUsersReq, id::SearchUsersRsp>(
        "/service/identity/search_users", _identity_svc, GatewayAuth::JWT_REQUIRED,
        &id::IdentityService_Stub::SearchUsers);
    route<id::IdentityService_Stub, id::GetMultiUserInfoReq, id::GetMultiUserInfoRsp>(
        "/service/identity/get_multi_info", _identity_svc, GatewayAuth::JWT_REQUIRED,
        &id::IdentityService_Stub::GetMultiUserInfo);

    // ====== Relationship ======
    route<rel::RelationshipService_Stub, rel::ListFriendsReq, rel::ListFriendsRsp>(
        "/service/relationship/list_friends", _relationship_svc, GatewayAuth::JWT_REQUIRED,
        &rel::RelationshipService_Stub::ListFriends);
    route<rel::RelationshipService_Stub, rel::SendFriendReq, rel::SendFriendRsp>(
        "/service/relationship/send_friend_request", _relationship_svc, GatewayAuth::JWT_REQUIRED,
        &rel::RelationshipService_Stub::SendFriendRequest);
    route<rel::RelationshipService_Stub, rel::HandleFriendReq, rel::HandleFriendRsp>(
        "/service/relationship/handle_friend_request", _relationship_svc, GatewayAuth::JWT_REQUIRED,
        &rel::RelationshipService_Stub::HandleFriendRequest);
    route<rel::RelationshipService_Stub, rel::RemoveFriendReq, rel::RemoveFriendRsp>(
        "/service/relationship/remove_friend", _relationship_svc, GatewayAuth::JWT_REQUIRED,
        &rel::RelationshipService_Stub::RemoveFriend);
    route<rel::RelationshipService_Stub, rel::SearchFriendsReq, rel::SearchFriendsRsp>(
        "/service/relationship/search_friends", _relationship_svc, GatewayAuth::JWT_REQUIRED,
        &rel::RelationshipService_Stub::SearchFriends);
    route<rel::RelationshipService_Stub, rel::BlockUserReq, rel::BlockUserRsp>(
        "/service/relationship/block_user", _relationship_svc, GatewayAuth::JWT_REQUIRED,
        &rel::RelationshipService_Stub::BlockUser);
    route<rel::RelationshipService_Stub, rel::UnblockUserReq, rel::UnblockUserRsp>(
        "/service/relationship/unblock_user", _relationship_svc, GatewayAuth::JWT_REQUIRED,
        &rel::RelationshipService_Stub::UnblockUser);
    route<rel::RelationshipService_Stub, rel::ListBlockedReq, rel::ListBlockedRsp>(
        "/service/relationship/list_blocked", _relationship_svc, GatewayAuth::JWT_REQUIRED,
        &rel::RelationshipService_Stub::ListBlockedUsers);
    route<rel::RelationshipService_Stub, rel::ListPendingReq, rel::ListPendingRsp>(
        "/service/relationship/list_pending", _relationship_svc, GatewayAuth::JWT_REQUIRED,
        &rel::RelationshipService_Stub::ListPendingRequests);

    // ====== Conversation ======
    route<conv::ConversationService_Stub, conv::ListConversationsReq, conv::ListConversationsRsp>(
        "/service/conversation/list", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::ListConversations);
    route<conv::ConversationService_Stub, conv::GetConversationReq, conv::GetConversationRsp>(
        "/service/conversation/get", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::GetConversation);
    route<conv::ConversationService_Stub, conv::CreateConversationReq, conv::CreateConversationRsp>(
        "/service/conversation/create", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::CreateConversation);
    route<conv::ConversationService_Stub, conv::UpdateConversationReq, conv::UpdateConversationRsp>(
        "/service/conversation/update", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::UpdateConversation);
    route<conv::ConversationService_Stub, conv::DismissConversationReq, conv::DismissConversationRsp>(
        "/service/conversation/dismiss", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::DismissConversation);
    route<conv::ConversationService_Stub, conv::AddMembersReq, conv::AddMembersRsp>(
        "/service/conversation/add_members", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::AddMembers);
    route<conv::ConversationService_Stub, conv::RemoveMembersReq, conv::RemoveMembersRsp>(
        "/service/conversation/remove_members", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::RemoveMembers);
    route<conv::ConversationService_Stub, conv::TransferOwnerReq, conv::TransferOwnerRsp>(
        "/service/conversation/transfer_owner", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::TransferOwner);
    route<conv::ConversationService_Stub, conv::ChangeMemberRoleReq, conv::ChangeMemberRoleRsp>(
        "/service/conversation/change_role", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::ChangeMemberRole);
    route<conv::ConversationService_Stub, conv::ListMembersReq, conv::ListMembersRsp>(
        "/service/conversation/list_members", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::ListMembers);
    route<conv::ConversationService_Stub, conv::SetMuteReq, conv::SetMuteRsp>(
        "/service/conversation/set_mute", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::SetMute);
    route<conv::ConversationService_Stub, conv::SetPinReq, conv::SetPinRsp>(
        "/service/conversation/set_pin", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::SetPin);
    route<conv::ConversationService_Stub, conv::SetVisibleReq, conv::SetVisibleRsp>(
        "/service/conversation/set_visible", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::SetVisible);
    route<conv::ConversationService_Stub, conv::QuitConversationReq, conv::QuitConversationRsp>(
        "/service/conversation/quit", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::QuitConversation);
    route<conv::ConversationService_Stub, conv::MarkReadReq, conv::MarkReadRsp>(
        "/service/conversation/mark_read", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::MarkRead);
    route<conv::ConversationService_Stub, conv::SaveDraftReq, conv::SaveDraftRsp>(
        "/service/conversation/save_draft", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::SaveDraft);
    route<conv::ConversationService_Stub, conv::SearchConversationsReq, conv::SearchConversationsRsp>(
        "/service/conversation/search", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::SearchConversations);
    route<conv::ConversationService_Stub, conv::GetMemberIdsReq, conv::GetMemberIdsRsp>(
        "/service/conversation/get_member_ids", _conversation_svc, GatewayAuth::JWT_REQUIRED,
        &conv::ConversationService_Stub::GetMemberIds);

    // ====== Message ======
    route<msg::MessageService_Stub, msg::SyncMessagesReq, msg::SyncMessagesRsp>(
        "/service/message/sync", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::SyncMessages, 10000);
    route<msg::MessageService_Stub, msg::GetHistoryReq, msg::GetHistoryRsp>(
        "/service/message/get_history", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::GetHistory);
    route<msg::MessageService_Stub, msg::GetMessagesByIdReq, msg::GetMessagesByIdRsp>(
        "/service/message/get_by_id", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::GetMessagesById);
    route<msg::MessageService_Stub, msg::SearchMessagesReq, msg::SearchMessagesRsp>(
        "/service/message/search", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::SearchMessages);
    route<msg::MessageService_Stub, msg::RecallMessageReq, msg::RecallMessageRsp>(
        "/service/message/recall", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::RecallMessage);
    route<msg::MessageService_Stub, msg::AddReactionReq, msg::AddReactionRsp>(
        "/service/message/add_reaction", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::AddReaction);
    route<msg::MessageService_Stub, msg::RemoveReactionReq, msg::RemoveReactionRsp>(
        "/service/message/remove_reaction", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::RemoveReaction);
    route<msg::MessageService_Stub, msg::GetReactionsReq, msg::GetReactionsRsp>(
        "/service/message/get_reactions", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::GetReactions);
    route<msg::MessageService_Stub, msg::PinMessageReq, msg::PinMessageRsp>(
        "/service/message/pin", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::PinMessage);
    route<msg::MessageService_Stub, msg::UnpinMessageReq, msg::UnpinMessageRsp>(
        "/service/message/unpin", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::UnpinMessage);
    route<msg::MessageService_Stub, msg::ListPinnedReq, msg::ListPinnedRsp>(
        "/service/message/list_pinned", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::ListPinnedMessages);
    route<msg::MessageService_Stub, msg::DeleteMessagesReq, msg::DeleteMessagesRsp>(
        "/service/message/delete", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::DeleteMessages);
    route<msg::MessageService_Stub, msg::ClearConversationReq, msg::ClearConversationRsp>(
        "/service/message/clear", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::ClearConversation);

    // ====== Transmite ======
    route<tx::MsgTransmitService_Stub, tx::SendMessageReq, tx::SendMessageRsp>(
        "/service/transmite/send", _transmite_svc, GatewayAuth::JWT_REQUIRED,
        &tx::MsgTransmitService_Stub::SendMessage, 1000);

    // ====== Media ======
    route<med::MediaService_Stub, med::ApplyUploadReq, med::ApplyUploadRsp>(
        "/service/media/apply_upload", _media_svc, GatewayAuth::JWT_REQUIRED,
        &med::MediaService_Stub::ApplyUpload);
    route<med::MediaService_Stub, med::CompleteUploadReq, med::CompleteUploadRsp>(
        "/service/media/complete_upload", _media_svc, GatewayAuth::JWT_REQUIRED,
        &med::MediaService_Stub::CompleteUpload);
    route<med::MediaService_Stub, med::ApplyDownloadReq, med::ApplyDownloadRsp>(
        "/service/media/apply_download", _media_svc, GatewayAuth::JWT_REQUIRED,
        &med::MediaService_Stub::ApplyDownload);
    route<med::MediaService_Stub, med::GetFileInfoReq, med::GetFileInfoRsp>(
        "/service/media/get_file_info", _media_svc, GatewayAuth::JWT_REQUIRED,
        &med::MediaService_Stub::GetFileInfo);
    route<med::MediaService_Stub, med::SpeechRecognitionReq, med::SpeechRecognitionRsp>(
        "/service/media/speech_recognition", _media_svc, GatewayAuth::JWT_REQUIRED,
        &med::MediaService_Stub::SpeechRecognition);

    // ====== Presence ======
    route<pres::PresenceService_Stub, pres::GetPresenceReq, pres::GetPresenceRsp>(
        "/service/presence/get", _presence_svc, GatewayAuth::JWT_REQUIRED,
        &pres::PresenceService_Stub::GetPresence);
    route<pres::PresenceService_Stub, pres::BatchGetPresenceReq, pres::BatchGetPresenceRsp>(
        "/service/presence/batch_get", _presence_svc, GatewayAuth::JWT_REQUIRED,
        &pres::PresenceService_Stub::BatchGetPresence);
    route<pres::PresenceService_Stub, pres::SubscribeReq, pres::SubscribeRsp>(
        "/service/presence/subscribe", _presence_svc, GatewayAuth::JWT_REQUIRED,
        &pres::PresenceService_Stub::SubscribePresence);
    route<pres::PresenceService_Stub, pres::UnsubscribeReq, pres::UnsubscribeRsp>(
        "/service/presence/unsubscribe", _presence_svc, GatewayAuth::JWT_REQUIRED,
        &pres::PresenceService_Stub::UnsubscribePresence);

    // ====== 注册 HTTP handler ======
    for (auto& route : _routes) {
        _http_server.Post(route.path.data(),
            [this](const httplib::Request& req, httplib::Response& res) {
                handle_request(req, res);
            });
    }
}

// ====== push_notify 实现 ======

inline void GatewayServer::push_notify(const std::string& target_uid,
                                        const ::chatnow::push::NotifyMessage& notify) {
    auto channel = _channels->choose(_push_svc);
    if (!channel) {
        LOG_WARN("Push 服务不可用，通知未下发 uid={}", target_uid);
        return;
    }
    ::chatnow::push::PushService_Stub stub(channel.get());
    auto* closure = new SelfDeleteRpcClosure<::chatnow::push::PushToUserReq,
                                              ::chatnow::push::PushToUserRsp>();
    closure->req.set_user_id(target_uid);
    closure->req.mutable_notify()->CopyFrom(notify);
    std::string uid_copy = target_uid;
    closure->on_done = [uid_copy](brpc::Controller* c, const ::chatnow::push::PushToUserRsp&) {
        if (c->Failed()) {
            LOG_WARN("PushToUser 失败 uid={}: {}", uid_copy, c->ErrorText());
        }
    };
    stub.PushToUser(&closure->cntl, &closure->req, &closure->rsp, closure);
}

// ====== GatewayServerBuilder ======

class GatewayServerBuilder {
public:
    void make_redis_object(const std::string& host, int port, int db, bool keep_alive) {
        _redis_host = host; _redis_port = port; _redis_db = db; _redis_keep_alive = keep_alive;
    }
    void make_jwt_object(const std::string& auth_config_path) {
        _jwt_config = ::chatnow::auth::load_jwt_config_from_file(auth_config_path);
    }
    void make_discovery_object(const std::string& reg_host, const std::string& base,
                               const std::string& identity, const std::string& relationship,
                               const std::string& conversation, const std::string& message,
                               const std::string& transmite, const std::string& media,
                               const std::string& presence, const std::string& push) {
        _reg_host = reg_host; _base = base;
        _identity_svc = identity; _relationship_svc = relationship;
        _conversation_svc = conversation; _message_svc = message;
        _transmite_svc = transmite; _media_svc = media;
        _presence_svc = presence; _push_svc = push;
    }
    void make_server_object(int http_port) { _http_port = http_port; }

    GatewayServer::ptr build() {
        auto redis = std::make_shared<sw::redis::Redis>(
            fmt::format("tcp://{}:{}/{}", _redis_host, _redis_port, _redis_db));
        auto jwt_codec = std::make_shared<::chatnow::auth::JwtCodec>(_jwt_config);
        auto jwt_store = std::make_shared<::chatnow::auth::JwtStore>(redis);

        auto channels = std::make_shared<ServiceManager>();
        channels->declared(_identity_svc);
        channels->declared(_relationship_svc);
        channels->declared(_conversation_svc);
        channels->declared(_message_svc);
        channels->declared(_transmite_svc);
        channels->declared(_media_svc);
        channels->declared(_presence_svc);
        channels->declared(_push_svc);
        auto put_cb = std::bind(&ServiceManager::onServiceOnline, channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto discovery = std::make_shared<Discovery>(_reg_host, _base, put_cb, del_cb);

        return std::make_shared<GatewayServer>(
            _http_port, channels, jwt_codec, jwt_store,
            _identity_svc, _relationship_svc, _conversation_svc,
            _message_svc, _transmite_svc, _media_svc,
            _presence_svc, _push_svc);
    }

private:
    std::string _redis_host; int _redis_port = 6379, _redis_db = 0; bool _redis_keep_alive = true;
    ::chatnow::auth::JwtConfig _jwt_config;
    std::string _reg_host, _base;
    std::string _identity_svc, _relationship_svc, _conversation_svc;
    std::string _message_svc, _transmite_svc, _media_svc, _presence_svc, _push_svc;
    int _http_port = 9000;
};

}  // namespace chatnow
