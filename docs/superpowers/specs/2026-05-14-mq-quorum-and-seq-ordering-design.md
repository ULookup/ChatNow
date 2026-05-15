# MQ 可靠性升级（quorum）与同会话 seq 严格有序设计

> **状态**: 设计完成，待评审
> **日期**: 2026-05-14
> **范围**: A1（消息队列可靠性升级到 quorum + 应用层配置硬化）+ A2（同会话 seq 在多实例 Message 服务下严格有序）
> **基线**: proto/ 当前 9 个域 + 现有 MQ 封装（`common/mq/rabbitmq.hpp`）+ 现有 Outbox 体系（`message_server.h`）+ `2026-05-13-message-reliability-hardening.md` + `2026-05-14-cross-cutting-architecture-design.md`
> **目标形态**: RabbitMQ 集群（≥3 节点）+ Redis 集群

---

## 0. 设计原则

1. **生产可上线** — 不是 demo，按真实 IM 上线标准
2. **不过度设计** — 已有 publisher confirm / consumer ack / Outbox / SeqGen 启动回填 / SETNX 幂等都保留，不重复发明
3. **应用层不假设单 broker / 单 Redis** — 即使 v1 部署形态待运维侧确认，应用层代码不能写出"只有单节点能跑"的逻辑
4. **差异化保护** — 只对"丢就真丢"的链路用 quorum，可恢复链路保留 classic durable + Outbox

明确**不做**的事：
- ❌ 全量 quorum（push/es 队列继续 classic）
- ❌ Transmite 端 outbox（chat_msg_queue 已是 quorum，broker 不丢；publisher confirm 失败客户端会重试）
- ❌ 自实现哈希分片（用 RabbitMQ 官方 consistent_hash_exchange 插件）
- ❌ 动态抢分片 / etcd lease（用配置式 shard_index）
- ❌ 自动 standby 接管（v1 被动等实例重启）
- ❌ 全链路 exactly-once（at-least-once + SETNX 幂等已经事实上等价）

---

## 1. 现状盘点（不重复发明）

A1+A2 的 spec 必须建立在现有能力之上。先列已有不变的部分：

| 能力 | 现状 | 本 spec 是否改动 |
|---|---|---|
| publisher confirm（4 态：Acked/Nacked/Lost/Error） | `common/mq/rabbitmq.hpp` AMQP::Reliable | 保留，新增 in-flight 上限 |
| consumer 显式 ack（ConsumeAction） | 同上 | 保留，新增同步 ack 约束 |
| Push Outbox + Reaper（confirm 失败 → 重投） | `message_server.h:710,744` | 保留 |
| ES Outbox + Reaper | `message_server.h:690,826` | 保留 |
| CrossInstance Outbox（跨 Push 实例失败重投） | `data_redis.hpp:587` | 保留 |
| SeqGen 启动回填（B1） | `2026-05-13-message-reliability-hardening.md` | 保留 |
| SeqGen Lua 原子 backfill | 同上 | 保留 |
| Redis SETNX 幂等去重 | `2026-05-14-dedup-redis-setnx.md` | 保留 |
| publisher confirm 同步异常兜底 | `transmite_server.h:280` | 保留 |
| consumer prefetch 默认 64 | `rabbitmq.hpp:45` | chat_msg_queue 改 1，其他不变 |

本 spec 仅补充上述未覆盖的两类问题：
- A1：broker 节点宕机导致 chat_msg_queue 上 in-flight 消息丢失
- A2：多实例 Message 服务并行消费导致同会话 seq 落库乱序

---

## 2. A1 — 队列可靠性差异化策略

### 2.1 队列分级

| 队列 | 类型 | 理由 |
|---|---|---|
| `chat_msg_queue.{0..K-1}` | **quorum** durable | 唯一持久化路径，丢即真丢；上游 Transmite 无 outbox |
| `push_queue` | classic **durable** | timeline 已落库 + push_outbox + 客户端重连可补；丢一条不致命 |
| `es_queue` | classic **durable** | ES 索引可重建 + es_outbox；丢一条不致命 |
| `dlx_*`（DLX 队列族） | classic **durable** | 已是异常消息，不再加 quorum |

**为什么不全 quorum**：quorum 队列吞吐约为 classic 的 1/2 ~ 2/3，磁盘占用 N 倍（写 Raft 日志到多数派 broker）。push/es 都是可恢复数据，不需要付这个代价。

