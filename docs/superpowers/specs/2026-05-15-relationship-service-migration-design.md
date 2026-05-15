# Relationship 服务迁移设计

> **状态**: 设计完成，待评审
> **日期**: 2026-05-15
> **范围**: 旧 `friend/` 服务（`FriendServiceImpl : public FriendService`）迁到新 proto `chatnow.relationship.RelationshipService`
> **基线**: `proto/relationship/relationship_service.proto` + `2026-05-14-service-migration-design.md` §3.2 + `2026-05-14-cross-cutting-architecture-design.md` §2.6 §2.8 §5.5
> **前置**: Identity 服务迁移已落地（`IdentityServiceImpl : public chatnow::identity::IdentityService`，走 `extract_auth(cntl)` + `HANDLE_RPC` 模式）

---

## 1. 范围与目标

把 `friend/source/friend_server.h` 从老 `FriendServiceImpl : public FriendService` 迁到 `RelationshipServiceImpl : public chatnow::relationship::RelationshipService`。同步：

- **目录改名**：`friend/` → `relationship/`（与 proto 命名一致；FriendApply / Relation DAO 表名保持不变）
- 9 个 RPC 全部落实（含 3 个新增 `BlockUser` / `UnblockUser` / `ListBlockedUsers`）
- 鉴权字段全走 metadata，proto 业务体清理 `optional user_id / session_id`
- 响应模式从 `success/errmsg` 改为 `ResponseHeader`，handler 一律用 `HANDLE_RPC` 宏 + `throw ServiceError(code, msg)`
- `HandleFriendRequest` 同意分支改为调 `ConversationService.CreateConversation`（不再直接操作 `ChatSessionTable / ChatSessionMemberTable`）
- 跨服务 stub：`UserService_Stub` → `chatnow::identity::IdentityService_Stub`
- 新增 `user_block` 表 + DAO，用于"拒新好友申请 + 搜索过滤"两个场景

明确**不做**（YAGNI）：

- ❌ 不动 Conversation 服务 / proto / 表（CreateConversation 接口由 Conversation 团队提供，本服务按目标态调用即可）
- ❌ Block 不解除已有好友关系、不清理已有会话、不撤销历史消息
- ❌ 不引入 Redis（friend 历来无 Redis 依赖；遗留的 `_redis_client` 字段顺手清掉）
- ❌ 不改 ES `user` 索引结构（沿用 ESUser）
- ❌ 不改客户端 HTTP 路径（沿用 `/service/friend/*`，Gateway 内部把它路由到新 stub；HTTP 路径改名留给后续 Gateway 统一切换）

---

## 2. 服务接口与 proto 字段

### 2.1 proto 改动

`proto/relationship/relationship_service.proto`：

- 全 9 个 Req 删除 `optional string user_id` / `optional string session_id` 字段（与横切 spec §2.8 一致）
- `FriendEvent.event_id` 改为非 optional（每条申请必有）
- 业务字段保留：`HandleFriendReq.apply_user_id` / `RemoveFriendReq.peer_id` / `BlockUserReq.peer_id` / `UnblockUserReq.peer_id`

### 2.2 清理后的 RPC 与字段

| RPC | Req 关键字段 | Rsp 关键字段 |
|---|---|---|
| `ListFriends` | `request_id, page` | `header, friend_list[], page` |
| `SendFriendRequest` | `request_id, respondent_id` | `header, notify_event_id?` |
| `HandleFriendRequest` | `request_id, notify_event_id, agree, apply_user_id` | `header, new_conversation_id?` |
| `RemoveFriend` | `request_id, peer_id` | `header` |
| `SearchFriends` | `request_id, search_key` | `header, user_info[]` |
| `BlockUser` | `request_id, peer_id` | `header` |
| `UnblockUser` | `request_id, peer_id` | `header` |
| `ListBlockedUsers` | `request_id, page` | `header, blocked_list[], page` |
| `ListPendingRequests` | `request_id` | `header, event[]` |

