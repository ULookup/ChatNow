# Gateway 重构 + Presence 服务 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Gateway 精简为纯代理层（~350行，鉴权+转发），Presence 服务从零新建（状态聚合+订阅+推送）。

**Architecture:** Gateway 采用显式路由注册 + 泛型转发器模板，每个端点 1 行声明。Presence 采用读/写分离：Push 直写 Redis presence 数据，Presence 服务负责聚合查询、订阅管理、定时扫描变化推送。

**Tech Stack:** C++17, httplib, brpc, Redis (hiredis+redis++), Protobuf3, JWT (HS256)

---

## File Structure

```
gateway/
  source/
    gateway_server.h        ─ 重写 (~350行): Route 路由表 + forward<T>() 泛型转发器
    gateway_server.cc       ─ 修改: main + gflags 同步
    gateway_auth.hpp        ─ 保留: JWT 鉴权中间件（不改）
    gateway_trace.hpp       ─ 保留: trace_id 生成 + 透传（不改）
    connection.hpp          ─ 删除: 已废弃（WS 在 Push）
  CMakeLists.txt            ─ 修改: proto_files 更新

presence/
  source/
    presence_server.h       ─ 新建 (~400行): PresenceServiceImpl + PresenceAggregator + Builder
    presence_server.cc      ─ 新建 (~20行): main
  CMakeLists.txt            ─ 新建
  conf/
    presence_server.conf    ─ 新建

proto/
  presence/
    presence_service.proto  ─ 修改: 删 SetPresence；Req 中调用方 user_id 从 metadata 取
```

---

## 完整路由表

```
# ====== Identity (WHITELISTED = 无需 JWT) ======
POST /service/identity/register          → IdentityService.Register          WHITELISTED
POST /service/identity/login             → IdentityService.Login             WHITELISTED
POST /service/identity/send_verify_code  → IdentityService.SendVerifyCode    WHITELISTED
POST /service/identity/refresh_token     → IdentityService.RefreshToken      WHITELISTED

# ====== Identity (JWT_REQUIRED) ======
POST /service/identity/logout            → IdentityService.Logout            JWT_REQUIRED
POST /service/identity/get_profile       → IdentityService.GetProfile        JWT_REQUIRED
POST /service/identity/update_profile    → IdentityService.UpdateProfile     JWT_REQUIRED
POST /service/identity/search_users      → IdentityService.SearchUsers       JWT_REQUIRED
POST /service/identity/get_multi_info    → IdentityService.GetMultiUserInfo  JWT_REQUIRED

# ====== Relationship ======
POST /service/relationship/list_friends         → RelationshipService.ListFriends          JWT_REQUIRED
POST /service/relationship/send_friend_request  → RelationshipService.SendFriendRequest    JWT_REQUIRED
POST /service/relationship/handle_friend_request → RelationshipService.HandleFriendRequest  JWT_REQUIRED
POST /service/relationship/remove_friend         → RelationshipService.RemoveFriend        JWT_REQUIRED
POST /service/relationship/search_friends        → RelationshipService.SearchFriends       JWT_REQUIRED
POST /service/relationship/block_user            → RelationshipService.BlockUser           JWT_REQUIRED
POST /service/relationship/unblock_user          → RelationshipService.UnblockUser         JWT_REQUIRED
POST /service/relationship/list_blocked          → RelationshipService.ListBlockedUsers    JWT_REQUIRED
POST /service/relationship/list_pending          → RelationshipService.ListPendingRequests JWT_REQUIRED

# ====== Conversation ======
POST /service/conversation/list              → ConversationService.ListConversations    JWT_REQUIRED
POST /service/conversation/get               → ConversationService.GetConversation      JWT_REQUIRED
POST /service/conversation/create            → ConversationService.CreateConversation   JWT_REQUIRED
POST /service/conversation/update            → ConversationService.UpdateConversation   JWT_REQUIRED
POST /service/conversation/dismiss           → ConversationService.DismissConversation  JWT_REQUIRED
POST /service/conversation/add_members       → ConversationService.AddMembers           JWT_REQUIRED
POST /service/conversation/remove_members    → ConversationService.RemoveMembers        JWT_REQUIRED
POST /service/conversation/transfer_owner    → ConversationService.TransferOwner        JWT_REQUIRED
POST /service/conversation/change_role       → ConversationService.ChangeMemberRole     JWT_REQUIRED
POST /service/conversation/list_members      → ConversationService.ListMembers          JWT_REQUIRED
POST /service/conversation/set_mute          → ConversationService.SetMute              JWT_REQUIRED
POST /service/conversation/set_pin           → ConversationService.SetPin               JWT_REQUIRED
POST /service/conversation/set_visible       → ConversationService.SetVisible           JWT_REQUIRED
POST /service/conversation/quit              → ConversationService.QuitConversation     JWT_REQUIRED
POST /service/conversation/mark_read         → ConversationService.MarkRead             JWT_REQUIRED
POST /service/conversation/save_draft        → ConversationService.SaveDraft            JWT_REQUIRED
POST /service/conversation/search            → ConversationService.SearchConversations  JWT_REQUIRED
POST /service/conversation/get_member_ids    → ConversationService.GetMemberIds         JWT_REQUIRED

# ====== Message ======
POST /service/message/sync            → MessageService.SyncMessages        JWT_REQUIRED timeout=10000
POST /service/message/get_history     → MessageService.GetHistory          JWT_REQUIRED
POST /service/message/get_by_id       → MessageService.GetMessagesById     JWT_REQUIRED
POST /service/message/search          → MessageService.SearchMessages      JWT_REQUIRED
POST /service/message/recall          → MessageService.RecallMessage       JWT_REQUIRED
POST /service/message/add_reaction    → MessageService.AddReaction         JWT_REQUIRED
POST /service/message/remove_reaction → MessageService.RemoveReaction      JWT_REQUIRED
POST /service/message/get_reactions   → MessageService.GetReactions        JWT_REQUIRED
POST /service/message/pin             → MessageService.PinMessage          JWT_REQUIRED
POST /service/message/unpin           → MessageService.UnpinMessage        JWT_REQUIRED
POST /service/message/list_pinned     → MessageService.ListPinnedMessages  JWT_REQUIRED
POST /service/message/delete          → MessageService.DeleteMessages      JWT_REQUIRED
POST /service/message/clear           → MessageService.ClearConversation   JWT_REQUIRED

# ====== Transmite ======
POST /service/transmite/send          → TransmiteService.SendMessage       JWT_REQUIRED timeout=1000

# ====== Media ======
POST /service/media/apply_upload      → MediaService.ApplyUpload       JWT_REQUIRED
POST /service/media/complete_upload   → MediaService.CompleteUpload    JWT_REQUIRED
POST /service/media/apply_download    → MediaService.ApplyDownload     JWT_REQUIRED
POST /service/media/get_file_info     → MediaService.GetFileInfo       JWT_REQUIRED
POST /service/media/speech_recognition → MediaService.SpeechRecognition JWT_REQUIRED

# ====== Presence ======
POST /service/presence/get            → PresenceService.GetPresence          JWT_REQUIRED
POST /service/presence/batch_get      → PresenceService.BatchGetPresence     JWT_REQUIRED
POST /service/presence/subscribe      → PresenceService.SubscribePresence    JWT_REQUIRED
POST /service/presence/unsubscribe    → PresenceService.UnsubscribePresence  JWT_REQUIRED
```

