# 3.0-dev 集成测试修复总结

> commit: `a4bc80d` | 分支: `3.0-dev` | 日期: 2026-05-20

## 测试结果

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| 通过 | 57 | ~88 |
| 失败 | ~34 | ~15 |

剩余 15 个失败主要集中在:
- **消息历史/同步** (7): 依赖 transmite/ES 链条，与 transmite Redis 迁移有关
- **场景测试** (3): 依赖消息同步链路
- **媒体上传** (2): MinIO 未启动
- **消息反应/置顶** (5): 后端未实现完整
- **搜索会话** (1): ES 索引延迟

---

## 修改清单 (20 文件)

### 1. Redis Cluster 兼容 — `common/dao/data_redis.hpp`

| 修改 | 说明 |
|------|------|
| `kSeqSession` / `kSeqUser` 改为实体 hash tag | 避免所有 seq key 压到 `{seq}` 单 slot，按会话/用户分散到 Redis Cluster |
| `pipeline(hash_tag)` | 保留底层能力；seq 批量申请已改为逐 key INCR，避免跨 slot pipeline 假设 |
| `scan()` 改为 for_each | 集群模式下遍历所有节点而非单节点 |
| `eval()` 模板签名调整 | 匹配集群 API，Ret 改为 void |
| `RedisClusterFactory` 连接检查 | `cluster->ping()` → `cluster->for_each([](Redis &r) { r.ping(); })` |

### 2. 服务发现 & 基础设施

- **`common/infra/leader_election.hpp`** — etcd API v2→v3: `etcd::Transaction`→`etcdv3::Transaction`, `timetolive`→`leasetimetolive`, `KeepAlive` 构造变更
- **`common/utils/redis_mutex.hpp`** — include 路径修正
- **`presence/source/presence_server.h`** — `expire(key, 10)`→`expire(key, std::chrono::seconds(10))`

### 3. 本地环境配置 (10 个 .conf 文件)

统一修改:
- 地址: `10.0.4.10` → `127.0.0.1`
- 日志路径: `/im/logs/` → `/home/icepop/ChatNow/logs/`
- auth_config: `/im/conf/auth.json` → `/home/icepop/ChatNow/conf/auth.json`
- redis_seeds: 空 → 完整集群节点列表 `127.0.0.1:6379-6384`

### 4. 业务逻辑修复

**`conversation/source/conversation_server.h`**:
- 拒绝 `CONVERSATION_TYPE_UNSPECIFIED` (返回 kSystemInvalidArgument)
- 插入成员时跳过 `auth.user_id` 去重 (防御调用方误传)
- `CreateConversation` 响应填 `self` 字段 (role + joined_at_ms)

**`relationship/source/relationship_server.h`**:
- `HandleFriendRequest` Accept 时只传 `apply_uid` 给 conversation 服务 (caller 自动加入)
- 显式设置 `type = PRIVATE`
- 重复已存在 pending 改为抛 `kRelationshipRequestPending` 而非静默返回成功

### 5. ODB Schema

- **`odb/conversation.hxx`**: `_conversation_id` varchar(32) → varchar(48)
- **`odb/conversation_member.hxx`**: 同上

原因: 私聊 conversation_id 为 `p_<uid1>_<uid2>` (最长 41 字符)

### 6. 测试文件

- **`tests/pkg/fixture/auth.go`**: 昵称生成 `rand.Int63()`→`rand.Int63n(1000000)` (避免超 22 字符限制)
- **`tests/func/identity_test.go`**:
  - 昵称校验错误码 `9004`→`1001`
  - 登录未找到 `1004`→`1001`
  - refresh token 复用 `1008`→`1003`
  - Gateway 401 场景改用 `require.Error(t, err)` 而非检查 protobuf header
  - 重复注册测试逻辑修复 (用不同 username + 相同 nickname)
- **`tests/func/auth_middleware_test.go`**: Gateway 401 场景同上
- **`tests/func/config.yaml`**: 本地测试配置 (symlink 到 ../config.yaml)

---

## 关键设计决策

1. **Redis Cluster seq hash tag**: seq key 使用 `{conversation_id}` / `{user_id}` 实体标签分散到不同 slot；批量 user seq 逐 key `INCR` 保证跨 slot 正确性，避免 `{seq}` 单 slot 热点
2. **Conversation 服务 caller 自动加入**: 参考主流 IM (微信/Signal) 设计，创建会话时 auth user 由服务端自动添加，调用方只需传其他参与者
3. **防御式去重**: 即使调用方误传 caller uid 作为 member_id，conversation 服务也自动跳过，避免数据库唯一约束冲突
4. **不简化服务端代码**: 遵循用户指示，测试适配服务端行为而非反向

## 3.0-dev 缓存增强补充

- 成员缓存 Redis key 统一使用实体 hash tag：`members` / `sentinel` / `members_ver` / `warm lock` 均落到 `{conversation_id}`，避免 Redis Cluster 下跨 slot Lua/CAS 与锁语义漂移。
- 新项目未上线，不保留旧 key 兼容；在线路由、presence device/sub、ES outbox、Transmite L1 和 Push route L1 key 均集中到 `redis_keys.hpp` helper，并统一使用实体 hash tag 格式。
- 成员缓存读路径使用 versioned snapshot + sentinel 负缓存：读 Redis SET 前后版本一致才可信；空集合只有在 sentinel 存在时才表示“确认空”，否则回源并通过 CAS warm。
- 成员缓存 version 只接受严格无符号十进制字符串；解析失败时进入 unknown 状态，不再退化为 `0`。unknown version 会让 snapshot 不稳定，并拒绝 CAS warm / sentinel 写入，避免损坏版本 key 时旧快照覆盖缓存。
- 成员缓存 warm / sentinel / touch TTL 写入统一拒绝非正 TTL，避免 `randomized_ttl()` 把禁用/异常 TTL 偷偷抬成 1 秒并写入 Redis。
- Conversation 与 Transmite 共用 `cache_sentinel_confirms_empty()` 规则，避免一个服务命中负缓存、另一个服务绕过负缓存打 DB。
- Transmite 的本地成员 L1 缓存携带 Redis version，命中时会重新校验当前 version；只有两个 version 都已知且相等才允许命中，unknown / 不一致时主动失效并计入 stale L1 指标。
- `LocalCache(0)` 明确定义为禁用缓存，便于灰度、压测和故障绕过；正 TTL 的 `set` / `set_if_absent` 均不会写入。
- `LocalCache::set()` 覆盖同 key 时会先识别旧 entry 是否已过期，过期则按新 entry 重建并计入 expired 统计和指标回调。