### 2.2 publisher 端配置

| 参数 | 值 | 适用 |
|---|---|---|
| `delivery_mode` | **2 (persistent)** | 全部队列。非持久化消息走内存，broker 重启即丢，quorum 也救不了 |
| `mandatory` | **true** | 全部队列。路由失败时 broker 触发 `basic.return`，应用层感知"消息没进队列" |
| publisher confirm 超时 | **10 秒** | 全部队列。onLost 不无限等；超时按 Lost 处理（已有 Outbox 兜底） |
| in-flight confirm 上限 | **1024 条 / connection** | 全部队列。AMQP-CPP `Reliable` 默认无上限，高并发可能撑爆内存 |

#### 2.2.1 mandatory 失败处理

新增 `MQClient::on_return_callback`：
```cpp
using PublishReturnCallback = std::function<void(
    const std::string& exchange,
    const std::string& routing_key,
    const std::string& body,
    int reply_code,
    const std::string& reply_text)>;
```

注册在 `MQClient` 构造时；broker 触发 basic.return 时调用。
- chat_msg_queue 路由失败 → 落 Transmite 端独立的 `unroutable_outbox`（Redis ZSET），由 Transmite 启动时的小型 reaper 重投（与现有 Outbox 同模式）
- push/es 路由失败 → 已有对应 Outbox 兜底，直接进 Outbox

> 路由失败几乎只发生在"队列声明遗漏 / exchange 配置错误"等部署事故。生产中很少发生，但**没有处理就会静默丢消息**——必须有兜底。

#### 2.2.2 in-flight 上限实现

`AMQP::Reliable` 没有原生限流。在 `Publisher` 类里加计数器：
```cpp
std::atomic<int> _in_flight_count{0};
constexpr int kMaxInFlight = 1024;

void publish_confirm(...) {
    while (_in_flight_count.load() >= kMaxInFlight) {
        std::this_thread::sleep_for(1ms);   // 或 condition_variable
    }
    _in_flight_count.fetch_add(1);
    _mq_client->publish_confirm(..., [this, original_cb](PublishStatus s, const std::string& m) {
        _in_flight_count.fetch_sub(1);
        original_cb(s, m);
    });
}
```

**为什么阻塞而非拒绝**：Transmite 处理消息是同步路径（client 等响应），阻塞客户端是合理的反压；拒绝会让客户端重试反而加大压力。1024 in-flight ≈ 几百 ms 排空，对客户端可感知但不致命。

### 2.3 consumer 端配置

| 队列 | prefetch | ack 模式 | 同步约束 |
|---|---|---|---|
| `chat_msg_queue.{i}` | **1** | manual ack | 回调必须**同步**返回 ConsumeAction，不允许 post_task 异步处理 |
| `push_queue` | 64 | manual ack | 不约束 |
| `es_queue` | 64 | manual ack | 不约束 |

**chat_msg_queue prefetch=1 + 同步 ack 的强约束**：
- 保证单实例内严格按收到顺序处理 + 提交 ack
- NackRequeue 时立即归还 broker，不被 64 条 in-flight 卡住
- 同步处理意味着 DB 写也是同步的（在 callback 内完成 INSERT 后再返回 Ack）；这是 v1 设计前提，未来异步化需重新评估
- 写进代码注释 + spec：**onDBMessage 回调必须同步处理 + 同步返回 Ack。任何异步化改动需重新评估 seq 有序性**

### 2.4 队列声明参数（chat_msg_queue.{i}）

```cpp
AMQP::Table args;
args["x-queue-type"] = "quorum";
args["x-quorum-initial-group-size"] = 3;          // 3 副本（broker 集群至少 3 节点）
args["x-delivery-limit"] = 5;                     // redeliver 5 次后强制进 DLX
// 不设 x-message-ttl（消息须持久化直到消费）
// 不设 x-max-length（应用层有限流）
```

`x-delivery-limit=5` 是 quorum 特有功能。consumer NackRequeue 5 次后自动进 DLX，防止"毒消息"无限重投把 broker 打爆。classic queue 没有这个，需要应用层计数；quorum 原生支持。

push_queue / es_queue / dlx_* 保持 classic durable：
```cpp
AMQP::Table args;
// 不设 x-queue-type（默认 classic）
// passive=false durable=true exclusive=false auto_delete=false
```

