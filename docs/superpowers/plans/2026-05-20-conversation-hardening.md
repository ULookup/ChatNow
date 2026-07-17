# Conversation 服务生产加固实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 conversation 服务 7 项缺陷，覆盖一致性/安全性/可观测性/性能四个维度。

**Architecture:** 改动集中在 `conversation_server.h`（RPC handler）、`mysql_conversation_member.hpp`（DAO）、`data_es.hpp`（ES 搜索）、proto（`AddMembersRsp`），以及新增 `metrics.hpp`（降级计数器）。不改动 Redis 数据结构、不新增 RPC、不改变客户端 HTTP 路径。

**Tech Stack:** C++17, brpc, ODB ORM, sw::redis++, elasticlient, protobuf, bvar

**Spec:** `docs/superpowers/specs/2026-05-20-conversation-hardening-design.md`

---

## File Map

| 文件 | 职责 |
|------|------|
| `proto/conversation/conversation_service.proto` | `AddMembersRsp` 新增 `failed_member_ids` |
| `common/infra/metrics.hpp` | **新建** — bvar counter 声明 |
| `common/dao/data_es.hpp` | `ESConversation` 索引 + `member_ids` / 搜索加 caller 过滤 / `append_data` 写 `member_ids` |
| `common/dao/mysql_conversation_member.hpp` | `transfer_owner` 原子事务方法 |
| `conversation/source/conversation_server.h` | 全部 handler 改动 + 新增 private 成员/方法 |

---

### Task 1: Proto — AddMembersRsp 新增 failed_member_ids

**Files:**
- Modify: `proto/conversation/conversation_service.proto`

- [ ] **Step 1: 修改 proto 文件**

在 `AddMembersRsp` 新增字段：

```proto
message AddMembersRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated string failed_member_ids = 2;
}
```

- [ ] **Step 2: 提交**

```bash
git add proto/conversation/conversation_service.proto
git commit -m "feat(proto): add failed_member_ids to AddMembersRsp"
```

---

### Task 2: TransferOwner 原子化

**Files:**
- Modify: `common/dao/mysql_conversation_member.hpp`
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 在 ConversationMemberTable 新增 transfer_owner 方法**

在 `mysql_conversation_member.hpp` 的 `update_role` 方法之后、`list_ordered_by_user` 之前，新增：

```cpp
    /* brief: 原子转让群主 — 一个事务内完成 role 交换 + conversation.owner_id 更新
     *  - FOR UPDATE 锁住两行 member + 一行 conversation，防并发转让
     *  - 校验 old_owner 确实是 OWNER、new_owner 是活跃成员
     *  - 成功返回 true，失败返回 false
     */
    bool transfer_owner(const std::string &cid,
                        const std::string &old_owner_id,
                        const std::string &new_owner_id) {
        try {
            odb::transaction trans(_db->begin());

            using MQuery = odb::query<ConversationMember>;
            auto m1 = _db->query_one<ConversationMember>(
                (MQuery::conversation_id == cid && MQuery::user_id == old_owner_id) + " FOR UPDATE");
            auto m2 = _db->query_one<ConversationMember>(
                (MQuery::conversation_id == cid && MQuery::user_id == new_owner_id) + " FOR UPDATE");

            if (!m1 || !m2 || m2->is_quit()) { trans.commit(); return false; }
            if (m1->role() != MemberRole::OWNER) { trans.commit(); return false; }

            m1->role(MemberRole::ADMIN);
            m2->role(MemberRole::OWNER);
            _db->update(*m1);
            _db->update(*m2);

            using ConvQuery = odb::query<Conversation>;
            auto c = _db->query_one<Conversation>(
                ConvQuery::conversation_id == cid);
            if (c) { c->owner_id(new_owner_id); _db->update(*c); }

            trans.commit();
            return true;
        } catch (std::exception &e) {
            LOG_ERROR("transfer_owner 失败 {}-{}-{}: {}", cid, old_owner_id, new_owner_id, e.what());
            return false;
        }
    }
```

- [ ] **Step 2: 修改 TransferOwner handler**

在 `conversation_server.h` 的 `TransferOwner` handler 中，替换原有实现：

