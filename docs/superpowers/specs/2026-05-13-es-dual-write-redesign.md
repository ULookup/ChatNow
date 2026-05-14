# ES 双写机制重构：DB 落库后派生 ES 索引事件

> **状态**: 设计完成，待评审
> **日期**: 2026-05-13
> **基线**: main @ `55decee`
> **范围**: Message 服务的 DB/ES 双写路径、MQ 拓扑、ES outbox 兜底

---

## 0. 背景与动机

### 0.1 当前双写路径

```
Transmite.publish_confirm
  │
  ▼
chat_msg_exchange (FANOUT)
  ├─ msg_queue_db  → onDBMessage  → MySQL (message + user_timeline)
  └─ msg_queue_es  → onESMessage  → Elasticsearch (仅文本)
```

两个 consumer 平权、独立消费。Broker 把同一条消息复制两份，各自反序列化、各自失败重试、各自 DLX。

### 0.2 核心问题

FANOUT 的"平行双写"制造了四种不一致态：

| DB Consumer | ES Consumer | 结果 |
|---|---|---|
| 成功 | 成功 | 一致 |
| 成功 | 失败 | DB 有、ES 无（搜不到，但能拉到） |
| 失败 | 成功 | **ES 有、DB 无（幽灵搜索结果）** |
| 失败 | 失败 | 都无（消息丢失） |

第三种——"ES 有 DB 没有"——是最坏情况：用户搜到一条消息，点击进去拉不到原文。

### 0.3 业界共识

微信、飞书、钉钉（第三代架构）、Discord 全部采用 **"消息存储是真相源，搜索索引是异步派生视图"** 的模式。钉钉第二代曾用平行双写，后因数据不一致严重而废弃，改用 binlog CDC 方案。

### 0.4 设计目标

- **消除"ES 有而 DB 无"的路径**——ES 事件仅在 DB commit 成功后产生
- **ES 投递失败有独立兜底**——不走已有 push outbox，单独建 ES outbox + reaper
- **MQ 拓扑简化**——FANOUT 上只保留 DB 队列，ES 走独立 DIRECT 交换机

---

## 1. 架构变更

### 1.1 改后拓扑

```
Transmite.publish_confirm
  │
  ▼
chat_msg_exchange (FANOUT)          ← 只剩一个绑定
  │
  ▼
msg_queue_db → onDBMessage
                 │
                 ├─ ① 事务: INSERT message + user_timeline
                 ├─ ② commit 成功
                 │     ├─ 文本消息: publish ESIndexEvent → es_index_exchange (DIRECT)
                 │     └─ 非文本消息: 跳过
                 ├─ ③ publish_confirm → push_queue
                 └─ ④ return Ack
                       │
                       ▼
              es_index_exchange (DIRECT)
                       │
                       ▼
              msg_queue_es → onESMessage → Elasticsearch

ES 投递失败:
  publish_confirm 回调中 enqueue 到 im:es:outbox (Redis ZSET)
  → ES reaper 线程定期 peek → remove → 重投
```

### 1.2 关键原则

> **DB commit 成功是 ES 事件投递的前置条件。不存在"ES 有而 DB 无"的代码路径。**

ES 事件在 `trans.commit()` 之后才构造和投递。如果 DB 事务失败，控制流不会到达 ES 投递代码。

---

## 2. Proto 变更

### 2.1 新增 `ESIndexEvent` — `proto/base.proto`

在 `InternalMessage` 定义之后追加。这是一个轻量级事件，只携带 ES 写入所需的最小字段集：

```protobuf
// ES 索引事件（DB consumer 落库成功后投递）
// 只含 ES 写入需要的字段，不含成员列表、文件二进制、user_seq 等
message ESIndexEvent {
    int64 message_id = 1;
    string chat_session_id = 2;
    string user_id = 3;
    string content = 4;
    int64 timestamp = 5;
    uint64 seq_id = 6;
    MessageType message_type = 7;  // 仅 STRING 类型会投递此事件
}
```

与 `InternalMessage` 对比：