### 2.5 declare_settings 结构改造

`common/mq/rabbitmq.hpp` 加字段（向后兼容）：
```cpp
struct declare_settings {
    // ... 已有字段（exchange, exchange_type, queue, binding_key, delayed_ttl_ms）

    bool        durable_queue   = true;       // 默认开
    bool        durable_message = true;       // 默认开（publish 时 delivery_mode=2）
    std::string queue_type      = "classic";  // "classic" / "quorum"
    int         delivery_limit  = 0;          // >0 时设 x-delivery-limit；仅 quorum 生效
    int         quorum_initial_group_size = 0; // >0 时设；仅 quorum 生效
    bool        mandatory       = true;       // publish 时 mandatory flag
};
```

旧调用方不传新字段则使用默认值（durable=true，type=classic，mandatory=true）。**默认值的变更影响**：
- 已有队列声明会自动变成 durable + mandatory 启用
- mandatory=true 必须有 on_return 回调注册，否则路由失败时仅 broker 端日志告警
- 部署上线前需要手动检查 `chat_msg_queue / push_queue / es_queue` 等历史队列是否已是 durable（管理 UI 看；如果是 transient 必须先停服 + 删除 + 重新声明）

`_declared` 内部使用这些字段：
```cpp
if (settings.queue_type == "quorum") {
    args["x-queue-type"] = "quorum";
    if (settings.quorum_initial_group_size > 0)
        args["x-quorum-initial-group-size"] = settings.quorum_initial_group_size;
    if (settings.delivery_limit > 0)
        args["x-delivery-limit"] = settings.delivery_limit;
}

int flags = 0;
if (settings.durable_queue) flags |= AMQP::durable;
_channel.declareQueue(queue, flags, args);
```

publish 时按 `durable_message` 设 delivery_mode；按 `mandatory` 设 publish flag。

### 2.6 broker 部署清单（应用层依赖）

应用层启动前必须满足以下 broker 状态（运维侧负责，本 spec 仅声明依赖）：

1. RabbitMQ 集群 ≥3 节点，互相加入 cluster
2. 启用插件：
   ```bash
   rabbitmq-plugins enable rabbitmq_management         # 管理 UI
   rabbitmq-plugins enable rabbitmq_consistent_hash_exchange  # A2 用
   ```
3. RabbitMQ 版本 ≥ **3.13.0**（quorum + consistent_hash_exchange + delivery-limit 兼容性下限）
4. `rabbitmqctl set_policy ha-quorum "^chat_msg_queue\." '{"queue-mode":"default"}' --apply-to queues` —— 仅占位，quorum 队列本身已 HA，无需额外 policy

写入 `docs/operations/rabbitmq-cluster.md` 作为部署清单。

### 2.7 与已有 Outbox 体系的关系

现有 Outbox 是 **broker 之后** 的兜底（Message → push/es 投递失败时）。本 spec 加 quorum 是 **broker 之前 + broker 内部** 的可靠性强化。两者互补：

```
Transmite ──(publisher confirm + quorum)──→ chat_msg_queue.{i} (quorum, broker 集群多数派持久化)
                                                     │
                                                     ▼
                                            Message 实例 i 消费
                                                     │
                                                     ├──► DB 落库（已落即"持久化"）
                                                     │
                                                     ├──► push_queue (classic durable + push_outbox 兜底)
                                                     │
                                                     └──► es_queue (classic durable + es_outbox 兜底)
```

**关键不变量**：消息一旦落 DB（chat_msg_queue 消费成功 + INSERT 成功 + ACK），就视为"持久化完成"。后续 push/es 投递失败有 Outbox 自愈，不会丢用户数据。

---

## 3. A2 — 同会话 seq 严格有序的多实例边界

### 3.1 问题陈述

Message 服务必须多实例部署（吞吐 + 可用性）。chat_msg_queue 上的 InternalMessage 是不同会话混合流，broker 默认 round-robin 给多个 consumer：
```
chat_msg_queue: [conv1#100, conv1#101, conv2#50, conv1#102, ...]
broker 分发：
  Instance A 收到 conv1#100, conv2#50
  Instance B 收到 conv1#101, conv1#102
  并行处理 → 101 可能先于 100 落库 → 读侧 SELECT seq>99 出现瞬时空洞
```

