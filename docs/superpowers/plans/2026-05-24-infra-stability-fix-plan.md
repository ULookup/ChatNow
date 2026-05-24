# 前端三个 Issue 修复 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复三个前端 Issue：#18（update_read_ack 404）、#19（消息推送静默失败）、#20（Presence 状态不推送）

**Architecture:** 三个修复独立、无交叉依赖。每个 Issue 只改一个文件。不改 proto、不新增文件。

**Tech Stack:** C++17, brpc, webscoketpp, protobuf, Redis, RabbitMQ

---

### Task 1: Issue #18 — 补全 Gateway 路由注册

**Files:**
- Modify: `gateway/source/gateway_server.h` (在 line 391 `ClearConversation` 路由之后)

- [ ] **Step 1: 在 register_routes() 的 Message 路由段末尾添加两个路由**

在 `gateway/source/gateway_server.h` 约 line 391（`ClearConversation` 路由结束 `);` 之后，空一行插入：

```cpp
    route<msg::MessageService_Stub, msg::UpdateReadAckReq, msg::UpdateReadAckRsp>(
        "/service/message/update_read_ack", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::UpdateReadAck);
    route<msg::MessageService_Stub, msg::SelectByClientMsgIdReq, msg::SelectByClientMsgIdRsp>(
        "/service/message/select_by_client_msg_id", _message_svc, GatewayAuth::JWT_REQUIRED,
        &msg::MessageService_Stub::SelectByClientMsgId);
```

注意：`SelectByClientMsgId` 的类型名需要从 proto 确认。查看 `proto/message/message_service.proto` 中 `SelectByClientMsgId` 的请求/响应消息名。

- [ ] **Step 2: 验证 proto 类型名**

```bash
grep -A2 'rpc SelectByClientMsgId' /home/icepop/ChatNow/proto/message/message_service.proto
grep -A2 'rpc UpdateReadAck' /home/icepop/ChatNow/proto/message/message_service.proto
```

确认请求/响应消息类型后，调整 Step 1 的模板参数。

- [ ] **Step 3: Commit**

```bash
git add gateway/source/gateway_server.h
git commit -m "fix(gateway): add missing routes for UpdateReadAck and SelectByClientMsgId"
```

---

### Task 2: Issue #19 — onDBMessage push publisher 为空时防御

**Files:**
- Modify: `message/source/message_server.h` (lines 569-585)

- [ ] **Step 1: 将 `if (_push_publisher)` 改为 if-else，else 打 LOG_ERROR 并写 outbox**

定位 `onDBMessage` 方法中约 line 569-585 的 push 发布段。当前代码：

```cpp
        // 发布到 Push 队列，由 Push 服务进行 WS 下发
        if (_push_publisher) {
            std::string push_payload = internal_msg.SerializeAsString();
            auto outbox = _push_outbox;
            std::map<std::string, std::string> push_headers;
            ::chatnow::mq::mq_inject_trace_headers(push_headers);
            try {
                _push_publisher->publish_confirm(push_payload, push_headers,
                    [push_payload, outbox](PublishStatus st, const std::string &err) {
                        if (st != PublishStatus::Acked && outbox)
                            outbox->enqueue(push_payload, static_cast<long long>(time(nullptr)));
                    });
            } catch (std::exception &e) {
                LOG_ERROR("DB-Consumer: 发布 Push 事件异常 mid={}: {}", mid, e.what());
                if (outbox) outbox->enqueue(push_payload, static_cast<long long>(time(nullptr)));
            }
        }
```

修改为：

```cpp
        // 发布到 Push 队列，由 Push 服务进行 WS 下发
        if (_push_publisher) {
            std::string push_payload = internal_msg.SerializeAsString();
            auto outbox = _push_outbox;
            std::map<std::string, std::string> push_headers;
            ::chatnow::mq::mq_inject_trace_headers(push_headers);
            try {
                _push_publisher->publish_confirm(push_payload, push_headers,
                    [push_payload, outbox](PublishStatus st, const std::string &err) {
                        if (st != PublishStatus::Acked && outbox)
                            outbox->enqueue(push_payload, static_cast<long long>(time(nullptr)));
                    });
            } catch (std::exception &e) {
                LOG_ERROR("DB-Consumer: 发布 Push 事件异常 mid={}: {}", mid, e.what());
                if (outbox) outbox->enqueue(push_payload, static_cast<long long>(time(nullptr)));
            }
        } else {
            LOG_ERROR("DB-Consumer: Push publisher 未初始化，消息无法实时推送 mid={}", mid);
            auto outbox = _push_outbox;
            if (outbox) {
                std::string push_payload = internal_msg.SerializeAsString();
                outbox->enqueue(push_payload, static_cast<long long>(time(nullptr)));
            }
        }
```

