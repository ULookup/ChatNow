# ES 双写机制重构 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 ES 索引从 FANOUT 平行双写重构为 DB commit 后派生投递，消除"ES 有而 DB 无"的幽灵搜索结果。

**Architecture:** DB commit 成功是 ES 事件投递的前置条件——onDBMessage 事务提交后在同一个回调中构造轻量级 ESIndexEvent 并 publish 到独立 DIRECT 交换机，ES consumer 解析 ESIndexEvent 写入。投递失败由独立 ESOutbox (Redis ZSET + Lua 租约) 兜底。

**Tech Stack:** C++17, protobuf, ODB ORM (MySQL), Redis (sw::redis++), brpc, AMQP-CPP (RabbitMQ), Elasticsearch

---

## 文件结构

| 文件 | 职责 | 改动类型 |
|------|------|---------|
| `proto/base.proto` | 新增 `ESIndexEvent` 消息（~300B vs ~20KB InternalMessage） | 新增消息 |
| `common/dao/data_redis.hpp` | 新增 `ESOutbox` 类（Redis ZSET + Lua 租约，独立于 PushOutbox） | 新增类 |
| `message/source/message_server.h` | 核心改动：构造注入、onDBMessage 加 ES 投递、onESMessage 改解析、ES reaper | 多处修改 |
| `message/source/message_server.cc` | 新增 gflags + builder 调用 | 少量修改 |
| `conf/message_server.conf` | MQ 拓扑配置变更 | 配置修改 |

---

## Part 1: Proto + Redis DAO

### Task 1: proto/base.proto — 新增 ESIndexEvent 消息

**Files:**
- Modify: `proto/base.proto`（在 `InternalMessage` 之后，line 167 后追加）

- [ ] **Step 1: 追加 ESIndexEvent 消息定义**

在 `InternalMessage` 定义末尾（line 167 `}` 后）追加：

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
    MessageType message_type = 7;
}
```

- [ ] **Step 2: 验证 proto 编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow && protoc --cpp_out=. proto/base.proto 2>&1 | head -10
```

- [ ] **Step 3: Commit**

```bash
git add proto/base.proto
git commit -m "feat: proto 新增 ESIndexEvent 轻量级索引事件消息"
```

---

### Task 2: data_redis.hpp — 新增 ESOutbox 类

**Files:**
- Modify: `common/dao/data_redis.hpp`（在 `PushOutbox` 类之后追加）

- [ ] **Step 1: 在 PushOutbox 类之后追加 ESOutbox 类**

在 `PushOutbox` 类的 `};` 之后（CrossInstanceOutbox 之后）追加：

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

- [ ] **Step 2: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "feat: 新增 ESOutbox 类——ES 索引投递失败兜底"
```

---

## Part 2: MessageServiceImpl 核心改动

### Task 3: MessageServiceImpl 构造函数 + 成员变量 + 析构函数更新

**Files:**
- Modify: `message/source/message_server.h:33-67`

- [ ] **Step 1: 更新构造函数签名，新增 ES publisher 和 ES outbox 参数**

将构造函数签名（line 33-45）改为：

```cpp
    MessageServiceImpl(const std::string &file_service_name,
                        const std::string &user_service_name,
                        const std::string &chatsession_service_name,
                        const ServiceManager::ptr &channels,
                        const std::shared_ptr<elasticlient::Client> &es_client,
                        const std::shared_ptr<odb::core::database> &mysql_client,
                        const std::shared_ptr<Subscriber> &mq_subscriber,
                        const std::shared_ptr<Subscriber> &es_subscriber,
                        const declare_settings &db_settings,
                        const declare_settings &es_settings,
                        const SeqGen::ptr &seq_gen,
                        const Publisher::ptr &push_publisher = nullptr,
                        const PushOutbox::ptr &push_outbox = nullptr,
                        const Publisher::ptr &es_publisher = nullptr,
                        const ESOutbox::ptr &es_outbox = nullptr)