| 字段 | InternalMessage | ESIndexEvent |
|---|---|---|
| message_id | ✓ | ✓ |
| chat_session_id | ✓ | ✓ |
| sender.user_id | ✓ | ✓ |
| content | ✓ | ✓ |
| timestamp | ✓ | ✓ |
| seq_id | ✓ | ✓ |
| message_type | ✓ | ✓ |
| member_id_list (×N) | ✓ (~20KB) | ✗ |
| user_seqs (×N) | ✓ | ✗ |
| sender 完整信息 | ✓ | ✗ |
| client_msg_id | ✓ | ✗ |
| is_large_group | ✓ | ✗ |
| **序列化后体积** | **~20KB** | **~300B** |

---

## 3. MQ 拓扑变更

### 3.1 交换机与队列

| 组件 | 改前 | 改后 |
|---|---|---|
| `chat_msg_exchange` 绑定 | `msg_queue_db` + `msg_queue_es` | 仅 `msg_queue_db` |
| 新增 `es_index_exchange` | 无 | DIRECT 交换机 |
| `msg_queue_es` 绑定 | 绑定到 FANOUT | 绑定到 DIRECT `es_index_exchange` |
| ES 事件发布者 | Transmite（通过 FANOUT 复制） | Message.onDBMessage（DB commit 后） |

### 3.2 发布/订阅关系

| 交换机 | 类型 | 发布者 | 队列 | 消费者 |
|---|---|---|---|---|
| `chat_msg_exchange` | FANOUT | Transmite | `msg_queue_db` | onDBMessage |
| `msg_push_exchange` | DIRECT | Message.onDBMessage | `msg_push_queue` | onPushMessage |
| `es_index_exchange` | DIRECT | Message.onDBMessage | `msg_queue_es` | onESMessage |

### 3.3 过渡期双路径

上线期间，`msg_queue_es` 同时绑定到 FANOUT（旧）和 DIRECT（新）。ES consumer 改为能解析两种 proto（`ESIndexEvent` 和 `InternalMessage`），防止过渡期丢消息：

```
过渡期:
  chat_msg_exchange (FANOUT) ──── msg_queue_es  ← 旧路径，收到 InternalMessage
  es_index_exchange  (DIRECT) ─── msg_queue_es  ← 新路径，收到 ESIndexEvent
```

确认新版稳定后，删除 FANOUT 上的 `msg_queue_es` 绑定，下掉兼容代码。

---

## 4. 代码改动

### 4.1 `proto/base.proto` — 追加 `ESIndexEvent`

在 `InternalMessage` 之后追加上述 proto 定义。

### 4.2 `common/dao/data_redis.hpp` — 新增 `ESOutbox`

在现有 `PushOutbox` 类之后追加一个独立的 outbox 类。Redis 隔离于 push outbox：