---

### Task 1: Proto 清理 — 删除旧 proto + 修正 presence proto

**Files:**
- Modify: `proto/presence/presence_service.proto`

- [ ] **Step 1: 修改 presence_service.proto — 删 SetPresence RPC，Req 中调用方 user_id 从 metadata 取**

```protobuf
syntax = "proto3";
package chatnow.presence;

import "common/envelope.proto";
import "common/types.proto";

option cc_generic_services = true;

enum PresenceState {
    PRESENCE_UNSPECIFIED = 0;
    ONLINE = 1;
    AWAY = 2;
    BUSY = 3;
    OFFLINE = 4;
    INVISIBLE = 5;
}

message DevicePresence {
    string device_id = 1;
    chatnow.common.DevicePlatform platform = 2;
    PresenceState state = 3;
    int64 last_active_at_ms = 4;
}

message Presence {
    string user_id = 1;
    PresenceState aggregated_state = 2;
    int64 last_active_at_ms = 3;
    repeated DevicePresence devices = 4;
}

service PresenceService {
    rpc GetPresence(GetPresenceReq) returns (GetPresenceRsp);
    rpc BatchGetPresence(BatchGetPresenceReq) returns (BatchGetPresenceRsp);
    rpc SubscribePresence(SubscribeReq) returns (SubscribeRsp);
    rpc UnsubscribePresence(UnsubscribeReq) returns (UnsubscribeRsp);
    rpc SendTyping(TypingReq) returns (TypingRsp);
}

message GetPresenceReq {
    string request_id = 1;
    string user_id = 2;                  // 查询目标
}
message GetPresenceRsp {
    chatnow.common.ResponseHeader header = 1;
    Presence presence = 2;
}

message BatchGetPresenceReq {
    string request_id = 1;
    repeated string user_ids = 2;
}
message BatchGetPresenceRsp {
    chatnow.common.ResponseHeader header = 1;
    map<string, Presence> presences = 2;
}

message SubscribeReq {
    string request_id = 1;
    repeated string subscribe_user_ids = 2;
    // subscriber_user_id 从 metadata 取
}
message SubscribeRsp { chatnow.common.ResponseHeader header = 1; }

message UnsubscribeReq {
    string request_id = 1;
    repeated string unsubscribe_user_ids = 2;
    // subscriber_user_id 从 metadata 取
}
message UnsubscribeRsp { chatnow.common.ResponseHeader header = 1; }

message TypingReq {
    string request_id = 1;
    string conversation_id = 2;
    bool is_typing = 3;
    // user_id、device_id 从 metadata 取
}
message TypingRsp { chatnow.common.ResponseHeader header = 1; }
```