- [ ] **Step 2: Commit**

```bash
git add message/source/message_server.h
git commit -m "fix(message): add defensive logging and outbox fallback when push publisher is null in onDBMessage"
```

---

### Task 3: Issue #19 — push_outbox_reaper 空指针保护

**Files:**
- Modify: `message/source/message_server.h` (lines 1074-1081)

- [ ] **Step 1: 在 reaper 的 for 循环前加 null guard**

定位 `start_push_outbox_reaper` 方法约 line 1074-1081：

```cpp
                try {
                    auto items = _push_outbox->peek(50);
                    for (const auto &item : items) {
                        _push_publisher->publish_confirm(item, {},
                            [outbox = _push_outbox, item](PublishStatus st, const std::string &) {
                                if (st == PublishStatus::Acked && outbox) outbox->remove(item);
                            });
                    }
                } catch (std::exception &e) {
```

修改为：

```cpp
                try {
                    auto items = _push_outbox->peek(50);
                    if (items.empty()) continue;
                    if (!_push_publisher) {
                        LOG_ERROR("PushOutbox reaper: publisher 未初始化，跳过重试 (pending {} 条)", items.size());
                        continue;
                    }
                    for (const auto &item : items) {
                        _push_publisher->publish_confirm(item, {},
                            [outbox = _push_outbox, item](PublishStatus st, const std::string &) {
                                if (st == PublishStatus::Acked && outbox) outbox->remove(item);
                            });
                    }
                } catch (std::exception &e) {
```

- [ ] **Step 2: Commit**

```bash
git add message/source/message_server.h
git commit -m "fix(message): add null guard for push publisher in outbox reaper"
```

---

### Task 4: Issue #19 — 其他 push 发布点加 error 日志

**Files:**
- Modify: `message/source/message_server.h` (lines 769, 792, 820)

- [ ] **Step 1: publish_recalled_notify_ 加 else 日志（line 769）**

```cpp
// 改前:
    void publish_recalled_notify_(const std::string &cid, int64_t mid) {
        if (!_push_publisher) return;
        // ...

// 改后:
    void publish_recalled_notify_(const std::string &cid, int64_t mid) {
        if (!_push_publisher) {
            LOG_ERROR("publish_recalled_notify: push publisher 未初始化 cid={} mid={}", cid, mid);
            return;
        }
        // ...
```

- [ ] **Step 2: publish_reaction_notify_ 加 else 日志（line 792）**

```cpp
// 改前:
        if (!_push_publisher || target_uid == actor_uid) return;

// 改后:
        if (target_uid == actor_uid) return;
        if (!_push_publisher) {
            LOG_ERROR("publish_reaction_notify: push publisher 未初始化 target={} mid={}", target_uid, mid);
            return;
        }
```

- [ ] **Step 3: publish_pin_notify_ 加 else 日志（line 820）**

```cpp
// 改前:
    void publish_pin_notify_(...) {
        if (!_push_publisher) return;

// 改后:
    void publish_pin_notify_(...) {
        if (!_push_publisher) {
            LOG_ERROR("publish_pin_notify: push publisher 未初始化 cid={} mid={}", cid, mid);
            return;
        }
```

- [ ] **Step 4: Commit**

```bash
git add message/source/message_server.h
git commit -m "fix(message): add error logging when push publisher is null in notify helpers"
```

---

### Task 5: Issue #20 — 新增 `_write_presence_offline_` 方法

**Files:**
- Modify: `push/source/push_server.h` (在 `_write_presence_online_` 方法之后，约 line 499)

- [ ] **Step 1: 在 PushServiceImpl 的 private 区域添加方法**

在 `_write_presence_online_` 方法结束（约 line 499 的 `}`）之后插入：

```cpp
    void _write_presence_offline_(const std::string &uid, const std::string &did) {
        try {
            std::string k = std::string("im:presence:device:{") + uid + "}:" + did;
            auto pipe = _redis->pipeline();
            pipe.hset(k, "state", "OFFLINE");
            pipe.hset(k, "last_active_at_ms", std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
            pipe.expire(k, std::chrono::seconds(kPresenceTtlSec));
            pipe.exec();
        } catch (std::exception &e) {
            LOG_WARN("Presence offline write failed uid={} did={}: {}", uid, did, e.what());
        }
    }
```