### 3.2 方案：consistent_hash_exchange 插件

不用自实现哈希。理由：

| 维度 | 自实现哈希 | consistent_hash_exchange 插件 |
|---|---|---|
| Rebalance | K 改变 → 同会话乱序，需停服迁移 | 一致性哈希环动态调整，仅少量 key 重映射 |
| Bug 风险 | hash 函数代码改动可能让同会话进不同队列 | hash 在 broker，应用层只填 routing_key |
| 维护 | 自己负责 | RabbitMQ 官方维护 |
| 代码改动 | Transmite + Message 都要改 ~100 行 | Transmite 加 5 行，Message 改队列绑定 |
| 失败模式 | 应用层算错（隐蔽） | 不可能算错 |

**唯一不能用插件的场景**：broker 不允许装插件（云托管限制 / 安全合规）。我们自部署，不存在。

### 3.3 拓扑设计

```
Exchange: chat_msg_exchange
  type:     x-consistent-hash
  durable:  true

  ─── weight=1 ──→ chat_msg_queue.0  (quorum, durable, x-delivery-limit=5)
  ─── weight=1 ──→ chat_msg_queue.1  (quorum, durable, x-delivery-limit=5)
  ─── weight=1 ──→ chat_msg_queue.2  (quorum, durable, x-delivery-limit=5)
  ─── weight=1 ──→ chat_msg_queue.3  (quorum, durable, x-delivery-limit=5)
```

**初始 K=4**。理由：
- IM 早期吞吐 ~1k msg/s
- 单 quorum queue 串行 prefetch=1 处理 ~500 msg/s（含 DB 写 + ES/push 投递）
- 4 队列 ≈ 2k msg/s 容量，留 2x 余量
- 队列数 = Message 实例数（1:1 绑定），4 个实例
- 增长到 K=8 / K=16 时无需停服（一致性哈希插件支持动态加队列）

### 3.4 Transmite publish 改造

旧：
```cpp
_publisher->publish_confirm(internal_msg.SerializeAsString(), cb);
// publish 到固定 exchange + 固定 routing_key
```

新：
```cpp
const std::string& conv_id = internal_msg.message().conversation_id();
_publisher->publish_confirm(
    "chat_msg_exchange",     // exchange
    conv_id,                 // routing_key（一致性哈希输入）
    internal_msg.SerializeAsString(),
    cb);
```

**强约束**：routing_key **必须是 conversation_id**。任何其他值（如空字符串、user_id、消息 id）都会破坏同会话有序性。写进代码注释 + spec。

### 3.5 Message 实例 ↔ 队列绑定

**配置式分片**（不做动态抢分片）：
```yaml
# Message 实例 0 配置
message:
  shard_index: 0
  shard_total: 4

# Message 实例 1 配置
message:
  shard_index: 1
  shard_total: 4

# ... 实例 2、3 同
```

部署时 4 个实例的配置不同：
- k8s StatefulSet：用 ordinal index 自动注入（`MESSAGE_SHARD_INDEX=$(echo $POD_NAME | rev | cut -d- -f1 | rev)`）
- docker-compose：手工配置 4 个 service

**实例订阅逻辑**：
```cpp
const std::string queue_name = "chat_msg_queue." + std::to_string(_config.shard_index);
_subscriber_db = MQFactory::create<Subscriber>(_mq_client, /*settings for queue_name with prefetch=1*/, _on_db_message);
```

实例**只订阅自己负责的那一个队列**。不订阅其他分片。

### 3.6 实例宕机处理（v1 被动）

实例 2 挂了 → chat_msg_queue.2 上消息堆积。
- broker 集群 quorum + durable 保证消息不丢
- v1 等待实例 2 重启，重启后继续消费
- 影响：1/4 流量延迟（其他 3/4 不受影响）
- v2 路径登记：standby 实例自动接管（需 etcd lease + 配置热更新）—— 本 spec 不实施

**v1 接受这个延迟**的前提：
- 部署侧使用进程管理工具（systemd/supervisor/k8s）保证实例自动重启
- Outbox Reaper 仍然每个实例一份，Reaper 内部 Redis 分布式锁（已有）保证同时只有一个实例 reap

### 3.7 Rebalance 行为（K 调整时）