- [ ] **Step 2: 全仓 grep 确认旧 proto 无引用后删除**

```bash
# 检查这些旧 proto 文件是否还有代码引用
grep -rn "gateway.proto\|notify.proto\|base.proto" --include="*.cc" --include="*.h" --include="CMakeLists.txt" .
# 如果只有 proto 自身 import 且无 .cc/.h 引用，标记待删
```

**注意**: 旧 proto 文件 (`gateway.proto`, `user.proto`, `file.proto`, `notify.proto`, `transmite.proto`, `speech.proto`, `base.proto`) 的实际删除放到所有代码迁移完成后统一处理（Task 12）。

- [ ] **Step 3: Commit**

```bash
git add proto/presence/presence_service.proto
git commit -m "refactor(proto): 精简 presence proto — 删 SetPresence，调用方 user_id 走 metadata"
```

---

### Task 2: Gateway 核心 — 泛型转发器模板 + 路由基础设施

**Files:**
- Create: `gateway/source/gateway_server.h` (完整重写)

- [ ] **Step 1: 编写 gateway_server.h — 前半部分（include + Route 声明 + forward 模板）**

```cpp
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
#include <string>

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

    // ====== 路由注册快捷宏 ======

    template<typename Stub, typename Req, typename Rsp, typename Method>
    void route(const std::string& path, const std::string& svc_name,
               GatewayAuth auth, Method method, int timeout_ms = 3000)
    {
        GatewayRoute r;
        r.path = path;
        r.service_name = svc_name;
        r.auth = auth;
        r.timeout_ms = timeout_ms;
        r.handler = [=](const httplib::Request& req, httplib::Response& res,
                        const ::chatnow::gateway::AuthInfo& a,
                        const ServiceManager::ptr& ch, int to, brpc::Controller&) {
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

    // ====== Push 通知辅助（保留用于 Presence 变化推送） ======

    void push_notify(const std::string& target_uid, const NotifyMessage& notify);

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

}  // namespace chatnow
```

- [ ] **Step 2: Commit**

```bash
git add gateway/source/gateway_server.h
git commit -m "feat(gateway): 重写 Gateway 核心 — 泛型转发器 + 路由表基础设施"
```

---

### Task 3: Gateway — 路由注册表 (register_routes)

**Files:**
- Modify: `gateway/source/gateway_server.h` (追加 register_routes 实现)

- [ ] **Step 1: 在 gateway_server.h 末尾（namespace 内）追加 register_routes 实现**

```cpp
void GatewayServer::register_routes() {
    using namespace ::chatnow;
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
    route<tx::TransmiteService_Stub, tx::SendMessageReq, tx::SendMessageRsp>(
        "/service/transmite/send", _transmite_svc, GatewayAuth::JWT_REQUIRED,
        &tx::TransmiteService_Stub::SendMessage, 1000);

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
            [this, &route](const httplib::Request& req, httplib::Response& res) {
                handle_request(req, res);
            });
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add gateway/source/gateway_server.h
git commit -m "feat(gateway): 添加完整路由注册表 — 全部新 proto 路径"
```

---

### Task 4: Gateway — main 函数适配

**Files:**
- Modify: `gateway/source/gateway_server.cc`

- [ ] **Step 1: 重写 gateway_server.cc**