```

- [ ] **Step 2: 更新初始化列表**

在当前初始化列表末尾 `_push_outbox(push_outbox)` 之后追加：

```cpp
                        _es_publisher(es_publisher),
                        _es_outbox(es_outbox)
```

- [ ] **Step 3: 在 private 成员变量区新增 ES 成员**

在现有 `PushOutbox::ptr _push_outbox;` 之后（line 904）追加：

```cpp
    Publisher::ptr _es_publisher;  // DB commit 后向 es_index_exchange 投递 ESIndexEvent
    ESOutbox::ptr  _es_outbox;     // ES 索引投递失败兜底
```

- [ ] **Step 4: 更新析构函数**

将析构函数（line 65-67）更新为同时停止两个 reaper：

```cpp
    ~MessageServiceImpl() {
        stop_outbox_reaper();
        stop_es_outbox_reaper();
    }
```

- [ ] **Step 5: Commit**

```bash
git add message/source/message_server.h
git commit -m "feat: MessageServiceImpl 构造函数新增 ES publisher/outbox 参数"
```

---

### Task 4: onDBMessage — DB commit 后投递 ESIndexEvent

**Files:**
- Modify: `message/source/message_server.h:662-686`（trans.commit() 之后）

- [ ] **Step 1: 在 trans.commit() 之后、push 投递之前插入 ES 事件投递**

在 `trans.commit()` 的 LOG_INFO 之后（line 665 后）、push publish 之前（line 667）插入：

```cpp
            // 6. DB commit 成功后投递 ES 索引事件（仅文本消息）
            //    失败 → 落 ESOutbox（Redis ZSET），由独立 reaper 定期重投
            if(msg_type == MessageType::STRING && _es_publisher) {
                ESIndexEvent es_event;
                es_event.set_message_id(static_cast<int64_t>(mid));
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
                            if(st != PublishStatus::Acked) {
                                LOG_WARN("ES-Publisher: 投递 es_index_exchange 失败 mid={} err={}, 入 ES outbox", mid, err);
                                if(es_outbox) es_outbox->enqueue(es_payload, now_ts);
                            }
                        });
                } catch(std::exception &e) {
                    LOG_WARN("ES-Publisher: 同步异常 mid={} err={}, 入 ES outbox", mid, e.what());
                    if(_es_outbox) _es_outbox->enqueue(es_payload, now_ts);
                }
            }