当 K 从 4 加到 8：
1. 通过 broker 管理 UI 或 `rabbitmqadmin` 新建 chat_msg_queue.4 ~ .7（quorum durable）
2. 绑定到 chat_msg_exchange，weight=1
3. 一致性哈希环自动调整，约 50% conv_id 重映射（虚拟节点 100x 后实际 ~12.5%）
4. 新部署 4 个 Message 实例（shard_index=4..7）
5. 老实例继续消费各自队列，自然消化完旧消息

**Rebalance 瞬间的有序性**：理论上同 conv_id 在哈希切换的极短窗口内，新发送的消息可能进新队列、刚发送的可能还在旧队列里没消费完——**最坏情况是同会话相邻两条消息相对顺序错一次**。客户端按 seq_id 排序展示，user_seq 单调，所以最终展示仍然按 seq 排序，仅"持久化时序"短暂错位，对用户无感知。

**写进 spec 作为已知行为**：rebalance 期间不做"宣称同会话严格有序"的强承诺，但承诺"最终展示按 seq 严格有序（客户端排序）"。

### 3.8 ACK 顺序约束（强制）

chat_msg_queue 的 onDBMessage 回调内：
```cpp
ConsumeAction onDBMessage(const char* body, size_t len, bool /*redelivered*/) {
    InternalMessage msg;
    if (!msg.ParseFromArray(body, len)) return ConsumeAction::NackDiscard;

    // ... DB 写 + push_publisher.publish_confirm + es_publisher.publish_confirm
    // 全部同步完成后才 return Ack

    return ConsumeAction::Ack;  // 同步返回，不 post_task
}
```

写入代码注释（`message_server.h` onDBMessage）和 spec 章节：
> 本回调必须同步处理 + 同步返回 ConsumeAction。任何异步化改动（post_task 到独立线程 / 协程化 / DB 写异步化）都会破坏 prefetch=1 提供的"单实例内同会话有序"保证，需要重新评估 seq 有序性方案（如引入 multiple ack 或 channel 级串行队列）。

---

## 4. 变更清单

### 4.1 proto 改动

无。本 spec 完全在传输层（MQ 配置 + 消费拓扑），不改 proto。

### 4.2 代码改动

| 文件 | 改动 | 估算行数 |
|---|---|---|
| `common/mq/rabbitmq.hpp` | declare_settings 加 6 字段；`_declared` 按字段构建 args；publish 按 durable_message 设 delivery_mode；publish 按 mandatory flag；新增 on_return 回调注册；Publisher 加 in_flight 计数器 + 阻塞反压 | ~150 |
| `common/mq/rabbitmq.hpp` | publisher confirm 加 10s 超时（计时器 + 触发 onLost） | ~30 |
| `transmite/source/transmite_server.h` | publish 改为 `chat_msg_exchange` + routing_key=conversation_id；exchange/queue 声明改为 quorum；新增 unroutable_outbox（Redis ZSET）+ 启动时 reaper | ~80 |
| `message/source/message_server.h` | chat_msg_queue 声明改为 quorum + 4 分片；订阅 `chat_msg_queue.{shard_index}`；onDBMessage 同步约束注释；prefetch=1 | ~60 |
| `push/source/push_server.h` | 队列声明加 durable=true（已是 durable 则无改动）；mandatory + on_return 处理 | ~20 |
| `common/dao/data_redis.hpp` | 新增 UnroutableOutbox 类（与 PushOutbox 同模式） | ~80 |
| 配置文件 | 4 实例的 shard_index/shard_total | ~10 |
| `docker-compose.yml` | RabbitMQ 集群配置 + 启用插件（运维侧） | 运维 |

### 4.3 部署侧依赖

写入 `docs/operations/rabbitmq-cluster.md`：
- RabbitMQ 集群 ≥3 节点
- 启用插件：management + consistent_hash_exchange
- RabbitMQ 版本 ≥3.13.0
- 4 个 chat_msg_queue.{0..3} 队列预先声明（应用层启动时也会幂等声明，但建议运维提前建好）
- chat_msg_exchange 类型 x-consistent-hash

写入 `docs/operations/redis-cluster.md`：
- Redis 集群 / Sentinel 模式
- 应用层使用 sw::redis::RedisCluster 或 Sentinel client（v1 spec 不约束具体实现，但代码不能假设单节点）
- 关键 key（SeqGen / Outbox / Lease 等）确保哈希到合适的 slot

