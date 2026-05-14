# 消息可靠性加固：SeqGen 启动回填 & Push 链路容错

> **状态**: 设计完成，待评审
> **日期**: 2026-05-13
> **基线**: main @ `55decee`
> **范围**: SeqGen Redis 重启恢复、跨实例 PushBatch 失败容错、OnlineRoute 惰性清理、心跳重传去耦合

---

## 0. 背景

当前系统存在两类"静默丢消息"路径，都源于基础设施异常后的恢复机制缺失：

### 0.1 故障场景

| # | 故障 | 触发条件 | 后果 |
|---|------|---------|------|
| B1 | SeqGen Redis 重启 | Redis failover / 重启 / key 驱逐 | INCR 从 1 开始 → 与 DB 已有 seq 唯一约束冲突 → INSERT 失败 → 消息静默丢失 |
| B3 | 跨实例 PushBatch 失败 | Push 实例崩溃 / 网络分区 / 服务发现延迟 | 在线用户收不到消息，仅一行 LOG_WARN，无重试/outbox |
| M2 | OnlineRoute 崩溃残留 | Push 非正常退出 (kill -9 / OOM) → `unbind()` 未调用 | SET 中残留死实例条目，后续推送发给 ghost → 失败 |
| M3 | 心跳重传耦合 Message | Message 服务部署/故障 | `_on_heartbeat_resend` 依赖 GetOfflineMsg RPC，Message 不可用时 unacked 无法补送 |

### 0.2 根因共性

正常路径工作良好，但基础设施抖动时缺少自愈能力。每个问题都需要**恢复路径**：

- B1 → SeqGen 状态的持久恢复
- B3+M2 → 跨实例推送的失败兜底 + 死实例发现
- M3 → Push 本地自包含的补送能力

### 0.3 设计目标

1. Redis 重启后，Message 服务启动时自动从 DB 回填所有 session/user seq，消除消息丢失窗口
2. 跨实例 PushBatch 失败时，消息进入 CrossInstanceOutbox 定期重试，不静默丢弃
3. 跨实例失败时惰性清理 OnlineRoute 残留，不引入额外的心跳系统
4. Push 服务本地缓存最近消息，心跳重传优先命中本地缓存，仅降级时走 Message RPC

---

## 1. Part 1: SeqGen 启动回填（B1）

### 1.1 当前状态

`SeqGen::backfill_session/user()` 已在 `data_redis.hpp:236-254` 实现：

```cpp
backfill_session(ssid, base)  → GET key → if cur < base → SET key base
backfill_user(uid, base)      → GET key → if cur < base → SET key base
```

但全代码库无任何调用。`SeqGen` 对象在 Message 服务构造时创建（`message_server.h:1096` 传入），但启动时从未回填。

### 1.2 方案

Message 服务在订阅 MQ 队列**之前**，从 DB 查询当前最大 seq 值并回填 Redis：

```
MessageServerBuilder::make_rpc_object()
  │
  ├─ brpc::Server::Start(port)
  ├─ _backfill_seq_from_db()              ← 新增
  │    ├─ SELECT chat_session_id, MAX(seq_id) FROM message GROUP BY chat_session_id
  │    │     for each row: _seq_gen->backfill_session(ssid, max_seq+1)   (+1 因为 INCR 返回下一次值)
  │    └─ SELECT user_id, MAX(user_seq) FROM user_timeline GROUP BY user_id
  │          for each row: _seq_gen->backfill_user(uid, max_seq+1)
  ├─ subscribe db_queue  ← 回填完成后才开始消费
  └─ subscribe es_queue
```

**为什么在 subscribe 之前**：回填中若已有 consumer 消费消息，可能拿到旧 seq。先回填再订阅，消除窗口。

### 1.3 backfill 原子性修正

当前 `backfill_session/user` 的 GET-then-SET 在多 Message 实例并发启动时存在 TOCTOU race。

**改为 Lua 脚本**：

```lua
-- backfill_lua: 仅当 key 不存在或值小于 base 时才更新
local cur = redis.call('GET', KEYS[1])
if not cur or tonumber(cur) < tonumber(ARGV[1]) then
    redis.call('SET', KEYS[1], ARGV[1])
    return 1
end
return 0
```

替换 `data_redis.hpp` 中 `backfill_session` 和 `backfill_user` 的实现。多实例同时调用时，最终结果等价于最大值 SET，无 race。

### 1.4 性能

- 查询走覆盖索引：`SELECT session_id, MAX(seq_id) FROM message GROUP BY session_id` 在 `(session_id, seq_id)` 联合索引上只需扫描索引
- 百万消息、千会话级别：预计 < 5s
- 如果表过大：可用游标分批（每批 5000 会话），每批间 yield

### 1.5 新增代码