```cpp
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
            if (!_mysql_member->transfer_owner(req->conversation_id(), auth.user_id, req->new_owner_id()))
                throw ServiceError(::chatnow::error::kSystemInternalError,
                                   "transfer_owner failed");
        });
    }
```

- [ ] **Step 3: 提交**

```bash
git add common/dao/mysql_conversation_member.hpp conversation/source/conversation_server.h
git commit -m "fix(conversation): make TransferOwner atomic with single DB transaction"
```

---

### Task 3: CreateConversation PRIVATE 幂等路径成员校验

**Files:**
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 修改 CreateConversation handler 的 PRIVATE 分支**

在 `conversation_server.h` 的 `CreateConversation` handler 中，将 PRIVATE 幂等路径：

```cpp
            if (type_p == ConversationType::PRIVATE) {
                const auto& peer = req->member_ids(0);
                cid = private_id_of_(auth.user_id, peer);
                if (_mysql_conv->exists(cid)) {
                    rsp->mutable_conversation()->set_conversation_id(cid);
                    rsp->mutable_conversation()->set_type(type_p);
                    return;        // 幂等
                }
```

改为：

```cpp
            if (type_p == ConversationType::PRIVATE) {
                const auto& peer = req->member_ids(0);
                cid = private_id_of_(auth.user_id, peer);
                if (_mysql_conv->exists(cid)) {
                    if (!require_member_(cid, auth.user_id))
                        throw ServiceError(::chatnow::error::kConversationNotMember,
                                           "conversation exists but you are not a member");
                    rsp->mutable_conversation()->set_conversation_id(cid);
                    rsp->mutable_conversation()->set_type(type_p);
                    return;
                }
```

- [ ] **Step 2: 提交**

```bash
git add conversation/source/conversation_server.h
git commit -m "fix(conversation): validate membership on idempotent PRIVATE create"
```

---

### Task 4: 降级 Metrics 埋点

**Files:**
- Create: `common/infra/metrics.hpp`
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 创建 metrics.hpp**

新建 `common/infra/metrics.hpp`：

```cpp
#pragma once

#include <bvar/bvar.h>

namespace chatnow::metrics {

// fail-soft 降级计数器
inline bvar::Adder<long> g_degraded_identity_total;
inline bvar::Adder<long> g_degraded_message_total;
inline bvar::Adder<long> g_degraded_es_write_total;

}  // namespace chatnow::metrics
```

- [ ] **Step 2: 在 conversation_server.h 引入 metrics.hpp**

在 `conversation_server.h` 头部 include 区新增：

```cpp
#include "infra/metrics.hpp"
```

- [ ] **Step 3: 在 fetch_user_infos_ 失败路径埋点**

在 `fetch_user_infos_` 方法中，每个 `return false` 前加 counter：

```cpp
    bool fetch_user_infos_(brpc::Controller* in_cntl, const std::string& rid,
                           const std::vector<std::string>& uids,
                           std::unordered_map<std::string, ::chatnow::common::UserInfo>& out)
    {
        if (uids.empty()) return true;
        auto channel = _mm_channels->choose(_identity_service_name);
        if (!channel) {
            LOG_ERROR("rid={} identity 子服务节点不可达 svc={}", rid, _identity_service_name);
            metrics::g_degraded_identity_total << 1;
            return false;
        }
        // ... stub call ...
        if (out_cntl.Failed()) {
            LOG_ERROR("rid={} GetMultiUserInfo brpc 失败: {}", rid, out_cntl.ErrorText());
            metrics::g_degraded_identity_total << 1;
            return false;
        }
        if (!irsp.header().success()) {
            LOG_ERROR("rid={} GetMultiUserInfo 业务失败: code={} msg={}",
                      rid, irsp.header().error_code(), irsp.header().error_message());
            metrics::g_degraded_identity_total << 1;
            return false;
        }
        // ...
    }
```

- [ ] **Step 4: 在 fetch_last_message_ 失败路径埋点**

在 `fetch_last_message_` 方法中，每个 `return false` 前加 counter：

```cpp
    bool fetch_last_message_(/* ... */) {
        auto channel = _mm_channels->choose(_message_service_name);
        if (!channel) {
            metrics::g_degraded_message_total << 1;
            return false;
        }
        // ... stub call ...
        if (out_cntl.Failed() || !mrsp.header().success() || mrsp.messages_size() == 0) {
            if (out_cntl.Failed())
                metrics::g_degraded_message_total << 1;
            return false;
        }
        // ...
    }
```