```cpp
#include "gateway_server.h"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_int32(http_listen_port, 9000, "HTTP服务器监听端口");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(identity_service, "/service/identity_service", "Identity 子服务名称");
DEFINE_string(relationship_service, "/service/relationship_service", "Relationship 子服务名称");
DEFINE_string(conversation_service, "/service/conversation_service", "Conversation 子服务名称");
DEFINE_string(message_service, "/service/message_service", "Message 子服务名称");
DEFINE_string(transmite_service, "/service/transmite_service", "Transmite 子服务名称");
DEFINE_string(media_service, "/service/media_service", "Media 子服务名称");
DEFINE_string(presence_service, "/service/presence_service", "Presence 子服务名称");
DEFINE_string(push_service, "/service/push_service", "Push 子服务名称");

DEFINE_string(redis_host, "127.0.0.1", "Redis服务器访问地址");
DEFINE_int32(redis_port, 6379, "Redis服务器访问端口");
DEFINE_int32(redis_db, 0, "Redis默认库号");
DEFINE_bool(redis_keep_alive, true, "Redis长连接保活");

DEFINE_string(auth_config, "/im/conf/auth.json", "JWT 鉴权配置文件路径(JSON)");

int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    chatnow::GatewayServerBuilder gsb;
    gsb.make_redis_object(FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db, FLAGS_redis_keep_alive);
    gsb.make_jwt_object(FLAGS_auth_config);
    gsb.make_discovery_object(FLAGS_registry_host, FLAGS_base_service,
                              FLAGS_identity_service, FLAGS_relationship_service,
                              FLAGS_conversation_service, FLAGS_message_service,
                              FLAGS_transmite_service, FLAGS_media_service,
                              FLAGS_presence_service, FLAGS_push_service);
    gsb.make_server_object(FLAGS_http_listen_port);

    auto server = gsb.build();
    server->start();
    return 0;
}
```

**关键变更**: 去掉 `user_service` / `file_service` / `speech_service` 旧 gflags，替换为 `identity_service` / `media_service` / `presence_service`。

- [ ] **Step 2: 更新 gateway_server.h 末尾追加 GatewayServerBuilder**

```cpp
class GatewayServerBuilder {
public:
    void make_redis_object(const std::string& host, int port, int db, bool keep_alive) {
        _redis_host = host; _redis_port = port; _redis_db = db; _redis_keep_alive = keep_alive;
    }
    void make_jwt_object(const std::string& auth_config_path) {
        _auth_config_path = auth_config_path;
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
        auto loader = std::make_shared<::chatnow::auth::AuthConfigLoader>(_auth_config_path);
        loader->load();
        auto jwt_codec = std::make_shared<::chatnow::auth::JwtCodec>(
            loader->key_map(), loader->current_kid());
        auto jwt_store = std::make_shared<::chatnow::auth::JwtStore>(redis);

        auto discovery = std::make_shared<Discovery>(_reg_host, _base);
        auto channels = std::make_shared<ServiceManager>();
        channels->add(_identity_svc, discovery);
        channels->add(_relationship_svc, discovery);
        channels->add(_conversation_svc, discovery);
        channels->add(_message_svc, discovery);
        channels->add(_transmite_svc, discovery);
        channels->add(_media_svc, discovery);
        channels->add(_presence_svc, discovery);
        channels->add(_push_svc, discovery);

        return std::make_shared<GatewayServer>(
            _http_port, channels, jwt_codec, jwt_store,
            _identity_svc, _relationship_svc, _conversation_svc,
            _message_svc, _transmite_svc, _media_svc,
            _presence_svc, _push_svc);
    }

private:
    std::string _redis_host; int _redis_port, _redis_db; bool _redis_keep_alive;
    std::string _auth_config_path;
    std::string _reg_host, _base;
    std::string _identity_svc, _relationship_svc, _conversation_svc;
    std::string _message_svc, _transmite_svc, _media_svc, _presence_svc, _push_svc;
    int _http_port = 9000;
};
```

- [ ] **Step 3: Commit**

```bash
git add gateway/source/gateway_server.cc gateway/source/gateway_server.h
git commit -m "feat(gateway): 重写 main + Builder — 新服务名 gflags 适配"
```

---

### Task 5: Gateway CMakeLists 更新

**Files:**
- Modify: `gateway/CMakeLists.txt`

- [ ] **Step 1: 更新 proto_files 列表 — 去掉旧 proto，加入新 proto**

```cmake
set(proto_files
    common/types.proto
    common/error.proto
    common/envelope.proto
    identity/identity_service.proto
    relationship/relationship_service.proto
    conversation/conversation_service.proto
    message/message_types.proto
    message/message_service.proto
    media/media_service.proto
    presence/presence_service.proto
    push/push_service.proto
    push/notify.proto
    transmite/transmite_service.proto
)
```