- [ ] **Step 2: Commit**

```bash
git add push/source/push_server.h
git commit -m "feat(push): add _write_presence_offline_ for disconnect presence state"
```

---

### Task 6: Issue #20 — 新增 `_refresh_presence_ttl_` 方法

**Files:**
- Modify: `push/source/push_server.h` (在 `_write_presence_offline_` 之后)

- [ ] **Step 1: 添加 TTL 刷新方法**

在 `_write_presence_offline_` 方法之后插入：

```cpp
    void _refresh_presence_ttl_(const std::string &uid, const std::string &did) {
        try {
            std::string k = std::string("im:presence:device:{") + uid + "}:" + did;
            _redis->expire(k, std::chrono::seconds(kPresenceTtlSec));
        } catch (std::exception &e) {
            LOG_WARN("Presence TTL refresh failed uid={} did={}: {}", uid, did, e.what());
        }
    }
```

- [ ] **Step 2: Commit**

```bash
git add push/source/push_server.h
git commit -m "feat(push): add _refresh_presence_ttl_ to refresh presence key on heartbeat"
```

---

### Task 7: Issue #20 — 新增 `_notify_presence_change_` 方法（含跨实例推送）

**Files:**
- Modify: `push/source/push_server.h` (在 `_refresh_presence_ttl_` 之后)

- [ ] **Step 1: 添加实时推送方法**

`_notify_presence_change_` 读取 `im:presence:sub:{uid}` 的订阅者集合，按本地/远程分组，本地 `_local_send`，远程 `PushBatch` RPC。注意 `PeerToUids` 的 `std::vector` 需在构造 NotifyMessage 前去重。

该方法位于 PushServiceImpl private 区域。

需要 `#include <algorithm>` 用于 `std::sort` / `std::unique`。

```cpp
    void _notify_presence_change_(const std::string &uid, const std::string &state) {
        if (!_redis) return;
        try {
            std::vector<std::string> subs;
            _redis->smembers("im:presence:sub:" + uid, std::inserter(subs, subs.end()));
            if (subs.empty()) return;

            ::chatnow::push::NotifyMessage notify;
            notify.set_notify_type(::chatnow::push::NotifyType::PRESENCE_CHANGE_NOTIFY);
            auto* pc = notify.mutable_presence_change();
            pc->set_user_id(uid);
            pc->set_state(state);
            std::string payload = notify.SerializeAsString();

            std::unordered_map<std::string, std::vector<std::string>> peer_to_uids;
            for (const auto& sub_uid : subs) {
                auto route = resolve_route(sub_uid);
                if (route.device_ids.empty()) continue;
                for (const auto& did : route.device_ids) {
                    auto it = route.device_to_instance.find(did);
                    std::string inst = (it != route.device_to_instance.end()) ? it->second : "";
                    if (inst.empty() || inst == _instance_id) {
                        _local_send(sub_uid, did, payload);
                    } else {
                        peer_to_uids[inst].push_back(sub_uid);
                    }
                }
            }

            if (peer_to_uids.empty()) return;

            long long now_ts = static_cast<long long>(time(nullptr));
            for (auto& kv : peer_to_uids) {
                const std::string& peer = kv.first;
                auto& uids = kv.second;
                std::sort(uids.begin(), uids.end());
                uids.erase(std::unique(uids.begin(), uids.end()), uids.end());

                auto channel = _mm_channels->choose(peer);
                if (!channel) {
                    LOG_WARN("Presence notify: 对端 {} 不可达，跳过 {} 个订阅者", peer, uids.size());
                    continue;
                }
                PushService_Stub stub(channel.get());
                auto* closure = new SelfDeleteRpcClosure<PushBatchReq, PushBatchRsp>();
                closure->req.set_request_id("presence-notify-" + uid);
                for (const auto& u : uids) closure->req.add_user_id_list(u);
                closure->req.mutable_notify()->CopyFrom(notify);
                closure->on_done = [peer](brpc::Controller* c, const PushBatchRsp&) {
                    if (c->Failed())
                        LOG_WARN("Presence PushBatch 跨实例失败 peer={}: {}", peer, c->ErrorText());
                };
                stub.PushBatch(&closure->cntl, &closure->req, &closure->rsp, closure);
            }
        } catch (std::exception &e) {
            LOG_WARN("Presence notify failed uid={}: {}", uid, e.what());
        }
    }
```

