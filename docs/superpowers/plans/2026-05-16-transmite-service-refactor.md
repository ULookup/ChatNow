# Transmite 服务重构 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重构 Transmite 服务：Proto 重写（`SendMessage` RPC）+ 服务实现对齐新架构（metadata 鉴权、Stub 切换、ResponseHeader）。

**Architecture:** 修改 3 个文件：proto 定义、服务实现头文件、配置文件。无新文件，无 DAO/ODB 变更。核心发送管线逻辑（幂等→限流→拉成员→发号→MQ）保持不变。

**Tech Stack:** C++17, brpc, Protobuf, RabbitMQ (AMQP), Redis, Snowflake

---

### Task 1: 重写 Proto 定义

**Files:**
- Modify: `proto/transmite/transmite_service.proto`（全量重写）

- [ ] **Step 1: 重写 proto 文件**

用以下内容完整替换 `proto/transmite/transmite_service.proto`：

```protobuf
syntax = "proto3";
package chatnow.transmite;

option cc_generic_services = true;

import "common/envelope.proto";
import "message/message_types.proto";

service MsgTransmitService {
    rpc SendMessage(SendMessageReq) returns (SendMessageRsp);
}

message SendMessageReq {
    string request_id = 1;
    string conversation_id = 2;
    chatnow.message.MessageContent content = 3;
    string client_msg_id = 4;                           // 客户端幂等键
    optional chatnow.message.ReplyRef reply_to = 5;     // 回复引用
    repeated string mentioned_user_ids = 6;             // @提及
    optional chatnow.message.ForwardInfo forward_info = 7; // 转发来源
}

message SendMessageRsp {
    chatnow.common.ResponseHeader header = 1;
    chatnow.message.Message message = 2;                // 组装后的完整消息
}
```

- [ ] **Step 2: 提交**

```bash
git add proto/transmite/transmite_service.proto
git commit -m "proto(transmite): GetTransmitTarget → SendMessage，Req 去 user_id/session_id，加 reply_to/mentioned_user_ids/forward_info，Rsp 切 ResponseHeader"
```

---

### Task 2: 更新配置文件

**Files:**
- Modify: `conf/transmite_server.conf:11`

- [ ] **Step 1: 改 user_service 为 identity_service**

将第 11 行 `-user_service=/service/user_service` 改为：
```
-identity_service=/service/identity_service
```

- [ ] **Step 2: 提交**

```bash
git add conf/transmite_server.conf
git commit -m "conf(transmite): user_service → identity_service"
```

---

### Task 3: Server — Include 与 Builder 配置名

**Files:**
- Modify: `transmite/source/transmite_server.h:1-19`（includes）
- Modify: `transmite/source/transmite_server.h:35-56`（TransmiteServiceImpl 构造参数/成员）
- Modify: `transmite/source/transmite_server.h:296-298`（TransmiteServiceImpl 成员变量）
- Modify: `transmite/source/transmite_server.h:389-396`（Builder::make_discovery_object）

- [ ] **Step 1: 替换 includes**

删除：
```cpp
#include "common/types.pb.h"
```

新增（在 `#include "infra/logger.hpp"` 之后插入）：
```cpp
#include "auth/auth_context.hpp"
#include "auth/forward_auth.hpp"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
```

注意：`#include "identity/identity_service.pb.h"` 已存在（第 15 行），不需要改。

- [ ] **Step 2: 重命名构造参数与成员变量 `_user_service_name` → `_identity_service_name`**

在 `TransmiteServiceImpl` 类中（约第 35-56 行），将构造函数第一个参数名从 `user_service_name` 改为 `identity_service_name`：

```cpp
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
                    const RateLimiter::ptr &rate_limiter)
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
                    _rate_limiter(rate_limiter) {}
```

在 private 成员变量区（约第 296 行），将：
```cpp
std::string _user_service_name;
```
改为：
```cpp
std::string _identity_service_name;
```

- [ ] **Step 3: 更新 Builder 中服务发现注册名**

在 `TransmiteServerBuilder::make_discovery_object`（约第 389-396 行），将：
```cpp
_user_service_name = user_service_name;
...
_mm_channels->declared(_user_service_name);
```
改为：
```cpp
_identity_service_name = identity_service_name;
...
_mm_channels->declared(_identity_service_name);
```

同时将 `make_discovery_object` 的第三个参数名从 `user_service_name` 改为 `identity_service_name`。

- [ ] **Step 4: 更新 Builder::make_rpc_object 中的构造传参**

在 `make_rpc_object`（约第 481 行）中，`new TransmiteServiceImpl(` 的第一个参数从 `_user_service_name` 改为 `_identity_service_name`。

- [ ] **Step 5: 提交**

```bash
git add transmite/source/transmite_server.h
git commit -m "refactor(transmite): includes + _user_service_name → _identity_service_name"
```

---

### Task 4: Server — SendMessage Handler 重写

**Files:**
- Modify: `transmite/source/transmite_server.h:59-293`（`GetTransmitTarget` 方法 → `SendMessage`）

- [ ] **Step 1: 替换函数签名与开头（auth 提取 + err_response 辅助）**

将函数签名及开头（约第 59-80 行）替换为：

```cpp
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
```

- [ ] **Step 2: 替换文件消息校验（field accessor 对齐新 MessageContent）**