- [ ] **Step 5: 在 ES 写入失败路径埋点**

在 `CreateConversation` 和 `UpdateConversation` 的 `_es_conv->append_data` 调用处，以及 `DismissConversation` 的 `_es_conv->remove` 调用处，返回值检查加 counter：

```cpp
// CreateConversation, UpdateConversation:
if (!_es_conv->append_data(ent))
    metrics::g_degraded_es_write_total << 1;

// DismissConversation:
if (!_es_conv->remove(req->conversation_id()))
    metrics::g_degraded_es_write_total << 1;
```

- [ ] **Step 6: 提交**

```bash
git add common/infra/metrics.hpp conversation/source/conversation_server.h
git commit -m "feat(conversation): add degraded metrics counters for fail-soft paths"
```

---

### Task 5: ListConversations — LastMessage Redis 缓存

**Files:**
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 在 ConversationServiceImpl 成员中新增 _last_msg_cache**

在 `conversation_server.h` 的 `ConversationServiceImpl` 类 private 成员区（`_mm_channels` 之后）新增：

```cpp
    LastMessage::ptr              _last_msg_cache;
```

- [ ] **Step 2: 修改构造函数，接收并初始化 LastMessage**

将构造函数签名从：

```cpp
    ConversationServiceImpl(const std::shared_ptr<elasticlient::Client> &es_client,
                            const std::shared_ptr<odb::core::database> &mysql_client,
                            const Members::ptr &members_cache,
                            const ServiceManager::ptr &channel_manager,
                            const std::string &identity_service_name,
                            const std::string &media_service_name,
                            const std::string &message_service_name,
                            const ConversationServiceConfig &cfg)
```

改为：

```cpp
    ConversationServiceImpl(const std::shared_ptr<elasticlient::Client> &es_client,
                            const std::shared_ptr<odb::core::database> &mysql_client,
                            const Members::ptr &members_cache,
                            const LastMessage::ptr &last_msg_cache,
                            const ServiceManager::ptr &channel_manager,
                            const std::string &identity_service_name,
                            const std::string &media_service_name,
                            const std::string &message_service_name,
                            const ConversationServiceConfig &cfg)
```

并在初始化列表加 `_last_msg_cache(last_msg_cache)`。

- [ ] **Step 3: 修改 fetch_last_message_ 加 L1 缓存读 + 回写**

修改 `fetch_last_message_` 方法，在开头加 Redis 缓存读，在末尾加缓存回写：

```cpp
    bool fetch_last_message_(brpc::Controller* in_cntl, const std::string& rid,
                             const std::string& cid, unsigned long after_seq,
                             ::chatnow::message::MessagePreview& out)
    {
        // L1: Redis 缓存
        auto cached = _last_msg_cache->get(cid);
        if (cached) {
            if (parse_preview_json_(*cached, out)) return true;
        }

        auto channel = _mm_channels->choose(_message_service_name);
        if (!channel) {
            metrics::g_degraded_message_total << 1;
            return false;
        }
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
            if (out_cntl.Failed())
                metrics::g_degraded_message_total << 1;
            return false;
        }
        const auto& m = mrsp.messages(0);
        out.set_message_id(m.message_id());
        out.set_sender_id(m.sender_id());
        out.set_message_type(m.content().type());
        out.set_sent_at_ms(m.created_at_ms());
        out.set_status(m.status());

        // 回写 Redis 缓存
        std::string preview_json = serialize_preview_json_(out);
        if (!preview_json.empty())
            _last_msg_cache->set(cid, preview_json);

        return true;
    }
```

- [ ] **Step 4: 新增 JSON 序列化/反序列化辅助方法**

在 `ConversationServiceImpl` 的 private 区新增两个方法：

