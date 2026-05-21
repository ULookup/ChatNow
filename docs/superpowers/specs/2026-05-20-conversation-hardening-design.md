# Conversation 服务生产加固设计

> **日期**: 2026-05-20
> **基线**: feat/redis-cluster-fix (3.0-dev)
> **范围**: Conversation 服务 7 项缺陷修复 + 性能优化
> **来源**: conversation 服务 deep review

---

## 0. 设计目标

将 conversation 服务从"暗降级、弱一致性"提升到生产就绪。覆盖一致性、安全性、可观测性、性能四个维度。

---

## 1. 修复清单

| # | 问题 | 级别 | 改动文件 |
|---|------|------|----------|
| 1 | `TransferOwner` 三步非原子 | Critical | `mysql_conversation_member.hpp`, `conversation_server.h` |
| 2 | `CreateConversation` PRIVATE 幂等路径缺成员校验 | Important | `conversation_server.h` |
| 3 | fail-soft 路径缺 metrics | Important | 新增 `common/infra/metrics.hpp`, `conversation_server.h` |
| 4 | `ListConversations` N+1 RPC 调 Message | Improvement | `conversation_server.h` |
| 5 | `SearchConversations` N+1 DB 查成员 | Improvement | `data_es.hpp`, `conversation_server.h` |
| 6 | `AddMembers` 部分失败无感知 | Improvement | `conversation_server.h`, proto |
| 7 | `ListMembers` 无分页 | Improvement | `conversation_server.h` |

---

## 2. TransferOwner 原子化

### 问题

`update_role(newOwner, OWNER)` / `update_role(oldOwner, ADMIN)` / `select+update owner_id` 三个 DAO 调用各自独立事务，中间失败导致群有两个 OWNER 或不一致。

### 方案

`ConversationMemberTable` 新增 `transfer_owner` 方法，一个 DB 事务内 `FOR UPDATE` + 原子写入：

```cpp
// common/dao/mysql_conversation_member.hpp
bool transfer_owner(const std::string &cid,
                    const std::string &old_owner_id,
                    const std::string &new_owner_id) {
    odb::transaction trans(_db->begin());

    using query = odb::query<ConversationMember>;
    auto m1 = _db->query_one<ConversationMember>(
        (query::conversation_id == cid && query::user_id == old_owner_id) + " FOR UPDATE");
    auto m2 = _db->query_one<ConversationMember>(
        (query::conversation_id == cid && query::user_id == new_owner_id) + " FOR UPDATE");

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
}
```

### Handler 简化

`conversation_server.h` 的 `TransferOwner` handler 从三步 DAO 调用改为一步：

```cpp
if (!_mysql_member->transfer_owner(req->conversation_id(), auth.user_id, req->new_owner_id()))
    throw ServiceError(::chatnow::error::kSystemInternalError, "transfer_owner failed");
```

权限校验保留在 handler 层（`role_of_ == OWNER`、target 必须是成员）。

---

## 3. CreateConversation PRIVATE 幂等路径成员校验

### 问题

已有 PRIVATE 会话时，`CreateConversation` 幂等返回 cid 但不校验 caller 是否为该会话成员。第三方可通过 `CreateConversation(PRIVATE, member_ids=[B])` 反推 A 和 B 的单聊存在性。

### 方案

幂等返回前增加 `require_member_` 检查：

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
}
```

`require_member_` 是一次 DB 查询，在幂等路径上可接受（PRIVATE 创建本来就要查 `exists`）。

---

## 4. 降级 Metrics 埋点

### 问题

`fetch_user_infos_` / `fetch_last_message_` / ES 写入失败时静默降级，既没有协议信号也没有 metrics，运维无法感知。

### 方案

新增 `common/infra/metrics.hpp`，基于 brpc bvar（thread-local 无锁 counter）：

```cpp
#pragma once
#include <bvar/bvar.h>

namespace chatnow::metrics {
    inline bvar::Adder<long> g_degraded_identity_total;
    inline bvar::Adder<long> g_degraded_message_total;
    inline bvar::Adder<long> g_degraded_es_write_total;
}
```

埋点位置：

| 方法 | 条件 | 埋点 |
|------|------|------|
| `fetch_user_infos_` | 返回 false | `metrics::g_degraded_identity_total << 1` |
| `fetch_last_message_` | 返回 false | `metrics::g_degraded_message_total << 1` |
| `_es_conv->append_data/remove` | 返回 false | `metrics::g_degraded_es_write_total << 1` |

Grafana 告警规则：5 分钟内 `degraded_*_total > 100` → 通知 oncall。

**Proto 不改动**。降级对用户透明（客户端用本地缓存兜底），对运维可见。

---

## 5. ListConversations N+1 RPC 优化

### 问题

`fetch_last_message_` 对每个会话发一次 `MessageService.SyncMessages` RPC。200 个会话 = 200 次 RPC。

### 方案

使用已有 `LastMessage` Redis 缓存（`common/dao/data_redis.hpp:492`）做 L1 缓存：

```cpp
// L1: Redis 缓存
auto cached = _last_msg_cache->get(cid);
if (cached && parse_preview_json(*cached, out)) return true;

// L2: RPC 回源
// ... 现有 MessageService.SyncMessages 逻辑 ...