将「步骤 0: 入参合法性校验」部分（约第 77-98 行）替换为：

```cpp
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
```

- [ ] **Step 3: 替换幂等去重（Stub 切换 + 响应格式）**

将「步骤 1: 客户端幂等去重」部分（约第 100-123 行）替换为：

```cpp
    // ③ 客户端幂等去重（命中 client_msg_id 索引则直接返回旧消息）
    if (!client_msg_id.empty()) {
        auto msg_channel = _mm_channels->choose(_message_service_name);
        if (msg_channel) {
            chatnow::message::MessageService_Stub stub(msg_channel.get());
            chatnow::message::SelectByClientMsgIdReq dup_req;
            chatnow::message::SelectByClientMsgIdRsp dup_rsp;
            brpc::Controller dup_cntl;
            dup_req.set_request_id(rid);
            dup_req.set_client_msg_id(client_msg_id);
            stub.SelectByClientMsgId(&dup_cntl, &dup_req, &dup_rsp, nullptr);
            if (!dup_cntl.Failed() && dup_rsp.header().success()) {
                LOG_INFO("请求ID: {} - 命中幂等 client_msg_id={} 直接返回旧消息", rid, client_msg_id);
                response->mutable_header()->set_request_id(rid);
                response->mutable_header()->set_success(true);
                response->mutable_message()->CopyFrom(dup_rsp.message());
                return;
            }
        } else {
            LOG_WARN("请求ID: {} - message_service 不可用，跳过幂等检查（fail-open）", rid);
        }
    }
```

- [ ] **Step 4: 替换限流（uid 来源改为 auth）**

将「步骤 1.5: 限流」部分（约第 126-142 行）替换为（仅改变量名，uid 和 chat_ssid 已在 Step 1 中定义）：

```cpp
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
```

- [ ] **Step 5: 替换并行 RPC（IdentityService_Stub + sender 成员校验）**

将「步骤 2: 并行 RPC」部分（约第 144-205 行）替换为：

```cpp
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
```

- [ ] **Step 6: ID 生成 + InternalMessage 组装 + MQ 投递（保持逻辑仅改 field accessor）**

将「步骤 3-6」部分（约第 207-260 行）替换为：

```cpp
    // ⑦ 申请 session_seq（Redis INCR）
    unsigned long session_seq = _seq_gen->next_session_seq(chat_ssid);
    if (session_seq == 0) {
        LOG_ERROR("请求ID: {} - 申请 session_seq 失败 ssid={}", rid, chat_ssid);
        return err_response(rid, chatnow::error::kSystemUnavailable, "序号生成失败");
    }

    // ⑧ 组装 InternalMessage
    InternalMessage internal_msg;
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
```

注意：InternalMessage 类型名保持不变（仍然是 `InternalMessage`，新 proto 中结构未变），但 `mutable_message()` 现在返回 `chatnow::message::Message*` 而非旧类型。`Message` 不包含 `mutable_sender()`（sender 用 `sender_id` 字段），删除旧代码中的 `msg_info->mutable_sender()->CopyFrom(user_rsp.user_info());`。

- [ ] **Step 7: 替换 MQ 投递 + 响应返回部分**

将「步骤 6」（约第 261-293 行）替换为：

```cpp
    // ⑩ MQ publish_confirm（异步回调完成后 Run done）
    google::protobuf::Closure *async_done = rpc_guard.release();

    std::shared_ptr<std::atomic<bool>> done_called =
        std::make_shared<std::atomic<bool>>(false);
    try {
        std::map<std::string, std::string> _mq_headers;
        ::chatnow::mq::mq_inject_trace_headers(_mq_headers);
        _publisher->publish_confirm(internal_msg.SerializeAsString(),
            _mq_headers,
            [async_done, response, rid, done_called](PublishStatus status, const std::string &mq_msg) {
                if (done_called->exchange(true)) return;
                if (status == PublishStatus::Acked) {
                    LOG_DEBUG("请求ID: {} - 消息成功投递到 Broker", rid);
                } else {
                    LOG_ERROR("请求ID: {} - 消息投递到 Broker 失败: {}", rid, mq_msg);
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
```

- [ ] **Step 8: 提交**

```bash
git add transmite/source/transmite_server.h
git commit -m "refactor(transmite): SendMessage handler — metadata 鉴权 + Stub 切换 + ResponseHeader"
```

---

### Task 5: 构建验证

- [ ] **Step 1: 编译**

```bash
cd /home/icepop/ChatNow/build && cmake .. && make -j$(nproc)
```

- [ ] **Step 2: 修复编译错误（如有）**

常见预期问题：
- proto 编译后生成的 C++ 类型名与代码中引用的不一致：检查命名空间（`chatnow::transmite::SendMessageReq` / `chatnow::identity::GetProfileReq` 等）
- `MessageContent` field accessor 名：新 proto 用 `type()`/`image()`/`file()`/`audio()`，不是旧的 `message_type()`/`image_message()` 等
- `SelectByClientMsgId` 的 exists check：新 proto 直接用 `header().success()` + 判断 `message().message_id() != 0`

- [ ] **Step 3: 提交（如修改）**

```bash
git add -A
git commit -m "fix(transmite): 编译错误修复"
```