```cpp
    /* brief: MessagePreview → JSON string（手动拼接，避免引入 protobuf-json 依赖） */
    static std::string serialize_preview_json_(const ::chatnow::message::MessagePreview &p) {
        std::ostringstream oss;
        oss << "{\"mid\":\"" << p.message_id() << "\""
            << ",\"sid\":\"" << p.sender_id() << "\""
            << ",\"type\":" << static_cast<int>(p.message_type())
            << ",\"ts\":" << p.sent_at_ms()
            << ",\"status\":" << static_cast<int>(p.status()) << "}";
        return oss.str();
    }

    /* brief: JSON string → MessagePreview */
    static bool parse_preview_json_(const std::string &json,
                                    ::chatnow::message::MessagePreview &out) {
        Json::Value root;
        if (!UnSerialize(json, root)) return false;
        out.set_message_id(root.get("mid", "").asString());
        out.set_sender_id(root.get("sid", "").asString());
        out.set_message_type(static_cast<::chatnow::message::MessageType>(
            root.get("type", 0).asInt()));
        out.set_sent_at_ms(root.get("ts", 0).asInt64());
        out.set_status(root.get("status", 0).asInt());
        return true;
    }
```

这两个方法需要 include `jsoncpp/json/json.h` 和 `"infra/icsearch.hpp"`（已有 `UnSerialize` 声明）。

- [ ] **Step 5: 修改 ConversationServerBuilder**

`make_rpc_object` 中构造 `ConversationServiceImpl` 时传入 `_last_msg_cache`：

```cpp
    auto *impl = new ConversationServiceImpl(
        _es_client, _mysql_client, _members_cache, _last_msg_cache, _mm_channels,
        _identity_service_name, _media_service_name, _message_service_name, _cfg);
```

`make_redis_object` 中初始化 `_last_msg_cache`：

```cpp
    _members_cache = std::make_shared<Members>(_redis_client);
    _last_msg_cache = std::make_shared<LastMessage>(_redis_client);
```

在 `ConversationServerBuilder` 的 private 成员区新增：

```cpp
    LastMessage::ptr                        _last_msg_cache;
```

- [ ] **Step 6: 提交**

```bash
git add conversation/source/conversation_server.h
git commit -m "feat(conversation): add LastMessage Redis cache for ListConversations"
```

---

### Task 6: SearchConversations — ES 冗余 member_ids

**Files:**
- Modify: `common/dao/data_es.hpp`
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: ESConversation 索引新增 member_ids 字段**

在 `ESConversation::create_index()` 中（`data_es.hpp:225-235`），新增一行：

```cpp
    bool create_index() {
        bool ret = ESIndex(_client, "chat_session")
            .append("chat_session_id",   "keyword", "standard", true)
            .append("chat_session_name")
            .append("chat_session_type", "integer", "standard", false)
            .append("avatar_id",         "keyword", "standard", false)
            .append("status",            "integer", "standard", false)
            .append("update_time",       "long",    "standard", false)
            .append("member_ids",        "keyword", "standard", false)
            .create();
        // ...
    }
```

- [ ] **Step 2: ESConversation::append_data 写入 member_ids**

在 `ESConversation::append_data` 末尾加入 member_ids 写入。`Conversation` 实体不含 member_ids，所以改签名加参数：

```cpp
    bool append_data(const chatnow::Conversation &c,
                     const std::vector<std::string> &member_ids = {}) {
        static const boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
        long ts = (c.update_time() - epoch).total_seconds();
        ESInsert builder(_client, "chat_session");
        builder.append("chat_session_id",   c.conversation_id())
               .append("chat_session_name", c.conversation_name())
               .append("chat_session_type", static_cast<int>(c.conversation_type()))
               .append("avatar_id",         c.avatar_id())
               .append("status",            static_cast<int>(c.status()))
               .append("update_time",       ts);
        if (!member_ids.empty()) {
            Json::Value mids(Json::arrayValue);
            for (const auto &uid : member_ids) mids.append(uid);
            builder.append("member_ids", mids);
        }
        bool ret = builder.insert(c.conversation_id());
        if (!ret) {
            LOG_ERROR("会话搜索数据插入/更新失败 cid={}", c.conversation_id());
            return false;
        }
        return true;
    }
```

注意 `ESInsert::append` 的模板参数需要匹配 `Json::Value`，该方法签名是 `template <typename T> ESInsert &append(const std::string &key, const T &val)`，可以接受。

- [ ] **Step 3: ESConversation 新增 update_member_ids 部分更新方法**