```

并更新后续 push_queue 投递的注释序号（6 → 7）。

- [ ] **Step 2: Commit**

```bash
git add message/source/message_server.h
git commit -m "feat: onDBMessage 在 DB commit 后投递 ESIndexEvent，消除 ES 有而 DB 无的不一致"
```

---

### Task 5: onESMessage — 改为解析 ESIndexEvent + 兼容 InternalMessage

**Files:**
- Modify: `message/source/message_server.h:762-806`（onESMessage 方法体）

- [ ] **Step 1: 重写 onESMessage，兼容两种 proto**

将 `onESMessage` 方法体替换为：

```cpp
    ConsumeAction onESMessage(const char *body, size_t sz, bool redelivered) {
        // 1. 先尝试解析 ESIndexEvent（新路径）
        ESIndexEvent es_event;
        if(es_event.ParseFromArray(body, sz)) {
            if(es_event.message_type() != MessageType::STRING) {
                LOG_WARN("ES-Consumer: 收到非文本 ESIndexEvent mid={}", es_event.message_id());
                return ConsumeAction::Ack;
            }
            bool ret = _es_client->appendData(
                es_event.user_id(),
                static_cast<unsigned long>(es_event.message_id()),
                es_event.timestamp(),
                es_event.chat_session_id(),
                es_event.content()
            );
            if(!ret) {
                if(redelivered) {
                    LOG_ERROR("ES-Consumer: ESIndexEvent 二次失败转 DLX mid={}", es_event.message_id());
                    return ConsumeAction::NackDiscard;
                }
                LOG_ERROR("ES-Consumer: ESIndexEvent 写入 ES 失败（首次），重投 mid={}", es_event.message_id());
                return ConsumeAction::NackRequeue;
            }
            LOG_DEBUG("ES-Consumer: ESIndexEvent 索引成功 mid={}", es_event.message_id());
            return ConsumeAction::Ack;
        }

        // 2. 回退兼容旧路径 InternalMessage（过渡期 FANOUT 残留消息）
        chatnow::InternalMessage internal_msg;
        if(!internal_msg.ParseFromArray(body, sz)) {
            LOG_ERROR("ES-Consumer: 反序列化 ESIndexEvent/InternalMessage 均失败");
            return ConsumeAction::NackDiscard;
        }
        const auto &msg_info = internal_msg.message_info();
        if(msg_info.message().message_type() != chatnow::MessageType::STRING) {
            return ConsumeAction::Ack;
        }
        std::string user_id = msg_info.sender().user_id();
        unsigned long message_id = msg_info.message_id();
        long create_time = msg_info.timestamp();
        std::string session_id = msg_info.chat_session_id();
        std::string content = msg_info.message().string_message().content();
        bool ret = _es_client->appendData(user_id, message_id, create_time, session_id, content);
        if(!ret) {
            if(redelivered) {
                LOG_ERROR("ES-Consumer: InternalMessage 二次失败转 DLX mid={}", message_id);
                return ConsumeAction::NackDiscard;
            }
            LOG_ERROR("ES-Consumer: InternalMessage 写入 ES 失败（首次），重投 mid={}", message_id);
            return ConsumeAction::NackRequeue;
        }
        LOG_DEBUG("ES-Consumer: InternalMessage(兼容) 索引成功 mid={}", message_id);
        return ConsumeAction::Ack;
    }
```

- [ ] **Step 2: Commit**

```bash
git add message/source/message_server.h
git commit -m "feat: onESMessage 优先解析 ESIndexEvent，回退兼容 InternalMessage 过渡期"
```

---

### Task 6: MessageServiceImpl — ES outbox reaper

**Files:**
- Modify: `message/source/message_server.h`（在 `stop_outbox_reaper` 之后，`onESMessage` 之前）

- [ ] **Step 1: 在 stop_outbox_reaper 之后追加 ES reaper 方法**

```cpp
    void start_es_outbox_reaper(const std::string &owner) {
        if(!_es_outbox || !_es_publisher) {
            LOG_WARN("ES outbox reaper 未启动：es_outbox / es_publisher 未注入");
            return;
        }
        constexpr int kReapIntervalSec = 5;
        constexpr int kLeaseTtlSec     = 30;
        constexpr int kBatchLimit      = 50;
        _es_reaper_running.store(true);
        _es_reaper_owner = owner;
        _es_reaper_thread = std::thread([this]() {
            while(_es_reaper_running.load()) {
                try {
                    if(!_es_outbox->try_acquire_reaper_lease(_es_reaper_owner, kLeaseTtlSec)) {
                        std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                        continue;
                    }
                    auto batch = _es_outbox->peek(kBatchLimit);
                    if(batch.empty()) {
                        std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                        continue;
                    }
                    LOG_INFO("ES outbox reaper: 取出 {} 条待重投", batch.size());
                    for(const auto &payload : batch) _es_outbox->remove(payload);
                    auto es_outbox = _es_outbox;
                    for(const auto &payload : batch) {
                        std::string p = payload;
                        _es_publisher->publish_confirm(p,
                            [es_outbox, p](PublishStatus st, const std::string &err) {
                                if(st == PublishStatus::Acked) return;
                                LOG_WARN("ES outbox reaper 重投仍失败: {}", err);
                                if(es_outbox) es_outbox->enqueue(
                                    p, static_cast<long long>(time(nullptr)));
                            });
                    }
                } catch(std::exception &e) {
                    LOG_ERROR("ES outbox reaper 异常: {}", e.what());
                }
                std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
            }
            if(_es_outbox) _es_outbox->release_reaper_lease(_es_reaper_owner);
            LOG_INFO("ES outbox reaper 已停止");
        });
    }

    void stop_es_outbox_reaper() {
        _es_reaper_running.store(false);
        if(_es_reaper_thread.joinable()) _es_reaper_thread.join();
    }