调用方均通过 brpc Controller metadata 提供 `x-user-id / x-device-id / x-trace-id / x-jwt-jti`，由服务端 `extract_auth(cntl)` 解析。

---

## 3. 服务类与 handler 模式

### 3.1 类替换

- **新类**：`namespace chatnow::relationship { class RelationshipServiceImpl : public chatnow::relationship::RelationshipService }`
- **旧类** `FriendServiceImpl : public FriendService` 整体删除（不留兼容壳，与 Identity 迁移同模式）

### 3.2 handler 范式

与 `IdentityServiceImpl` 对齐：

```cpp
void SendFriendRequest(::google::protobuf::RpcController* cntl_base,
                       const SendFriendReq* req, SendFriendRsp* rsp,
                       ::google::protobuf::Closure* done) override {
    HANDLE_RPC(cntl, req, rsp, {
        // auth.user_id / auth.device_id / auth.trace_id 直接可用
        if (auth.user_id == req->respondent_id())
            throw ServiceError(ErrorCode::SYSTEM_INVALID_ARGUMENT, "cannot add self");
        if (_mysql_user_block->is_blocked(req->respondent_id(), auth.user_id))
            throw ServiceError(ErrorCode::RELATIONSHIP_BLOCKED, "blocked by peer");
        // ... FriendApplyTable 业务逻辑
    });
}
```

### 3.3 错误码映射（spec §5.4）

| 错误码 | 触发场景 |
|---|---|
| `RELATIONSHIP_ALREADY_FRIENDS` (2001) | `SendFriendRequest` 时 `_mysql_relation->exists(uid, pid) == true` |
| `RELATIONSHIP_NOT_FRIENDS` (2002) | `RemoveFriend` 时关系不存在 |
| `RELATIONSHIP_BLOCKED` (2003) | `SendFriendRequest` 时被对方拉黑 |
| `RELATIONSHIP_REQUEST_PENDING` (2004) | 已有 PENDING 申请未处理；或申请被拒后 72h 内重复申请（暂复用此码，后续若需区分再加新码） |
| `AUTH_USER_NOT_FOUND` (1004) | peer_id 不存在（IdentityService.GetMultiUserInfo 返回空 map 视作放过；本服务一般不主动校验） |
| `SYSTEM_UNAVAILABLE` (9002) | 下游 IdentityService / ConversationService 不可达 |

`HANDLE_RPC` 宏统一处理：`extract_auth + LogContext::set + ResponseHeader 写入 + ServiceError 捕获 + LogContext::clear`，handler 内一律 `throw ServiceError(code, msg)`，不再手填 `set_success(false) / set_errmsg(...)`。

### 3.4 现有业务规则保留

- `SendFriendRequest`：双方已是好友 → 拒；曾被拒且距离上次拒绝不足 72h → 拒；其它情况新建或复用 PENDING
- `HandleFriendRequest(agree=false)`：仅更新 `friend_apply.status = REJECTED + handle_time`
- `HandleFriendRequest(agree=true)`：详见 §5

---

## 4. DAO 层：新增 user_block 表

### 4.1 ODB schema（新文件 `odb/user_block.hxx`）

```cpp
#pragma db object table("user_block")
class UserBlock {
public:
    UserBlock() = default;
    UserBlock(std::string blocker, std::string blocked,
              boost::posix_time::ptime ct)
        : blocker_id_(std::move(blocker)),
          blocked_id_(std::move(blocked)),
          create_time_(ct) {}

    // accessors ...

private:
    friend class odb::access;
    #pragma db id auto
    unsigned long id_;
    #pragma db type("VARCHAR(32)") index
    std::string blocker_id_;
    #pragma db type("VARCHAR(32)") index
    std::string blocked_id_;
    #pragma db type("DATETIME")
    boost::posix_time::ptime create_time_;
};

#pragma db index("idx_blocker_blocked") unique members(blocker_id_, blocked_id_)
```

权威 schema 由 `--generate-schema` 生成，**不**写 `sql/V*.sql`。

### 4.2 DAO（新文件 `common/dao/mysql_user_block.hpp`，单表单文件）