在 `ESConversation::search` 之后新增：

```cpp
    /* brief: 部分更新 member_ids 字段（成员变动后调用） */
    bool update_member_ids(const std::string &cid,
                           const std::vector<std::string> &member_ids) {
        Json::Value mids(Json::arrayValue);
        for (const auto &uid : member_ids) mids.append(uid);
        ESUpdate updater(_client, "chat_session");
        updater.set("member_ids", mids);
        bool ret = updater.update(cid);
        if (!ret) {
            LOG_ERROR("ES update_member_ids 失败 cid={}", cid);
            metrics::g_degraded_es_write_total << 1;
        }
        return ret;
    }
```

- [ ] **Step 4: ESConversation::search 新增 caller 过滤重载**

在原有的 `search(key, type, size)` 之后新增带 caller 过滤的重载：

```cpp
    /* brief: 搜索 + 仅返回 caller 是成员的会话（ES filter） */
    std::vector<std::string> search(const std::string &key,
                                    const std::string &caller_uid,
                                    int size = 20)
    {
        std::vector<std::string> res;
        ESSearch builder(_client, "chat_session");
        builder.append_must_match("chat_session_name", key)
               .append_must_term("status", std::to_string(0))
               .append_must_term("member_ids", caller_uid)
               .sort_by("update_time", "desc")
               .page(0, size);
        Json::Value json_session = builder.search();
        if (!json_session.isArray()) return res;
        for (int i = 0; i < (int)json_session.size(); ++i) {
            res.push_back(json_session[i]["_source"]["chat_session_id"].asString());
        }
        return res;
    }
```

- [ ] **Step 5: 修改 SearchConversations handler**

在 `conversation_server.h` 的 `SearchConversations` handler 中：

```cpp
    void SearchConversations(::google::protobuf::RpcController* base_cntl,
                             const ::chatnow::conversation::SearchConversationsReq* req,
                             ::chatnow::conversation::SearchConversationsRsp* rsp,
                             ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto cid_hits = _es_conv->search(req->search_key(), auth.user_id, 50);
            if (cid_hits.empty()) return;
            auto convs = _mysql_conv->select(cid_hits);
            for (auto &c : convs) {
                if (c.status() == ConversationStatus::DISMISSED) continue;
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
```

- [ ] **Step 6: 在成员变动操作中调 update_member_ids**

在以下 handler 中，DB 写成功后调 `update_member_ids`：

**CreateConversation** — 在 `invalidate_members_cache_` / `append_data` 之后：

```cpp
            invalidate_members_cache_(cid);
            if (!_es_conv->append_data(ent, member_ids))
                metrics::g_degraded_es_write_total << 1;
```

需要先收集 member_ids。在 handler 开头收集：

```cpp
            std::vector<std::string> all_member_ids;
            all_member_ids.push_back(auth.user_id);
            for (int i = 0; i < req->member_ids_size(); ++i)
                all_member_ids.push_back(req->member_ids(i));
```

**AddMembers** — `invalidate_members_cache_` 之后，重新取 member_ids 并更新 ES：

```cpp
            invalidate_members_cache_(req->conversation_id());
            auto updated_uids = _mysql_member->members(req->conversation_id());
            _es_conv->update_member_ids(req->conversation_id(), updated_uids);
```

**RemoveMembers** — `removed > 0` 分支内：

```cpp
            if (removed > 0) {
                invalidate_members_cache_(req->conversation_id());
                auto updated_uids = _mysql_member->members(req->conversation_id());
                _es_conv->update_member_ids(req->conversation_id(), updated_uids);
            }
```

**QuitConversation** — `invalidate_members_cache_` 之后：

```cpp
            invalidate_members_cache_(req->conversation_id());
            auto updated_uids = _mysql_member->members(req->conversation_id());
            _es_conv->update_member_ids(req->conversation_id(), updated_uids);
```

- [ ] **Step 7: 提交**

```bash
git add common/dao/data_es.hpp conversation/source/conversation_server.h
git commit -m "feat(conversation): add member_ids to ES index for server-side search filtering"
```

---

### Task 7: AddMembers 失败可见性

**Files:**
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 修改 AddMembers handler 收集失败 uid 并返回**