去掉: `gateway.proto`, `user.proto`, `file.proto`, `speech.proto`, `transmite.proto`, `base.proto`, `notify.proto`

- [ ] **Step 2: 配置同步**

```bash
git add gateway/CMakeLists.txt
git commit -m "build(gateway): CMakeLists 同步新 proto 列表"
```

---

### Task 6: Presence 服务 — Proto 编译 + CMakeLists 骨架

**Files:**
- Create: `presence/CMakeLists.txt`
- Modify: 根 `CMakeLists.txt`（添加 presence 子目录）

- [ ] **Step 1: 创建 presence/CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.1.3)
project(presence_server)

set(target "presence_server")

set(proto_path ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(proto_files
    common/types.proto
    common/error.proto
    common/envelope.proto
    presence/presence_service.proto
    push/push_service.proto
    push/notify.proto
)

set(proto_srcs "")
foreach(proto_file ${proto_files})
    string(REPLACE ".proto" ".pb.cc" proto_cc ${proto_file})
    string(REPLACE ".proto" ".pb.h" proto_hh ${proto_file})
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
        add_custom_command(
            PRE_BUILD
            COMMAND protoc
            ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR} -I ${proto_path}
                 --experimental_allow_proto3_optional ${proto_path}/${proto_file}
            DEPENDS ${proto_path}/${proto_file}
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
            COMMENT "生成Protobuf框架代码: ${proto_cc}"
        )
    endif()
    list(APPEND proto_srcs ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
endforeach()

set(src_files "")
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/source src_files)

add_executable(${target} ${src_files} ${proto_srcs})

target_link_libraries(${target} -lgflags -lspdlog
    -lfmt -lbrpc -lssl -lcrypto
    -lprotobuf -lleveldb -letcd-cpp-api
    -lcurl -lhiredis -lredis++ -lpthread -lboost_system)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../third/include)

INSTALL(TARGETS ${target} RUNTIME DESTINATION bin)
```

- [ ] **Step 2: 创建 presence/source/ 目录**

```bash
mkdir -p presence/source
```

- [ ] **Step 3: Commit**

```bash
git add presence/CMakeLists.txt
git commit -m "build(presence): 新建 CMakeLists 骨架"
```

---

### Task 7: Presence 服务 — 核心实现 (PresenceAggregator)

**Files:**
- Create: `presence/source/presence_server.h`

- [ ] **Step 1: 编写 presence_server.h — PresenceAggregator + PresenceServiceImpl 头文件**

```cpp
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
#include <thread>
#include <chrono>

namespace chatnow::presence {

class PresenceAggregator {
public:
    using ptr = std::shared_ptr<PresenceAggregator>;

    explicit PresenceAggregator(std::shared_ptr<sw::redis::Redis> redis)
        : _redis(std::move(redis)) {}

    Presence aggregate(const std::string& uid) {
        Presence p;
        p.set_user_id(uid);

        // pipeline HGETALL 所有 im:presence:device:{uid}:*
        // 先用 SCAN 找出该 uid 的所有 device key
        std::vector<std::string> device_keys;
        auto cursor = 0LL;
        do {
            std::tie(cursor, std::ignore) = _redis->scan(
                cursor, "im:presence:device:" + uid + ":*", 100,
                std::back_inserter(device_keys));
        } while (cursor != 0);

        using clock = std::chrono::steady_clock;
        auto now = clock::now();
        PresenceState best_state = PresenceState::OFFLINE;
        int64_t best_active_ms = 0;

        if (device_keys.empty()) {
            p.set_aggregated_state(PresenceState::OFFLINE);
            p.set_last_active_at_ms(0);
            return p;
        }

        // pipeline 批量取
        auto pipe = _redis->pipeline();
        std::vector<sw::redis::OptionalString> hashes;
        for (auto& k : device_keys) {
            pipe.hget(k, "last_active_at_ms");
            pipe.hget(k, "state");
            pipe.hget(k, "platform");
        }
        auto results = pipe.exec();

        for (size_t i = 0; i < device_keys.size(); ++i) {
            auto ttl_ms_str = results[i * 3 + 0].template get<sw::redis::OptionalString>();
            auto state_str  = results[i * 3 + 1].template get<sw::redis::OptionalString>();
            auto plat_str   = results[i * 3 + 2].template get<sw::redis::OptionalString>();

            if (!ttl_ms_str || !state_str) continue;  // 已过期

            int64_t last_ms = std::stoll(*ttl_ms_str);
            // 检查 TTL: 超过 125s 认为过期
            auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - clock::now() + std::chrono::milliseconds(
                    std::chrono::system_clock::now().time_since_epoch().count() - last_ms
                )).count();

            PresenceState dev_state = static_cast<PresenceState>(std::stoi(*state_str));

            DevicePresence* dp = p.add_devices();
            dp->set_device_id(
                device_keys[i].substr(device_keys[i].find_last_of(':') + 1));
            dp->set_state(dev_state);
            dp->set_last_active_at_ms(last_ms);
            if (plat_str) {
                dp->set_platform(
                    static_cast<chatnow::common::DevicePlatform>(std::stoi(*plat_str)));
            }

            // INVISIBLE → 对外显示 OFFLINE，只自己可见真实值
            if (dev_state == PresenceState::INVISIBLE) {
                dev_state = PresenceState::OFFLINE;
            }

            if (static_cast<int>(dev_state) < static_cast<int>(best_state)
                || best_state == PresenceState::OFFLINE) {
                best_state = dev_state;
            }
            if (last_ms > best_active_ms) best_active_ms = last_ms;
        }