- [ ] **Step 2: Commit**

```bash
git add push/source/push_server.h
git commit -m "feat(push): add _notify_presence_change_ with cross-instance PushBatch support"
```

---

### Task 8: Issue #20 — 接入点：close handler / auth / heartbeat

**Files:**
- Modify: `push/source/push_server.h` (三处调用点)

- [ ] **Step 1: 连接时推送 — `_handle_client_auth_` 约 line 443**

在 `_write_presence_online_(uid, did);` 之后新增一行：

```cpp
        _write_presence_online_(uid, did);
        _notify_presence_change_(uid, "ONLINE");
```

- [ ] **Step 2: 断开时写 OFFLINE 并推送 — close handler 约 lines 936-945**

`PushServerBuilder::make_ws_object` 中的 `set_close_handler` lambda。`_write_presence_offline_` 和 `_notify_presence_change_` 是 `PushServiceImpl` 的私有方法，需要通过 `_push_service` 指针调用，因此需将方法设为 public 或 friend。

**方案**: 将 `_write_presence_offline_`、`_refresh_presence_ttl_`、`_notify_presence_change_` 声明为 public（或将 PushServerBuilder 声明为 friend）。

在 close handler 中（`_connections->remove(conn)` 及 route 清理之后）新增：

```cpp
            _connections->remove(conn);
            if (_online_route) _online_route->unbind(uid, did, _instance_id);
            if (_local_route_cache) _local_route_cache->invalidate("route:" + uid);
            if (_push_service) {
                _push_service->write_presence_offline(uid, did);
                _push_service->notify_presence_change(uid, "OFFLINE");
            }
            LOG_DEBUG("WS 关闭 uid={} did={}", uid, did);
```

注意：此处调用的方法名为 `write_presence_offline` / `notify_presence_change` / `refresh_presence_ttl`（public 接口，无下划线前缀），内部实现委托给同名的 `_` 前缀私有方法。

- [ ] **Step 3: 心跳刷新 TTL — message handler 约 line 974-976**

在 heartbeat 分支 `_online_route->touch(uid_known);` 之后新增：

```cpp
        if (notify.notify_type() == NotifyType::CLIENT_HEARTBEAT) {
            _online_route->touch(uid_known);
            if (_push_service) _push_service->refresh_presence_ttl(uid_known, did_known);
        }
```

- [ ] **Step 4: 将三个方法设为 public**

将 Task 5、6、7 中新增的 `_write_presence_offline_`、`_refresh_presence_ttl_`、`_notify_presence_change_` 重命名为无前缀的 public 方法名（或将 PushServerBuilder 声明为 friend 类）。推荐方案：添加 public 包装方法，保持私有实现不变。

在 PushServiceImpl 的 public 区域（约 line 80，`set_resend_params` 之后）添加：

```cpp
    void write_presence_offline(const std::string &uid, const std::string &did) {
        _write_presence_offline_(uid, did);
    }
    void refresh_presence_ttl(const std::string &uid, const std::string &did) {
        _refresh_presence_ttl_(uid, did);
    }
    void notify_presence_change(const std::string &uid, const std::string &state) {
        _notify_presence_change_(uid, state);
    }
```

- [ ] **Step 5: Commit**

```bash
git add push/source/push_server.h
git commit -m "feat(push): wire presence notifications on connect, disconnect, and heartbeat"
```

---

### Task 9: 编译验证

- [ ] **Step 1: 编译 Gateway**

```bash
cd /home/icepop/ChatNow && cmake --build build --target gateway_server -- -j$(nproc) 2>&1 | tail -30
```

预期：编译成功，无错误。

- [ ] **Step 2: 编译 Message 服务**

```bash
cd /home/icepop/ChatNow && cmake --build build --target message_server -- -j$(nproc) 2>&1 | tail -30
```

预期：编译成功，无错误。

- [ ] **Step 3: 编译 Push 服务**

```bash
cd /home/icepop/ChatNow && cmake --build build --target push_server -- -j$(nproc) 2>&1 | tail -30
```

预期：编译成功，无错误。

- [ ] **Step 4: 如有编译错误，用 cpp-build-resolver agent 修复**

---

### Task 10: 最终 Commit

- [ ] **Step 1: 确认所有改动已提交**

```bash
git status
git log --oneline -10
```

- [ ] **Step 2: 如有多余改动，整理提交**