```

- [ ] **Step 2: 在 private 成员变量区追加 ES reaper 状态**

在现有的 PushOutbox reaper 状态（`_reaper_owner` 之后，line 909）追加：

```cpp
    // ES outbox reaper 状态
    std::atomic<bool> _es_reaper_running {false};
    std::thread _es_reaper_thread;
    std::string _es_reaper_owner;
```

- [ ] **Step 3: Commit**

```bash
git add message/source/message_server.h
git commit -m "feat: MessageServiceImpl 新增 ES outbox reaper——独立线程定期重投 ES 索引事件"
```

---

## Part 3: Builder + Config + Main

### Task 7: MessageServerBuilder — 新增 make_es_publisher + 构造注入

**Files:**
- Modify: `message/source/message_server.h:963-968`（make_redis_object）
- Modify: `message/source/message_server.h:970-986`（make_push_publisher 之后）
- Modify: `message/source/message_server.h:1090-1097`（make_rpc_object 中构造 MessageServiceImpl + 启动 reaper）
- Modify: `message/source/message_server.h:1187-1195`（private 成员）

- [ ] **Step 1: make_redis_object 中新增 ESOutbox 创建**

在 `make_redis_object` 中 `_push_outbox` 创建之后追加：

```cpp
        _es_outbox = std::make_shared<ESOutbox>(_redis);