| 文件 | 改动 | 行数 |
|------|------|------|
| `data_redis.hpp` | `backfill_session/user` 改用 Lua 原子 | ~20 |
| `message_server.h` | `MessageServerBuilder` 新增 `_backfill_seq_from_db()` + 调用 | ~30 |
| `dao/mysql_message.hpp` | 新增 `select_max_seq_by_session()` 查询（如不存在） | ~10 |
| `dao/mysql_user_timeline.hpp` | 新增 `select_max_user_seq()` 查询（如不存在） | ~10 |

---

## 2. Part 2: Push 可靠性加固（B3 + M2 + M3）

### 2.1 跨实例 PushBatch 失败 → CrossInstanceOutbox（B3）

#### 2.1.1 当前行为

`push_server.h:143-231`：

```
Phase 2: 跨实例转发
  for each peer → PushBatch RPC (SelfDeleteRpcClosure)
    channel 不可达 → LOG_WARN → 跳过（消息丢失）
    RPC 返回失败  → on_done LOG_WARN → 跳过（消息丢失）
```

与 `DB → push_queue` 路径（有 PushOutbox 兜底）形成不对称保护。

#### 2.1.2 方案

仿照 PushOutbox 模式，新增 `CrossInstanceOutbox`。

**onPushMessage 改动**（跨实例转发失败路径）：

```
Phase 2 改为:
  for each peer_instance → PushBatch RPC
    ├─ 成功 → done
    └─ 失败:
        ├─ SREM im:online:{uid} {dead_instance}    ← 惰性清理（见 2.2）
        └─ CrossInstanceOutbox.enqueue({
              payload: InternalMessage.SerializeAsString(),
              uids:    failed_uid_list,
              peer:    target_instance,
            }, now_ts)
```

**CrossInstanceOutbox reaper**（Push 服务内，复用 PushOutbox 的 Lua 租约模式）：

```
每 5s 唤醒:
  try_acquire_lease("cross_instance_reaper", 30s)
  batch = peek(50)
  for each item:
    remove(item)
    反序列化 → internal_msg + uid_list
    重新查询 OnlineRoute 获取当前在线实例（可能已恢复/切换）
    PushBatch RPC 重试
    失败 → enqueue 回 outbox（bump 时间戳推迟下次重试）
```

**enqueue payload 结构**（Proto 新增或直接用已有）：

为最小化改动，payload 复用 `InternalMessage` 序列化；失败的 uid 子集作为附加字段存入 ZSET member。简单方案：将 `{peer}\n{size=4}{uid1_len}uid1{uid2_len}uid2...` 作为 member，`InternalMessage` 序列化存入单独的 string key 并按 member 中的 key 引用。更简单的方案：直接把全部信息编码进 JSON member：

```json
{"k":"<InternalMessage base64>","u":["uid1","uid2"],"p":"peer_instance"}
```

CrossInstanceOutbox 的操作都是取出-重投-失败-放回，payload 大小 ~2KB，JSON 开销可接受。

#### 2.1.3 与 PushOutbox 的对比

| | PushOutbox | CrossInstanceOutbox |
|---|---|---|
| 位置 | Message 服务 | Push 服务 |
| 触发 | DB→push_queue MQ 投递失败 | push→peer PushBatch RPC 失败 |
| 重试动作 | publish_confirm 到 push_queue | PushBatch RPC 到 peer |
| payload | InternalMessage 序列化 | InternalMessage + uid 子集 + target_peer |
| Redis key | `im:push:outbox` / `im:push:outbox:lock` | `im:push:cross_outbox` / `im:push:cross_outbox:lock` |
| 锁 | Lua SET NX EX | Lua SET NX EX（复用同一模式） |

### 2.2 OnlineRoute 崩溃残留 → 惰性清理（M2）

**原则**：不引入独立的心跳系统、反向映射表或定时扫描线程。在现有 PushBatch 失败路径上顺手清理。

**机制**：

```
跨实例 PushBatch 失败时（channel 不可达 / connection refused）:
  // peer 确定已死
  for each uid in failed_uid_list:
    SREM im:online:{uid} {dead_instance}
```

**为什么不需要主动心跳检测**：
- 死实例残留只在**有人给该用户发消息**时才产生失败，此时正好清理
- 不活跃用户的残留无害（没人推消息，SET 大一点无所谓）
- 如果未来需要监控告警死实例数量，CrossInstanceOutbox reaper 可统计失败 peer，达到阈值时打 LOG_ERROR

### 2.3 心跳重传去耦合 → 本地消息缓存（M3）

#### 2.3.1 当前行为

`push_server.h:282-348` 的 `_on_heartbeat_resend`：

```
peek_due unacked user_seqs
  → GetOfflineMsg RPC (Message 服务)
    → 回调中 _local_send + bump_score
```

Message 服务不可用时，RPC 全部失败，unacked 消息无法补送，ZSET 持续膨胀。

#### 2.3.2 方案