```cpp
// =============================================================================
// ES 索引投递 outbox 兜底（message.onDBMessage → es_index_exchange 投递失败时持久化）
// =============================================================================

class ESOutbox
{
public:
    using ptr = std::shared_ptr<ESOutbox>;
    ESOutbox(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    void enqueue(const std::string &payload, long long score_ts) {
        try { _c->zadd(kEsOutboxKey, payload, static_cast<double>(score_ts)); }
        catch(std::exception &e) { LOG_ERROR("ESOutbox.enqueue 失败: {}", e.what()); }
    }

    std::vector<std::string> peek(long limit = 50) {
        std::vector<std::string> res;
        try {
            _c->zrange(kEsOutboxKey, 0, limit - 1, std::back_inserter(res));
        } catch(std::exception &e) { LOG_ERROR("ESOutbox.peek 失败: {}", e.what()); }
        return res;
    }

    void remove(const std::string &payload) {
        try { _c->zrem(kEsOutboxKey, payload); }
        catch(std::exception &e) { LOG_ERROR("ESOutbox.remove 失败: {}", e.what()); }
    }

    // 复用 PushOutbox 同款 Lua 租约（SET NX EX 原子化）
    bool try_acquire_reaper_lease(const std::string &owner, int ttl_sec) {
        static const char *kAcquireLua =
            "if redis.call('SET', KEYS[1], ARGV[1], 'NX', 'EX', ARGV[2]) then return 1 end "
            "if redis.call('GET', KEYS[1]) == ARGV[1] then "
            "    redis.call('EXPIRE', KEYS[1], ARGV[2]); return 1 "
            "end "
            "return 0";
        try {
            std::vector<std::string> keys = {kEsOutboxLockKey};
            std::vector<std::string> args = {owner, std::to_string(ttl_sec)};
            return _c->eval<long long>(kAcquireLua, keys.begin(), keys.end(),
                                        args.begin(), args.end()) == 1;
        } catch(std::exception &e) {
            LOG_ERROR("ESOutbox.try_acquire_reaper_lease 失败: {}", e.what());
            return false;
        }
    }

    void release_reaper_lease(const std::string &owner) {
        static const char *kReleaseLua =
            "if redis.call('GET', KEYS[1]) == ARGV[1] then "
            "    return redis.call('DEL', KEYS[1]) "
            "end "
            "return 0";
        try {
            std::vector<std::string> keys = {kEsOutboxLockKey};
            std::vector<std::string> args = {owner};
            _c->eval<long long>(kReleaseLua, keys.begin(), keys.end(),
                                args.begin(), args.end());
        } catch(std::exception &e) {
            LOG_ERROR("ESOutbox.release_reaper_lease 失败: {}", e.what());
        }
    }

private:
    std::shared_ptr<sw::redis::Redis> _c;
    static constexpr const char *kEsOutboxKey     = "im:es:outbox";
    static constexpr const char *kEsOutboxLockKey = "im:es:outbox:lock";
};
```

同时需要在 `data_redis.hpp` 的 `key` namespace 中新增两个 key 前缀常量（或在类内用 `static constexpr` 即可，上述设计直接在类内用 constexpr，避免污染 key namespace）。

### 4.3 `message/source/message_server.h` — 核心改动

#### 4.3.1 `MessageServiceImpl` 构造函数 — 新增参数

```cpp
MessageServiceImpl(/* ...现有参数... */
                   const Publisher::ptr &push_publisher = nullptr,
                   const PushOutbox::ptr &push_outbox = nullptr,
                   // 新增
                   const Publisher::ptr &es_publisher = nullptr,
                   const ESOutbox::ptr &es_outbox = nullptr)
```

#### 4.3.2 `onDBMessage` — commit 后投递 ES 事件

在事务 `commit()` 成功之后、push_queue 投递之前插入：

```cpp
// 在 trans.commit() 之后

// ---- 新增: ES 索引投递 ----
if (msg_type == MessageType::STRING && _es_publisher) {
    ESIndexEvent es_event;
    es_event.set_message_id(mid);
    es_event.set_chat_session_id(msg_info.chat_session_id());
    es_event.set_user_id(msg_info.sender().user_id());
    es_event.set_content(content);
    es_event.set_timestamp(msg_info.timestamp());
    es_event.set_seq_id(session_seq);
    es_event.set_message_type(MessageType::STRING);

    std::string es_payload = es_event.SerializeAsString();
    long long now_ts = static_cast<long long>(time(nullptr));
    auto es_outbox = _es_outbox;
    try {
        _es_publisher->publish_confirm(es_payload,
            [es_payload, es_outbox, mid, now_ts](PublishStatus st, const std::string &err) {
                if (st != PublishStatus::Acked) {
                    LOG_WARN("ES-Publisher: 投递 es_index_exchange 失败 mid={} err={}, 入 ES outbox", mid, err);
                    if (es_outbox) es_outbox->enqueue(es_payload, now_ts);
                }
            });
    } catch (std::exception &e) {
        LOG_WARN("ES-Publisher: 同步异常 mid={} err={}, 入 ES outbox", mid, e.what());
        if (_es_outbox) _es_outbox->enqueue(es_payload, now_ts);
    }
}
// ---- 新增结束 ----
```

**ES 投递是 fire-and-forget**，不阻塞 push_queue 投递，也不阻塞消息 ACK 返回。