// 回写缓存
if (ok) _last_msg_cache->set(cid, serialize_preview(out));
```

### 依赖

- `ConversationServiceImpl` 构造函数新增 `_last_msg_cache` 成员（`LastMessage::ptr`）
- `ConversationServerBuilder::make_redis_object` 中通过已有 `_redis_client` 构造 `LastMessage`
- Message 服务后续负责在写入新消息时 `LastMessage.set(cid, preview_json)`（本次范围外）

### 序列化

`MessagePreview` ↔ JSON 序列化用 protobuf `SerializeAsString` + base64，或简单手动拼接 JSON 字段（避免引入新依赖）。

---

## 6. SearchConversations ES 冗余 member_ids

### 问题

当前 ES 搜索返回 cid_hits 后，逐条 DB 查 `require_member_`，N+1 查询。

### 方案

ES `chat_session` 索引冗余 `member_ids` 字段，搜索时一步过滤 caller 是成员的会话。

**索引改动**（`data_es.hpp`）：

```cpp
// create_index() 新增
.append("member_ids", "keyword", "standard", false)
```

**写入路径**——成员集合变动时更新 ES `member_ids`（部分更新，不重写全文档）：

| 操作 | 触发点 |
|------|--------|
| `CreateConversation` | `append_data(ent)` 之后 |
| `AddMembers` | for 循环结束后 |
| `RemoveMembers` | `removed > 0` 分支内 |
| `QuitConversation` | `set_quit` 成功后 |
| `DismissConversation` | 不变（直接删文档） |

**读取路径**——`search()` 加 `must term: member_ids = caller_uid` filter，零次 DB 查询：

```cpp
auto cid_hits = _es_conv->search(req->search_key(), auth.user_id, 50);
auto convs = _mysql_conv->select(cid_hits);
for (auto &c : convs) {
    if (c.status() == ConversationStatus::DISMISSED) continue;
    // 填 proto...
}
```

### 一致性问题

ES 写入失败不阻塞主流程（与现有模式一致）。极端情况 ES member_ids 落后于 DB，搜索结果短暂缺失。一致性优化后续单独处理。

---

## 7. AddMembers 部分失败感知

### 问题

`AddMembers` 逐成员 best-effort 处理，但失败信息不返回给调用方，客户端无感知。

### 方案

**Proto 改动**：`AddMembersRsp` 新增 `repeated string failed_member_ids`：

```proto
message AddMembersRsp {
    common.ResponseHeader header = 1;
    repeated string failed_member_ids = 2;
}
```

**Handler 改动**：单个失败时追加到 rsp + 打 WARN 日志：

```cpp
if (m && m->is_quit()) {
    ok = _mysql_member->rejoin(...);
} else {
    ok = _mysql_member->append(row);
}
if (!ok) {
    rsp->add_failed_member_ids(uid);
    LOG_WARN("AddMembers 单个失败 cid={} uid={}", req->conversation_id(), uid);
}
```

**语义**：best-effort，不回滚整批。客户端根据 `failed_member_ids` 提示用户。

---

## 8. ListMembers 分页

### 问题

`has_more = false` 写死，500 人群一次性返回所有成员。

### 方案

`ListMembersReq` / `ListMembersRsp` 已有 `PageRequest` / `PageResponse` 字段。Handler 内加 offset 分页逻辑：

```cpp
int limit = req->page().limit() > 0 ? req->page().limit() : 50;
if (limit > 200) limit = 200;

auto uids = _mysql_member->members(req->conversation_id());
int total = static_cast<int>(uids.size());
int start = req->page().cursor();  // cursor 即 offset
int end = std::min(start + limit, total);

std::vector<std::string> page_uids(uids.begin() + start, uids.begin() + end);
auto rows = _mysql_member->select(req->conversation_id(), page_uids);

// 填 members...

rsp->mutable_page()->set_has_more(end < total);
rsp->mutable_page()->set_total_count(total);
```

光标使用 offset 模式。群成员变动频率低，offset 位移偏差可接受。

---

## 9. 不做（YAGNI）

- Proto `ResponseHeader` 不加 `degraded` 标志（降级对用户透明、对运维可见）
- 不新增服务间 RPC
- 不新增 Redis 数据结构
- `AddMembers` 不加 DB 整体回滚（best-effort 是正确语义）
- ES member_ids 一致性补偿不在本次范围
- `list_ordered_by_user` 裸 SQL 拼接不重构（已有 `_escape_id` 防注入，够用）

---

## 10. 测试

| 测试点 | 类型 | 描述 |
|--------|------|------|
| TransferOwner 并发 | 功能 | 两个请求同时转让，验证只有一个生效，role 一致 |
| TransferOwner 非 OWNER | 功能 | 成员调 TransferOwner 被拒 |
| CreateConversation 幂等非成员 | 功能 | 第三方调已有 PRIVATE 会话被拒 |
| AddMembers failed_member_ids | 功能 | 部分失败返回 failed_member_ids 列表 |
| ListMembers 分页 | 功能 | limit=10 返回 10 条 + has_more=true |
| ListConversations last_message 缓存 | 功能 | 缓存命中时不调 Message RPC |
| SearchConversations 成员过滤 | 功能 | 仅返回 caller 是成员的会话 |
| 降级 metrics | 手工 | 停 Identity/Message/ES，确认 counter 递增 |