Push 服务在 `onPushMessage` 的 `_local_send` 成功后，顺手把每条 payload 写入本地 LRU 缓存。

**数据结构**（per PushServiceImpl）：

```
std::deque<CacheEntry> _evict_list;  // FIFO
std::unordered_map<std::string, decltype(_evict_list)::iterator> _msg_cache;
  // key = "{uid}:{user_seq}"
std::mutex _cache_mu;
size_t _max_entries = 5000;  // 可配置，约 25-50MB
```

**_local_send 成功后写入**：

```
lock(_cache_mu)
key = uid + ":" + std::to_string(user_seq)
if _msg_cache.contains(key):
  更新已有 entry → 移到队尾
else:
  写入 _msg_cache + push_back _evict_list
  if size > _max_entries:
    pop_front → _msg_cache.erase(oldest)
```

**_on_heartbeat_resend 优先查本地缓存**：

```
对每个 pending user_seq:
  lock(_cache_mu)
  if hit: 直接用缓存 payload _local_send，不查 RPC
  if miss: 收集到 fallback_list

if fallback_list 非空:
  走原 GetOfflineMsg RPC 降级路径（仅查未命中的 seq）
```

**效果**：正常场景（消息在几秒内未 ack，心跳 5s 触发一次重传），心跳重传 100% 命中本地缓存，完全不需要 Message RPC。仅在 Push 服务重启后（本地缓存为空）走降级路径。

**内存控制**：5000 条消息 × ~5KB payload = ~25MB，合理。必要时可按 `_evict_list` 中消息按时间戳或 TTL 过期淘汰（更精确但当前不需要）。

### 2.4 新增/改动代码

| 文件 | 改动 | 行数 |
|------|------|------|
| `data_redis.hpp` | 新增 `CrossInstanceOutbox` 类（仿 PushOutbox） | ~50 |
| `push_server.h` | onPushMessage 加 CrossInstanceOutbox 入队 + SREM 清理 | ~25 |
| `push_server.h` | PushServiceImpl 加本地消息缓存 + `_local_send` 写缓存 | ~30 |
| `push_server.h` | `_on_heartbeat_resend` 优先查本地缓存 | ~15 |
| `push_server.h` | PushServerBuilder 新增 CrossInstanceOutbox reaper 启动 | ~20 |

总计 ~140 行改动 + ~50 行新 Redis DAO 类。

---

## 3. 完整改动清单

| 文件 | Part | 改动类型 | 行数 |
|------|------|---------|------|
| `data_redis.hpp` | P1 | backfill_session/user 改用 Lua 原子 | ~20 |
| `data_redis.hpp` | P2 | 新增 CrossInstanceOutbox 类 | ~50 |
| `message_server.h` | P1 | MessageServerBuilder::_backfill_seq_from_db() + 调用 | ~30 |
| `dao/mysql_message.hpp` | P1 | select_max_seq_by_session() | ~10 |
| `dao/mysql_user_timeline.hpp` | P1 | select_max_user_seq() | ~10 |
| `push_server.h` | P2 | onPushMessage 加 outbox 入队 + SREM | ~25 |
| `push_server.h` | P2 | 本地消息缓存数据结构 + 写入 + 查询修改 | ~45 |
| `push_server.h` | P2 | CrossInstanceOutbox reaper 启动 | ~20 |

**总计**：~210 行改动，零新文件，零 proto 变更。

---

## 4. 风险与取舍

| 决策 | 取舍 |
|------|------|
| backfill 在 subscribe 之前 | 启动变慢 ~5s，但消除 seq 冲突窗口。可接受 |
| CrossInstanceOutbox 复用小 JSON payload | JSON 解析开销 <1ms（~2KB），避免新增 proto message，改动最小 |
| OnlineRoute 惰性清理而非主动心跳 | 死实例残留最长存活到下次消息发送才清理，不活跃用户无害 |
| 本地缓存 LRU 容量 5000 | Push 重启后缓存为空时退化为 RPC 路径，功能不降级 |
| CrossInstanceOutbox 全部在 Push 进程内 | Push 服务 crash 时 outbox 数据也在 Redis，reaper 由其他 Push 实例接管 |

---

## 5. 验证要点

1. **SeqGen 回填**：Redis flushall → 重启 Message → 发送消息 → 验证 seq 接续而非从 1 开始
2. **CrossInstanceOutbox**：kill 一台 Push 实例 → 从另一实例向该实例上的用户发消息 → 验证消息进入 outbox → 恢复实例 → 验证 reaper 重投成功
3. **OnlineRoute 清理**：kill -9 Push 实例 → 发消息给该实例用户 → 检查 `im:online:{uid}` 中死实例已被 SREM
4. **本地缓存命中**：发消息 → 客户端不 ack → 触发心跳重传 → 检查日志确认命中缓存而非走 GetOfflineMsg