#### 4.3.3 `onESMessage` — 解析 ESIndexEvent

```cpp
ConsumeAction onESMessage(const char *body, size_t sz, bool redelivered) {
    ESIndexEvent es_event;
    if (!es_event.ParseFromArray(body, sz)) {
        LOG_ERROR("ES-Consumer: 反序列化 ESIndexEvent 失败");
        return ConsumeAction::NackDiscard;
    }

    // 防御性检查（发布端已保证仅文本消息投递）
    if (es_event.message_type() != MessageType::STRING) {
        LOG_WARN("ES-Consumer: 收到的非文本消息 mid={}", es_event.message_id());
        return ConsumeAction::Ack;
    }

    bool ret = _es_client->appendData(
        es_event.user_id(),
        static_cast<unsigned long>(es_event.message_id()),
        es_event.timestamp(),
        es_event.chat_session_id(),
        es_event.content()
    );

    if (!ret) {
        if (redelivered) {
            LOG_ERROR("ES-Consumer: 二次失败转 DLX mid={}", es_event.message_id());
            return ConsumeAction::NackDiscard;
        }
        LOG_ERROR("ES-Consumer: 写入 ES 失败（首次），重投 mid={}", es_event.message_id());
        return ConsumeAction::NackRequeue;
    }

    LOG_DEBUG("ES-Consumer: 索引成功 mid={}", es_event.message_id());
    return ConsumeAction::Ack;
}
```

#### 4.3.4 ES Outbox Reaper

在 `MessageServiceImpl` 中新增与 PushOutbox reaper 同模式的独立 reaper：

```cpp
void start_es_outbox_reaper(const std::string &owner) {
    if (!_es_outbox || !_es_publisher) {
        LOG_WARN("ES outbox reaper 未启动：es_outbox / es_publisher 未注入");
        return;
    }
    constexpr int kReapIntervalSec = 5;
    constexpr int kLeaseTtlSec     = 30;
    constexpr int kBatchLimit      = 50;
    _es_reaper_running.store(true);
    _es_reaper_owner = owner;
    _es_reaper_thread = std::thread([this]() {
        while (_es_reaper_running.load()) {
            try {
                if (!_es_outbox->try_acquire_reaper_lease(_es_reaper_owner, kLeaseTtlSec)) {
                    std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                    continue;
                }
                auto batch = _es_outbox->peek(kBatchLimit);
                if (batch.empty()) {
                    std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                    continue;
                }
                LOG_INFO("ES outbox reaper: 取出 {} 条待重投", batch.size());
                for (const auto &payload : batch) _es_outbox->remove(payload);
                auto es_outbox = _es_outbox;
                for (const auto &payload : batch) {
                    std::string p = payload;
                    _es_publisher->publish_confirm(p,
                        [es_outbox, p](PublishStatus st, const std::string &err) {
                            if (st == PublishStatus::Acked) return;
                            LOG_WARN("ES outbox reaper 重投仍失败: {}", err);
                            if (es_outbox) es_outbox->enqueue(
                                p, static_cast<long long>(time(nullptr)));
                        });
                }
            } catch (std::exception &e) {
                LOG_ERROR("ES outbox reaper 异常: {}", e.what());
            } catch (...) {
                LOG_ERROR("ES outbox reaper 未知异常");
            }
            std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
        }
        if (_es_outbox) _es_outbox->release_reaper_lease(_es_reaper_owner);
        LOG_INFO("ES outbox reaper 已停止");
    });
}

void stop_es_outbox_reaper() {
    _es_reaper_running.store(false);
    if (_es_reaper_thread.joinable()) _es_reaper_thread.join();
}
```

#### 4.3.5 新增成员变量

```cpp
Publisher::ptr _es_publisher;
ESOutbox::ptr  _es_outbox;

// ES outbox reaper 状态
std::atomic<bool> _es_reaper_running {false};
std::thread _es_reaper_thread;
std::string _es_reaper_owner;
```

#### 4.3.6 析构函数更新