| 方法 | 用途 |
|---|---|
| `bool insert(const std::string& blocker, const std::string& blocked)` | 写一行；唯一索引冲突视作幂等成功返回 true |
| `bool remove(const std::string& blocker, const std::string& blocked)` | 删除；不存在返回 true |
| `bool is_blocked(const std::string& blocker, const std::string& blocked)` | 命中即 true，给 `SendFriendRequest` 用 |
| `std::vector<std::string> list_blocked(const std::string& blocker, int offset, int limit)` | `ListBlockedUsers` 分页 |
| `int64_t count_blocked(const std::string& blocker)` | 给 `PageResponse.total` 用 |
| `std::vector<std::string> blocked_or_blocking(const std::string& uid)` | 一次合并查询：`SELECT blocked_id WHERE blocker_id=? UNION SELECT blocker_id WHERE blocked_id=?`，用于 `SearchFriends` 双向过滤 |

### 4.3 接入

- `RelationshipServiceImpl` 构造函数新增 `UserBlockTable::ptr`
- Builder `make_rpc_object` 内一并 new
- 与 `FriendApplyTable / RelationTable` 同 mysql_client，复用连接池

---

## 5. 跨服务调用

### 5.1 Identity stub 切换

| 旧 | 新 |
|---|---|
| `chatnow::UserService_Stub` | `chatnow::identity::IdentityService_Stub` |
| `rsp.success() / rsp.errmsg()` | `rsp.header().success() / rsp.header().error_message()` |

调用前 `chatnow::auth::forward_auth_metadata(in_cntl, &out_cntl)` 透传 metadata。

### 5.2 Conversation 调用（HandleFriendRequest 同意分支）

- 删除直接操作 `ChatSessionTable / ChatSessionMemberTable` 的代码
- 改调 `chatnow::conversation::ConversationService_Stub.CreateConversation`，参数 `type=SINGLE, member_ids=[uid, pid]`
- 取响应 `new_conversation_id` 回填 `HandleFriendRsp.new_conversation_id`

**失败处置**：
- 关系已写、`friend_apply` 已更新；Conversation 调用失败 → 记 ERROR 日志，`new_conversation_id` 留空，HTTP 响应 `header.success=true, error_code=OK`
- 客户端可走 ConversationService 自查或主动建单聊补救
- 现状语义本就无事务（写两次表），迁移**不引入新风险**

### 5.3 Builder 改动

- `make_discovery_object` 入参 `message_service_name` → 改为 `conversation_service_name`（friend 历来声明了 message stub 但未使用，旧 `GetRecentMsg` 已注释，顺手清理）
- 删未用字段 `_redis_client` / `_redis_client_ptr`
- 配置文件 `conf/friend_server.conf` → `conf/relationship_server.conf`，gflag `-message_service` → `-conversation_service`

### 5.4 stub 矩阵

| 调用方 | 被调 | 用途 | 鉴权透传 |
|---|---|---|---|
| RelationshipService | IdentityService.GetMultiUserInfo | 填好友列表 / 搜索结果 / 待办列表的 UserInfo | `forward_auth_metadata` |
| RelationshipService | ConversationService.CreateConversation | HandleFriendRequest 同意时建单聊 | `forward_auth_metadata` |

---

## 6. 目录与文件改动清单

### 6.1 新增

| 路径 | 内容 |
|---|---|
| `odb/user_block.hxx` | UserBlock 实体 + ODB pragma |
| `common/dao/mysql_user_block.hpp` | UserBlockTable DAO |
| `relationship/source/relationship_server.h` | RelationshipServiceImpl + RelationshipServer + Builder |
| `relationship/source/relationship_server.cc` | main 入口（gflags / etcd 注册 / 启动） |
| `relationship/CMakeLists.txt` | 编译 target `relationship_server` |
| `conf/relationship_server.conf` | gflags flagfile |

### 6.2 删除