### 4.4 测试

集成测试（必须）：
1. **broker 单节点宕机**：3 节点集群中关 1 个，chat_msg_queue 仍可读写，已 publish 但未消费的消息不丢
2. **publisher mandatory 失败**：删除 chat_msg_exchange 的某个 binding，publish 触发 on_return → 消息进 unroutable_outbox → reaper 重投
3. **同会话有序**：同 conv_id 连发 100 条 → 4 个 Message 实例消费 → DB 中 seq 严格递增（用 `LAG()` 窗口函数验证无空洞）
4. **跨会话并行**：50 个 conv_id × 20 条 → 4 实例并行消费 → 每个 conv_id 内严格有序
5. **实例宕机**：杀死 Message 实例 2 → chat_msg_queue.2 堆积 → 重启实例 2 → 消息全部消费
6. **prefetch=1 验证**：发 10 条到同一 chat_msg_queue.{i} → 同时刻 broker unacked count 始终 ≤ 1
7. **delivery_limit=5**：一条消息持续 NackRequeue → 第 6 次进 DLX

---

## 5. 与已有 spec 的衔接

| 已有 spec | 关系 |
|---|---|
| `2026-05-13-message-reliability-hardening.md` | SeqGen 启动回填仍然必要（Redis 重启时回填）。本 spec 不改 |
| `2026-05-14-dedup-redis-setnx.md` | SETNX 幂等去重仍然必要（quorum 是 at-least-once，重复消费可能发生）。本 spec 不改 |
| `2026-05-13-cache-strategy-redesign.md` | 无关 |
| `2026-05-13-es-dual-write-redesign.md` | es_queue 仍 classic durable + es_outbox，本 spec 不改 ES 双写策略 |
| `2026-05-14-cross-cutting-architecture-design.md` | 横切基础设施 spec；本 spec 在 trace_id 透传到 MQ headers 这一点上对齐（已规定）。无冲突 |
| `2026-05-14-proto-redesign.md` | 无关 |

---

## 6. 实施顺序建议

1. `common/mq/rabbitmq.hpp` 改造（declare_settings 字段 + on_return + in_flight 上限 + 10s 超时）
2. UnroutableOutbox 实现（`data_redis.hpp`）
3. broker 集群拉起 + 插件启用（运维侧前置）
4. 预先声明 chat_msg_exchange + 4 quorum 队列（运维侧或应用层启动时幂等声明）
5. Transmite 改造（publish 到 chat_msg_exchange，routing_key=conv_id）
6. Message 改造（订阅 chat_msg_queue.{shard_index}，prefetch=1，同步 ack）
7. push/es 队列加 durable + mandatory + on_return（不改 quorum）
8. 集成测试 7 项跑通
9. 上线灰度（先 1 个实例，再 4 个）

---

## 7. 验收标准

代码验收：
- [ ] declare_settings 6 个新字段在 `rabbitmq.hpp`
- [ ] Publisher 有 in_flight 计数器和阻塞反压
- [ ] publish_confirm 有 10s 超时（onLost 触发）
- [ ] mandatory=true 默认开 + on_return 回调可注册
- [ ] UnroutableOutbox + reaper 实现并接入 Transmite
- [ ] chat_msg_exchange 是 x-consistent-hash 类型
- [ ] chat_msg_queue.{0..3} 是 quorum + durable + x-delivery-limit=5
- [ ] Transmite publish routing_key 严格等于 conversation_id（含代码注释说明）
- [ ] Message 实例按 shard_index 配置只订阅一个分片
- [ ] onDBMessage 回调同步处理 + 同步 ack（含代码注释强约束）
- [ ] push_queue / es_queue 是 classic durable + mandatory

测试验收：
- [ ] 7 项集成测试全部通过

文档验收：
- [ ] `docs/operations/rabbitmq-cluster.md` 写入部署清单
- [ ] `docs/operations/redis-cluster.md` 写入部署清单

---

## 8. 后续路径（不在本 spec 实施）

- v2: K 从 4 扩到 8 的 rebalance runbook
- v2: standby Message 实例自动接管（etcd lease）
- v3: chat_msg_queue 单实例吞吐 >2k msg/s 时考虑改 multiple ack（需重新评估同步约束）
- v3: 跨地域 broker federation（多地域 IM）