```cpp
~MessageServiceImpl() {
    stop_outbox_reaper();
    stop_es_outbox_reaper();  // 新增
}
```

### 4.4 `MessageServerBuilder` — 新增 ES publisher 构造方法

```cpp
/* brief: 构造 ES 索引 Publisher（onDBMessage 在 DB commit 后投递 ESIndexEvent） */
void make_es_publisher(const std::string &exchange,
                       const std::string &queue,
                       const std::string &binding_key)
{
    if (!_mq_client) {
        LOG_WARN("MQ 未初始化，跳过 ES publisher");
        return;
    }
    _es_pub_settings = {
        .exchange = exchange,
        .exchange_type = chatnow::DIRECT,
        .queue = queue,
        .binding_key = binding_key
    };
    _es_publisher = std::make_shared<Publisher>(_mq_client, _es_pub_settings);
}
```

`make_redis_object` 中新增：

```cpp
_es_outbox = std::make_shared<ESOutbox>(_redis);
```

`make_rpc_object` 中将 `_es_publisher`、`_es_outbox` 传入 `MessageServiceImpl` 构造函数，并启动 ES reaper：

```cpp
message_service->start_outbox_reaper(owner);
message_service->start_es_outbox_reaper(owner);  // 新增
```

### 4.5 `MessageServer::start()` — 关停顺序更新

```cpp
void start() {
    _rpc_server->RunUntilAskedToQuit();
    if (_service_impl) {
        _service_impl->stop_outbox_reaper();
        _service_impl->stop_es_outbox_reaper();  // 新增：先停 ES reaper
    }
    _mq_client.reset();
    _rpc_server->Join();
    LOG_INFO("Message 关停完成");
}
```

### 4.6 配置文件 — `conf/message_server.conf`

```ini
# ---- 改前 ----
-mq_msg_exchange=chat_msg_exchange
-mq_msg_queue_db=msg_queue_db
-mq_msg_queue_es=msg_queue_es
-mq_msg_binding_key_db=msg_queue_db
-mq_msg_binding_key_es=msg_queue_es

# ---- 改后 ----
-mq_msg_exchange=chat_msg_exchange
-mq_msg_queue_db=msg_queue_db
-mq_msg_binding_key_db=msg_queue_db

-mq_es_exchange=es_index_exchange
-mq_es_queue=msg_queue_es
-mq_es_binding_key=msg_queue_es
```

### 4.7 `message_server.cc` — main 函数

调用 builder 新方法：

```cpp
builder.make_es_publisher(
    FLAGS_mq_es_exchange,
    FLAGS_mq_es_queue,
    FLAGS_mq_es_binding_key);
```

对应新增 gflags 声明。

---

## 5. 故障分析

### 5.1 逐场景走查

| # | 故障场景 | DB 状态 | ES 状态 | 自动修复 |
|---|---|---|---|---|
| 1 | 正常路径 | ✅ | ✅ | — |
| 2 | DB 事务失败（异常）| ❌ → NackRequeue/DLX | ❌（未产生事件）| DB 重试 |
| 3 | DB commit 成功，ES publish 同步异常 | ✅ | ❌ | → ES outbox → reaper 重投 |
| 4 | DB commit 成功，ES publish 回调 Nack | ✅ | ❌ | → ES outbox → reaper 重投 |
| 5 | DB commit 成功，进程在 ES publish 前 crash | ✅ | ❌ | → 消息已落库，ES 缺失；可被对账批量补 |
| 6 | ES consumer 写入 ES 失败（首次）| ✅ | ❌ | → NackRequeue 重试 |
| 7 | ES consumer 写入 ES 失败（二次）| ✅ | ❌ | → NackDiscard 进 DLX |
| 8 | ES outbox reaper 投递失败 | ✅ | ❌ | → 重新 enqueue（更新 score），下一轮重试 |

### 5.2 不存在的不一致态