在 `conversation_server.h` 的 `AddMembers` handler 中，将逐成员处理改为收集失败：

```cpp
            constexpr int kGroupMemberLimit = 500;
            if (c->member_count() + req->member_ids_size() > kGroupMemberLimit)
                throw ServiceError(::chatnow::error::kConversationMemberLimit, "member limit");

            auto now = boost::posix_time::microsec_clock::universal_time();
            for (int i = 0; i < req->member_ids_size(); ++i) {
                const auto& uid = req->member_ids(i);
                auto m = _mysql_member->select_self(req->conversation_id(), uid);
                if (m && !m->is_quit()) continue;     // 已是活跃成员，跳过
                bool ok;
                if (m && m->is_quit()) {
                    ok = _mysql_member->rejoin(req->conversation_id(), uid,
                                               ::chatnow::MemberRole::NORMAL,
                                               auth.user_id,
                                               ::chatnow::JoinSource::ADMIN_ADD);
                } else {
                    ::chatnow::ConversationMember row(req->conversation_id(), uid,
                        /*muted=*/false, /*visible=*/true,
                        ::chatnow::MemberRole::NORMAL, now);
                    row.inviter_id(auth.user_id);
                    row.join_source(::chatnow::JoinSource::ADMIN_ADD);
                    ok = _mysql_member->append(row);
                }
                if (!ok) {
                    rsp->add_failed_member_ids(uid);
                    LOG_WARN("AddMembers 单个失败 cid={} uid={}", req->conversation_id(), uid);
                }
            }
            invalidate_members_cache_(req->conversation_id());
```

- [ ] **Step 2: 提交**

```bash
git add conversation/source/conversation_server.h
git commit -m "feat(conversation): return failed_member_ids in AddMembers response"
```

---

### Task 8: ListMembers 分页

**Files:**
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 修改 ListMembers handler 加分页逻辑**

在 `conversation_server.h` 的 `ListMembers` handler 中：

```cpp
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

            auto uids = _mysql_member->members(req->conversation_id());
            int total = static_cast<int>(uids.size());

            int limit = req->page().limit() > 0 ? req->page().limit() : 50;
            if (limit > 200) limit = 200;
            int cursor = 0;
            try { cursor = std::stoi(req->page().cursor()); } catch (...) { cursor = 0; }
            if (cursor < 0) cursor = 0;

            int start = std::min(cursor, total);
            int end = std::min(start + limit, total);

            std::vector<std::string> page_uids(uids.begin() + start, uids.begin() + end);
            auto rows = _mysql_member->select(req->conversation_id(), page_uids);

            UserInfoMap umap;
            (void)fetch_user_infos_(cntl, req->request_id(), page_uids, umap);

            for (auto &m : rows) {
                if (m.is_quit()) continue;
                auto* item = rsp->add_members();
                auto it = umap.find(m.user_id());
                if (it != umap.end()) item->mutable_user_info()->CopyFrom(it->second);
                else                  item->mutable_user_info()->set_user_id(m.user_id());
                item->set_role(static_cast<::chatnow::conversation::MemberRole>(m.role()));
                item->set_join_time_ms(_to_ms(m.join_time()));
            }
            rsp->mutable_page()->set_has_more(end < total);
            rsp->mutable_page()->set_next_cursor(end < total ? std::to_string(end) : "");
            rsp->mutable_page()->set_total_count(total);
        });
    }
```

- [ ] **Step 2: 提交**

```bash
git add conversation/source/conversation_server.h
git commit -m "feat(conversation): add pagination support to ListMembers"
```

---

## 验证检查点

全部任务完成后：

```bash
# 1. 编译检查
cd build && cmake .. && make -j$(nproc)

# 2. 功能测试
cd tests && go test -tags=func ./... -run "Conversation" -v

# 3. 确认无新增编译警告
```

---

## 任务依赖

```
Task1 (proto) ──┐
                ├── Task7 (AddMembers failed_member_ids 依赖 proto)
Task2           │
Task3           │
Task4           ├── 无依赖，可任意顺序
Task5           │
Task6 ──────────┤ (ES 变更独立)
Task8 ──────────┘
```

所有 task 可并行执行（除了 Task7 依赖 Task1 的 proto 变更）。