| 路径 | 原因 |
|---|---|
| `friend/source/friend_server.h` | 整目录替换为 relationship/ |
| `friend/source/friend_server.cc` | 同上 |
| `friend/CMakeLists.txt` | 同上 |
| `conf/friend_server.conf` | 配置改名 |
| `proto/friend.proto` | 旧 flat proto 不再被任何服务使用（迁移完后全仓 grep 确认零引用再删） |

### 6.3 修改

| 路径 | 改动 |
|---|---|
| `proto/relationship/relationship_service.proto` | 删 `optional user_id / session_id`；`FriendEvent.event_id` 改非 optional |
| `CMakeLists.txt`（顶层） | `add_subdirectory(friend)` → `add_subdirectory(relationship)` |
| `gateway/source/gateway_server.h` | 6 个 friend handler 切到新 stub 与新 RPC 名；3 个新 handler（Block/Unblock/ListBlocked）按 §10 决定本期是否加 |
| `docker-compose.yml` | service 名 `friend_server` → `relationship_server`（含 image / depends_on 同步改） |

### 6.4 删除 `proto/friend.proto` 的时机

等 gateway handler 全部切换完，全仓 `grep "friend.pb"` 确认零引用再删，避免编译断裂。这一步在实施 plan 里单独占一格步骤。

---

## 7. 验收标准

每一条都需在实施 plan 里展开成可测 step：

1. `relationship_server` 启动后向 etcd 注册 `/service/relationship_service/...`，Discovery 从 gateway / chatsession 发现可用
2. 9 个 RPC 可调通（含 3 个新增）；每个 RPC 都从 metadata 取 user_id（`req.user_id` 字段在新 proto 已删）
3. `HandleFriendRequest(agree=true)` 调 ConversationService 建单聊，`new_conversation_id` 非空返回；Conversation 不可用时 fail-soft（关系已建、字段留空、记 ERROR）
4. `BlockUser` 后对方 `SendFriendRequest` 返回 `RELATIONSHIP_BLOCKED`；`SearchFriends` 不再返回拉黑双向
5. `UnblockUser` 后恢复
6. `RemoveFriend` 不影响 `user_block` 表（双向独立）
7. `proto/relationship/relationship_service.proto` 全文不含 `optional string user_id` / `optional string session_id`
8. 全仓 `grep "friend.pb" / "FriendService_Stub"` 零命中后，`proto/friend.proto` 删除生效
9. spdlog 输出含 `trace_id / user_id / device_id`（结构化 JSON）
10. 旧 `friend_server` 二进制 + `friend/` 目录已不复存在；`build/` 重新生成不出现 `friend_server` target

---

## 8. 兼容性

- **客户端 HTTP 路径不变**：沿用 `/service/friend/*`，Gateway 内部路由到新 RelationshipService stub
- **旧客户端 SDK 不受影响**：路径不变；服务端只读 metadata，proto 里被删的 `user_id` 字段就算客户端继续填也无影响
- **HTTP 路径改名**留给后续 Gateway 统一切换（`2026-05-14-service-migration-design.md` §3.8 收尾批次）

---

## 9. 工作量估算

| 模块 | 行数估 |
|---|---|
| ODB UserBlock | ~50 |
| UserBlockTable DAO | ~80 |
| RelationshipServiceImpl 9 个 handler | ~350 |
| Server / Builder / main | ~150 |
| Gateway 6 handler 替换 + 3 新增（如本期含） | ~120 |
| proto 改动 + CMakeLists 联动 | ~30 |
| **合计** | **~780** |

比 spec §七 估的 ~400 多，因为多算了 Gateway 联动与新表 DAO；**Gateway 那部分本期含或下期含都行**，由实施 plan 决定。

---

## 10. 后续工作（不在本 spec 范围）

- Gateway HTTP 路径从 `/service/friend/*` 改为 `/service/relationship/*`（与其它服务路径统一切换时一起做）
- `HandleFriendRequest` 申请被拒后冷静期错误码细化（若产品需要区分"已是好友"/"已 PENDING"/"冷静期内"）
- `BlockUser` 是否扩展到屏蔽消息推送 / 拦截会话邀请等更"微信式"的语义（当前明确不做）
