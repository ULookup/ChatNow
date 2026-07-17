# ES-MySQL 一致性设计

**目标：** 解决 Conversation、Identity 服务 ES 写入静默失败导致的不一致，建立统一的 ES 写入可靠性模式，为未来 CDC 迁移留好接口。

**原则：** 最终一致，ES 可落后 MySQL（秒级），但不允许永久性数据丢失。先防新增不一致，不管历史数据。

---

## 1. 现状分析

### 1.1 ES 使用分布

| 服务 | ES 索引 | 操作类型 | 写入模式 | 可靠性 |
|------|--------|---------|---------|--------|
| Message | `message` | UPSERT/DELETE | MQ 管道 + Outbox + Reaper | 好 |
| Conversation | `chat_session` | UPSERT/DELETE/PARTIAL_UPDATE | 同步 fire-and-forget | 差 |
| Identity | `user` | UPSERT | 同步 fire-and-forget（返回值被忽略） | 很差 |
| Relationship | `user` | 只读搜索 | 不写 | — |

### 1.2 问题根因

- MySQL 先 commit，后写 ES，ES 失败无法回滚
- Conversation：`append_data`/`remove`/`update_member_ids` 失败只打 bvar，RPC 正常返回
- Identity：`append_data` 返回值完全忽略，没有任何感知
- Message：MQ + DLX 重试 1 次后 `NackDiscard`，Reaper 每 5s 只拉 50 条，积压也可丢

### 1.3 一致性路径

```
RPC Handler
  → MySQL COMMIT                    // 成功，不可回滚
  → ES 直写 (retry 3x, backoff)     // 本次新增
  → 全部失败 → Redis Outbox        // 本次新增
  → Outbox Reaper (每 5s)           // 从 message 扩展到全服务
```

---

## 2. 整体架构

### 2.1 写入模型

| 服务 | 写入方式 | Outbox |
|------|---------|--------|
| Message | MQ 管道（保持现状） | Redis ESOutbox + Reaper |
| Conversation | ES Client 直写 + 3 次重试 | Redis ESOutbox + Reaper |
| Identity | ES Client 直写 + 3 次重试 | Redis ESOutbox + Reaper |
| Relationship | 只读不写 | — |

Conversation 和 Identity 写 ES 频率低（建群、加人、注册、改资料），不经过 MQ，直接写 ES。

### 2.2 重试策略

```
尝试1: ES 直写 ──失败──→ 等待 100ms
尝试2: ES 直写 ──失败──→ 等待 200ms
尝试3: ES 直写 ──失败──→ 入 Redis Outbox，打 metrics
                          ↓
               Reaper(每5s) ──成功──→ 从 Outbox 删除
                          ──失败──→ 保留，下次再试
```

### 2.3 Outbox

- **存储：** Redis Sorted Set，key 按服务隔离：`im:es:outbox:conversation`、`im:es:outbox:identity`、`im:es:outbox:message`
- **Payload：** `"index|doc_id|action|params..."` 字符串，同进程 Reaper 解析后重放对应的 ES 方法
- **排序：** score = `created_at_ms`，FIFO 顺序重放
- **Reaper：** 每个服务独立线程，etcd 选主，每 5s peek 50 条，直写 ES，成功 remove

---

## 3. 各服务改动

### 3.1 Conversation 服务

| 位置 | ES 操作 | 现状 | 改为 |
|------|--------|------|------|
| CreateConversation | UPSERT | `append_data(ent, members)` 不检查 | 重试 3 次 + Outbox |
| UpdateConversation | UPSERT | `append_data(ent)` 不检查 | 重试 3 次 + Outbox |
| DismissConversation | DELETE | `remove(cid)` 不检查 | 重试 3 次 + Outbox |
| AddMembers | PARTIAL_UPDATE | `update_member_ids(cid, uids)` | 重试 3 次 + Outbox |
| RemoveMembers | PARTIAL_UPDATE | 同上 | 重试 3 次 + Outbox |
| QuitConversation | PARTIAL_UPDATE | 同上 | 重试 3 次 + Outbox |

**新增组件：**
- `ESWriteHelper`：封装重试逻辑 `retry_write_es(lambda, outbox_key, outbox_payload)`
- 独立 Reaper 线程 + etcd leader 选举
- 独立 `elasticlient::Client` 实例给 Reaper（避免和 RPC 线程竞争）

### 3.2 Identity 服务

| 位置 | ES 操作 | 现状 | 改为 |
|------|--------|------|------|
| Register | UPSERT | `append_data(...)` 返回忽略 | 重试 3 次 + Outbox |
| UpdateProfile | UPSERT | 同上 | 同上 |

**新增组件：** 同 Conversation 的 `ESWriteHelper` + Reaper。

### 3.3 Message 服务

不改。保持现有 MQ 管道 + Outbox + Reaper。

### 3.4 Relationship 服务

不改。只读 ES 不做写入。

---

## 4. 监控

| 指标 | 来源 | 含义 |
|------|------|------|
| `g_degraded_es_write_total` | 已有 bvar | 进入 Outbox 的事件数 |
| `g_es_retry_total` | 新增 bvar | 重试发生次数（含最终成功） |
| `im:es:outbox:*` ZCARD | Redis | Outbox 积压量 |

**告警规则：**
- ZCARD > 1000 → ES 或网络异常，人为介入
- `g_degraded_es_write_total` 持续增长 → Outbox 链路也堵了

---

## 5. 线程安全

- `sw::redis++`：线程安全，Reaper 和 RPC 线程可共享 Redis Client
- `elasticlient::Client`：未确认线程安全，Reaper 使用独立 Client 实例
- `ESOutbox` 类：无锁，依赖 Redis Client 的线程安全

---

## 6. 未来 CDC 迁移

当 Canal + binlog 上线时：

1. Canal 监听业务表 binlog → 投递到现有 MQ exchange
2. MQ Consumer 端不动（接受同样的 JSON payload）
3. 应用层逐步去掉 `publish_confirm` 调用和 Outbox 写入
4. Outbox 机制保留作为 Canal 故障时的兜底

---

## 7. 不在范围内

- 历史数据对账修复
- 统一 `ESIndexEvent` proto 格式（等 CDC 时再统一）
- Message 服务链路修改
- Relationship 服务（只读不写）
- ES 索引重建工具