```

- [ ] **Step 2: 在 make_push_publisher 之后新增 make_es_publisher 方法**

```cpp
    /* brief: 构造 ES 索引 Publisher（onDBMessage 在 DB commit 后投递 ESIndexEvent） */
    void make_es_publisher(const std::string &exchange,
                           const std::string &queue,
                           const std::string &binding_key)
    {
        if(!_mq_client) {
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

- [ ] **Step 3: make_rpc_object 更新 MessageServiceImpl 构造 + 启动 ES reaper**

将 MessageServiceImpl 的构造改传新参数：

```cpp
        MessageServiceImpl *message_service = new MessageServiceImpl(
            _file_service_name, _user_service_name, _chatsession_service_name,
            _mm_channels, _es_client, _mysql_client,
            _subscriber_db, _subscriber_es,
            _db_queue_settings, _es_queue_settings,
            _seq_gen, _push_publisher, _push_outbox,
            _es_publisher, _es_outbox);
```

在 `message_service->start_outbox_reaper(owner);` 之后追加：

```cpp
        message_service->start_es_outbox_reaper(owner);
```

- [ ] **Step 4: 在 private 成员变量区追加 ES 相关成员**

在 `PushOutbox::ptr _push_outbox;` 之后追加：

```cpp
    Publisher::ptr _es_publisher;
    ESOutbox::ptr  _es_outbox;
    declare_settings _es_pub_settings;
```

- [ ] **Step 5: Commit**

```bash
git add message/source/message_server.h
git commit -m "feat: MessageServerBuilder 新增 make_es_publisher + ES outbox/reaper 注入"
```

---

### Task 8: MessageServer::start — 关停顺序更新

**Files:**
- Modify: `message/source/message_server.h:938-945`（start 方法）

- [ ] **Step 1: 在关停顺序中增加 stop_es_outbox_reaper**

将 `start()` 方法中 `stop_outbox_reaper()` 之后增加一行：

```cpp
    void start() {
        _rpc_server->RunUntilAskedToQuit();
        if(_service_impl) {
            _service_impl->stop_outbox_reaper();
            _service_impl->stop_es_outbox_reaper();
        }
        _mq_client.reset();
        _rpc_server->Join();
        LOG_INFO("Message 关停完成");
    }
```

- [ ] **Step 2: Commit**

```bash
git add message/source/message_server.h
git commit -m "feat: MessageServer 关停顺序新增 ES reaper 停止"
```

---

### Task 9: message_server.cc — 新增 gflags + main 调用

**Files:**
- Modify: `message/source/message_server.cc`

- [ ] **Step 1: 新增 ES publisher 的 gflags 声明**

在 `mq_push_binding_key` 的 DEFINE 之后（line 39 后）追加：

```cpp
DEFINE_string(mq_es_exchange, "es_index_exchange", "ES 索引事件的交换机名称（DIRECT）");
DEFINE_string(mq_es_queue, "msg_queue_es", "ES 索引事件队列名称");
DEFINE_string(mq_es_binding_key, "msg_queue_es", "ES 索引事件绑定键");
```

- [ ] **Step 2: 在 main 中调用 make_es_publisher**

在 `msb.make_push_publisher(...)` 调用之后（line 59 后）追加：

```cpp
    msb.make_es_publisher(FLAGS_mq_es_exchange, FLAGS_mq_es_queue, FLAGS_mq_es_binding_key);
```

- [ ] **Step 3: Commit**

```bash
git add message/source/message_server.cc
git commit -m "feat: main 新增 ES publisher gflags 和 builder 调用"
```

---

### Task 10: conf/message_server.conf — MQ 配置更新

**Files:**
- Modify: `conf/message_server.conf`

- [ ] **Step 1: 新增 ES 交换机/队列配置项**

在 `mq_push_binding_key=push` 之后追加：

```ini
-mq_es_exchange=es_index_exchange
-mq_es_queue=msg_queue_es
-mq_es_binding_key=msg_queue_es
```

- [ ] **Step 2: Commit**

```bash
git add conf/message_server.conf
git commit -m "feat: conf 新增 ES 独立交换机/队列配置"
```

---

## 验证计划

完成所有 Task 后：

1. **正常文本消息**：发送文本消息 → 验证 DB 落库后 ES 也有记录
2. **非文本消息跳过**：发送图片 → 验证 onDBMessage 不产生 ESIndexEvent（分支跳过）
3. **ES publisher 不可达**：停 RabbitMQ → 发消息 → 验证 `ZCARD im:es:outbox > 0`
4. **ES outbox reaper**：恢复 RabbitMQ → 验证 5-10s 后 `ZCARD im:es:outbox == 0`
5. **过渡期兼容**：旧 FANOUT 残留 InternalMessage → onESMessage 回退解析成功
6. **DB 事务失败**：模拟唯一索引冲突 → 验证不产生 ESIndexEvent（控制流不到达）
7. **多实例 reaper**：启动 2 个 Message 实例 → 验证只有一个 reaper 持锁工作（Redis `GET im:es:outbox:lock`）
8. **关停安全**：kill Message → 验证 reaper 线程 join 正常，无 crash

---

## 回滚说明

所有改动集中在 5 个文件。回滚到 `main @ 55decee` 即可恢复旧版 FANOUT 双写路径。

过渡期 `msg_queue_es` 同时绑定到 FANOUT（旧）和 DIRECT（新），ES consumer 能解析两种 proto。确认新版稳定后，删除 FANOUT 上的 `msg_queue_es` 绑定，下掉 onESMessage 中的 InternalMessage 兼容代码。

---

## 上线计划

**阶段 1：部署新版 Message 服务**
- `msg_queue_es` 同时绑定到 FANOUT 和 DIRECT
- ES consumer 优先解析 ESIndexEvent，回退解析 InternalMessage
- Transmite 不需要改动

**阶段 2：确认稳定（观察 1-2 天）**
- 确认 ES outbox 无异常堆积
- 确认 ES consumer 无 DLX 增长

**阶段 3：清理旧路径**
- 删除 FANOUT 上 `msg_queue_es` 的绑定
- ES consumer 中移除 InternalMessage 兼容解析代码
