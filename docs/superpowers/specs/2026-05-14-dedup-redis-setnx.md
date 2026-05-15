# 消息幂等去重：Redis SETNX 替代 RPC 查询

> 将 Transmite 的消息去重从 RPC SelectByClientMsgId 前移到 Redis SETNX，消除 seq 浪费的竞态窗口并减少 Message 服务压力。

---

## 动机

当前三层去重：

```
Transmite → RPC SelectByClientMsgId → SNOWFLAKE+SEQ → INSERT uk_client_msg
              ↑ 竞态窗口：前一条未入库时，重试请求也能通过此层
```

竞态窗口会导致重试请求浪费一个 seq + Snowflake ID（INSERT 时被 DB 唯一索引拦住）。且每条消息多一次 RPC + DB SELECT。

---

## 方案

在 Transmite 最前端加 Redis SETNX 原子抢占，RPC 查询层移除。

### 去重 key 格式

```
im:dedup:{user_id}:{client_msg_id} → "pending" | "{message_id}"
TTL: 300s
```

### 流程

```
Transmite 收到消息
    │
    ▼
Redis SETNX(key, "pending", NX, TTL300)
    │
    ├─ 1（首次）→ SNOWFLAKE + SEQ + MQ → INSERT message
    │                                       │
    │                                       ▼
    │                                  Redis SET(key, message_id, TTL300)  回填
    │
    └─ 0（key 已存在）→ Redis GET(key)
                            │
                            ├─ "pending" → 等 5×100ms 轮询 → 再 GET
                            │                  │
                            │                  ├─ message_id → 返回旧结果
                            │                  └─ 仍 pending → 超时继续（crash 残留）
                            │
                            └─ message_id → 直接返回旧结果
```

### 关键设计决策

| 决策 | 理由 |
|---|---|
| TTL 300s | 客户端指数退避 100ms→3s，5 分钟内必然重试完毕。过期后允许新发（向后兼容空 client_msg_id） |
| 值 "pending" | 区分"处理中"和"已完成"。前请求 crash 时 pending 残留，超时后继续即可 |
| 轮询 5×100ms | 等前一个请求完成。极端情况（前请求卡死）500ms 后继续，不阻塞 |
| Redis 不可用 | SETNX 抛异常 → catch 跳过，继续正常流程。DB 唯一索引兜底 |

### 移除的组件

移除 Transmite 到 Message 的 SelectByClientMsgId RPC 调用。Message 服务不再需要处理去重查询。

---

## 收益

| | 改前（RPC 去重） | 改后（Redis SETNX） |
|---|---|---|
| 去重延迟 | ~3ms | ~0.5ms |
| Message 去重负载 | 每条消息 1 次 SELECT | **零** |
| seq 浪费 | 竞态窗口内可能 | **无**（在 seq 分配前拦截） |
| Snowflake 浪费 | 同上 | **无** |
| 新增依赖 | — | 每条消息 1 次 Redis SET |

---

## 兜底

DB 层 `uk_client_msg (user_id, client_msg_id)` 唯一索引保留，作为最终保障。覆盖：

- Redis 不可用时的穿透
- TTL 过期后的超长间隔重试
- client_msg_id 为空的旧客户端

---

## 面试话术

> 消息幂等去重用 Redis SETNX 原子操作替代了 RPC 查询。关键是时机——在 seq 和 Snowflake ID 分配之前拦截。Redis 单线程模型让抢占没有竞态窗口，不像 RPC 查询那样存在"查了没入库→重试来了也查不到"的空隙。唯一索引保留做最终兜底。SET NX 模式在这个项目里已经用了三次（租约），加第四次是同一个模式。