**"ES 有而 DB 无"的路径被彻底消除**：
- ES 事件在 `trans.commit()` 之后才构造和投递
- 如果事务抛异常 → catch 块中 return NackRequeue/Discard → 控制流不到达 ES 投递代码
- 如果 `object_already_persistent` 异常 → return Ack → 控制流不到达 ES 投递代码
- `ParseFromArray` 失败 → return NackDiscard → 控制流不到达后续代码

### 5.3 残余不一致（场景 5）

进程在 DB commit 成功后、ES publish 前 crash。这是唯一会导致 ES 缺失的硬 crash 场景。概率极低（commit 调用和 publish 调用之间没有 IO 等待，只有几次内存赋值和 proto 序列化）。

如果需要 100% 消除此窗口，可以将 ES 事件写入 DB 的 outbox 表（与 message 表同事务），由 reaper 扫表投递。但这引入了新的 DB 表和复杂度。当前 ES 是搜索加速层，少量缺失不致命——用户搜不到仍可通过 `GetRecentMsg` 拉取。**建议在 v2.2 阶段引入定期对账任务覆盖此场景。**

---

## 6. Redis Key 变更

| Key | 用途 | 原有 |
|---|---|---|
| `im:push:outbox` | push_queue 投递失败兜底 | ✓ |
| `im:push:outbox:lock` | push outbox reaper 租约 | ✓ |
| `im:es:outbox` | ES 索引投递失败兜底 | **新增** |
| `im:es:outbox:lock` | ES outbox reaper 租约 | **新增** |

---

## 7. 上线计划

### 7.1 兼容过渡

**阶段 1：部署新版 Message 服务**

- `msg_queue_es` 同时绑定到 FANOUT（旧）和 DIRECT（新）
- ES consumer 能解析两种 proto：
  - `ESIndexEvent` 解析成功 → 新路径
  - 回退解析 `InternalMessage` → 旧路径（兼容尚未消费完的存量 FANOUT 消息）
- Transmite **不需要改动**

**阶段 2：确认新版稳定（观察 1-2 天）**

- 确认 ES outbox 无异常堆积
- 确认 ES consumer 无 DLX 增长
- 确认 MsgSearch 功能正常

**阶段 3：清理旧路径**

- 删除 FANOUT 上 `msg_queue_es` 的绑定（`rabbitmqctl clear_binding` 或管理界面操作）
- ES consumer 中移除 InternalMessage 兼容解析代码

### 7.2 重启顺序

1. 先启动 Message（新版声明 `es_index_exchange` + 绑定 `msg_queue_es`）
2. 再启动 Transmite（不受影响，仍投递到 `chat_msg_exchange`）
3. 其余服务顺序无变化

---

## 8. 改动清单

| 文件 | 改动类型 | 改动量（估算）|
|---|---|---|
| `proto/base.proto` | 新增 `ESIndexEvent` 消息 | +12 行 |
| `common/dao/data_redis.hpp` | 新增 `ESOutbox` 类 | +80 行 |
| `message/source/message_server.h` | `MessageServiceImpl` 新增 ES publisher/outbox/reaper | +80 行 |
| `message/source/message_server.h` | `MessageServerBuilder` 新增 `make_es_publisher` | +15 行 |
| `message/source/message_server.cc` | 新增 gflags + main 调用 | +8 行 |
| `conf/message_server.conf` | MQ 配置变更 | +4 / -3 行 |
| **总计** | | **~200 行** |

---

## 9. 测试要点

- [ ] 正常文本消息：DB 落库后 ES 也落库
- [ ] 非文本消息（图片/文件/语音）：不产生 ESIndexEvent，不进 ES
- [ ] ES publisher 不可达：事件进 ES outbox
- [ ] ES outbox reaper 重投：多实例下只有一个 reaper 持锁工作
- [ ] 过渡期兼容：同一 ES consumer 解析 ESIndexEvent 和 InternalMessage
- [ ] Message 服务 crash 重启：ES outbox 中积压事件被 reaper 重投
- [ ] DB 事务失败：不产生 ESIndexEvent，不产生 ES 脏数据
- [ ] `client_msg_id` 重复消息：命中唯一索引走 Ack 分支，不产生 ESIndexEvent（正确——已有消息已经索引过）