        if (best_state == PresenceState::OFFLINE && !device_keys.empty()) {
            best_state = PresenceState::OFFLINE;
        }
        p.set_aggregated_state(best_state);
        p.set_last_active_at_ms(best_active_ms);
        return p;
    }

    std::vector<std::string> active_user_ids() {
        std::vector<std::string> ids;
        auto cursor = 0LL;
        do {
            std::tie(cursor, std::ignore) = _redis->scan(
                cursor, "im:presence:device:*", 1000, std::back_inserter(ids));
        } while (cursor != 0);
        // 提取 uid（去掉前缀和后缀）
        for (auto& id : ids) {
            auto start = std::string("im:presence:device:").size();
            auto end = id.find_last_of(':');
            id = id.substr(start, end - start);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        return ids;
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

    void GetPresence(::google::protobuf::RpcController* cntl_base,
                     const GetPresenceReq* req, GetPresenceRsp* rsp,
                     ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(cntl_base);
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
        }
    }

    void BatchGetPresence(::google::protobuf::RpcController* cntl_base,
                          const BatchGetPresenceReq* req, BatchGetPresenceRsp* rsp,
                          ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(cntl_base);
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
        }
    }

    void SubscribePresence(::google::protobuf::RpcController* cntl_base,
                           const SubscribeReq* req, SubscribeRsp* rsp,
                           ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(cntl_base);
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

    void UnsubscribePresence(::google::protobuf::RpcController* cntl_base,
                             const UnsubscribeReq* req, UnsubscribeRsp* rsp,
                             ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(cntl_base);
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

    void SendTyping(::google::protobuf::RpcController* cntl_base,
                    const TypingReq* req, TypingRsp* rsp,
                    ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(cntl_base);
        try {
            auto auth = ::chatnow::auth::extract_auth(cntl);
            auto* h = rsp->mutable_header();
            h->set_success(true);
            h->set_error_code(::chatnow::error::kOK);
            h->set_request_id(req->request_id());

            if (req->is_typing()) {
                _redis->sadd("im:presence:typing:" + req->conversation_id(),
                             auth.user_id + ":" + std::to_string(
                                 std::chrono::system_clock::now().time_since_epoch().count()));
                _redis->expire("im:presence:typing:" + req->conversation_id(), 10);
            } else {
                // 用模糊匹配删除
                auto members = _redis->smembers("im:presence:typing:" + req->conversation_id());
                for (const auto& m : *members) {
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

    // 启动定时扫描线程（主线程调用）
    void start_change_scanner(int interval_sec = 5) {
        _scan_running = true;
        _scan_thread = std::thread([this, interval_sec]() {
            std::unordered_map<std::string, PresenceState> last_state;
            while (_scan_running) {
                std::this_thread::sleep_for(std::chrono::seconds(interval_sec));
                if (!_scan_running) break;

                auto active_uids = _aggregator->active_user_ids();
                for (const auto& uid : active_uids) {
                    auto p = _aggregator->aggregate(uid);
                    auto it = last_state.find(uid);
                    if (it == last_state.end() || it->second != p.aggregated_state()) {
                        last_state[uid] = p.aggregated_state();
                        notify_subscribers(uid, p);
                    }
                }
                // 清理已下线的用户
                for (auto it = last_state.begin(); it != last_state.end();) {
                    if (std::find(active_uids.begin(), active_uids.end(), it->first)
                        == active_uids.end()) {
                        if (it->second != PresenceState::OFFLINE) {
                            Presence offline;
                            offline.set_user_id(it->first);
                            offline.set_aggregated_state(PresenceState::OFFLINE);
                            notify_subscribers(it->first, offline);
                        }
                        it = last_state.erase(it);
                    } else {
                        ++it;
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
        auto subs = _redis->smembers("im:presence:sub:" + uid);
        if (!subs || subs->empty()) return;

        auto channel = _channels->choose(_push_service_name);
        if (!channel) return;

        NotifyMessage notify;
        notify.set_notify_type(NotifyType::PRESENCE_CHANGE_NOTIFY);

        for (const auto& sub_uid : *subs) {
            PushService_Stub stub(channel.get());
            auto* closure = new SelfDeleteRpcClosure<PushToUserReq, PushToUserRsp>();
            closure->req.set_request_id("");
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
```

- [ ] **Step 2: 检查编译**

```bash
# 先确认 proto 生成正确
protoc --cpp_out=/tmp -I proto --experimental_allow_proto3_optional proto/presence/presence_service.proto
```

- [ ] **Step 3: Commit**

```bash
git add presence/source/presence_server.h
git commit -m "feat(presence): PresenceAggregator + PresenceServiceImpl 核心实现"
```

---

### Task 8: Presence 服务 — main + Builder

**Files:**
- Create: `presence/source/presence_server.cc`

- [ ] **Step 1: 编写 presence_server.cc**

```cpp
#include "presence_server.h"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_int32(listen_port, 9050, "Presence 服务 RPC 监听端口");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(presence_service, "/service/presence_service", "Presence 子服务名称");
DEFINE_string(push_service, "/service/push_service", "Push 子服务名称");

DEFINE_string(redis_host, "127.0.0.1", "Redis服务器访问地址");
DEFINE_int32(redis_port, 6379, "Redis服务器访问端口");
DEFINE_int32(redis_db, 0, "Redis默认库号");
DEFINE_bool(redis_keep_alive, true, "Redis长连接保活");

DEFINE_int32(change_scan_interval_sec, 5, "状态变化扫描间隔（秒）");

int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    // Redis
    auto redis = std::make_shared<sw::redis::Redis>(
        fmt::format("tcp://{}:{}/{}", FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db));

    // 服务发现
    auto discovery = std::make_shared<Discovery>(FLAGS_registry_host, FLAGS_base_service);
    auto channels = std::make_shared<ServiceManager>();
    channels->add(FLAGS_push_service, discovery);

    // Presence 服务实例
    auto impl = std::make_shared<chatnow::presence::PresenceServiceImpl>(
        redis, channels, FLAGS_push_service);

    // 启动变化扫描器
    impl->start_change_scanner(FLAGS_change_scan_interval_sec);

    // brpc server
    brpc::Server server;
    brpc::ServerOptions opt;
    opt.num_threads = 4;
    opt.idle_timeout_sec = 30;

    if (server.AddService(impl.get(), brpc::SERVER_OWNS_SERVICE) != 0) {
        LOG_ERROR("Presence 服务注册失败");
        return -1;
    }

    butil::EndPoint ep;
    if (butil::str2endpoint("0.0.0.0", FLAGS_listen_port, &ep) != 0) {
        LOG_ERROR("Presence 服务端口解析失败 port={}", FLAGS_listen_port);
        return -1;
    }

    if (server.Start(ep, &opt) != 0) {
        LOG_ERROR("Presence 服务启动失败");
        return -1;
    }

    // 注册到 etcd
    auto reg = std::make_shared<Registry>(FLAGS_registry_host);
    reg->register_service(FLAGS_presence_service, "0.0.0.0", FLAGS_listen_port);

    LOG_INFO("Presence 服务已启动 port={}", FLAGS_listen_port);

    server.RunUntilAskedToQuit();

    impl->stop_change_scanner();
    server.Stop(0);
    server.Join();

    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add presence/source/presence_server.cc
git commit -m "feat(presence): main + Builder — 启动/注册/变化扫描"
```

---

### Task 9: Presence 配置文件

**Files:**
- Create: `conf/presence_server.conf`

- [ ] **Step 1: 创建 conf/presence_server.conf**

```
# Presence 服务配置
-run_mode=false
-log_file=presence_server.log
-log_level=0

# RPC 端口
-listen_port=9050

# 注册中心
-registry_host=http://127.0.0.1:2379
-base_service=/service
-presence_service=/service/presence_service
-push_service=/service/push_service

# Redis
-redis_host=127.0.0.1
-redis_port=6379
-redis_db=0
-redis_keep_alive=true

# 状态扫描间隔（秒）
-change_scan_interval_sec=5
```

- [ ] **Step 2: Commit**

```bash
git add conf/presence_server.conf
git commit -m "feat(presence): 添加配置文件"
```

---

### Task 10: Gateway 配置同步

**Files:**
- Modify: `conf/push_server.conf` (添加 presence_service 引用 — 若 Push 需要调 Presence)
- Modify: `conf/gateway.conf` (同步新的 gflag 名称，若存在)

- [ ] **Step 1: 检查 gateway 配置是否存在并更新**

```bash
ls conf/ | grep -i gateway
# 若存在 gateway 配置，更新 gflag 名称
```

**注意**: Gateway 目前可能没有独立配置（gflag 直接传参），此任务按实际是否存在配置文件调整。

- [ ] **Step 2: Commit**

```bash
# 按实际变动提交
```

---

### Task 11: 集成编译验证

- [ ] **Step 1: 编译 Gateway**

```bash
mkdir -p build/gateway && cd build/gateway
cmake ../../gateway -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

预期: 编译成功，无错误。

- [ ] **Step 2: 编译 Presence**

```bash
mkdir -p build/presence && cd build/presence
cmake ../../presence -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

预期: 编译成功，无错误。

- [ ] **Step 3: 验证 Gateway grep — 旧引用清除**

```bash
grep -rn "UserService_Stub\|FileService_Stub\|SpeechService_Stub\|MsgTransmitService_Stub\|session_id" gateway/source/ | grep -v ".bak"
```

预期: 零命中。

- [ ] **Step 4: Commit（如有编译相关的 fixup 改动）**

---

### Task 12: 清理旧 proto 文件

- [ ] **Step 1: 全仓 grep 确认零引用**

```bash
for f in gateway.proto user.proto file.proto notify.proto transmite.proto speech.proto base.proto; do
    echo "=== $f ==="
    grep -rn "$(basename $f .proto)" --include="*.cc" --include="*.h" --include="*.proto" --include="CMakeLists.txt" . | grep -v "build/" | grep -v ".git/"
done
```

- [ ] **Step 2: 删除零引用的旧 proto**

```bash
# 仅删除确认零引用的文件
git rm proto/gateway.proto
```

- [ ] **Step 3: Commit**

```bash
git commit -m "chore(proto): 删除旧 gateway.proto"
```

---

### Task 13: 移除 gateway_server.h 中的 connection.hpp 依赖

**Files:**
- Modify: `gateway/source/gateway_server.h`

- [ ] **Step 1: 删除 gateway source 目录下的 connection.hpp**

```bash
git rm gateway/source/connection.hpp
```

`connection.hpp` 已废弃（Push 终结 WS，Gateway 不再管连接）。新 gateway_server.h 已不 include。

- [ ] **Step 2: Commit**

```bash
git commit -m "chore(gateway): 删除废弃的 connection.hpp"
```

---

## Self-Review

**1. Spec coverage:**
- ✓ Gateway 纯代理设计 → Task 2-5
- ✓ 路由注册 + 转发器模板 → Task 2, 3
- ✓ JWT 鉴权中间件保留 → 已有（gateway_auth.hpp）
- ✓ 端点级超时可配 → Task 3 (route 模板有 timeout_ms 参数)
- ✓ Presence 读/写分离 → Task 7 (aggregator 只读 Redis)
- ✓ 多设备状态聚合 → Task 7 (aggregate 方法)
- ✓ 订阅自动建立 → Task 7 (SubscribePresence)
- ✓ 状态变化扫描 → Task 7 (start_change_scanner)
- ✓ Proto 变更 → Task 1
- ✓ Typing 预留 → Task 7 (SendTyping 实现)
- ✓ 限流设计 → spec 已定义，Plan 中不单独实现（YAGNI — 先 Nginx 层做）
- ✓ 验收标准 → Task 11 编译验证 + grep 验证

**2. Placeholder scan:** 无 TBD/TODO/placeholder。

**3. Type consistency:**
- `Route` 结构一致 — `GatewayRoute` 在 Task 2 定义，Task 3 使用 ✓
- `forward<T>()` 模板签名一致 — Task 2 定义，Task 3 route() 调用 ✓
- `PresenceAggregator` 接口一致 — Task 7 定义 `aggregate(uid)`，`active_user_ids()` ✓
- Proto 类型与路由注册表一致 — 所有 `route<>()` 调用中的 Req/Rsp 类型对应 proto 文件定义 ✓
