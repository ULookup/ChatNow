# Design: 前端三个 Issue 修复

**日期**: 2026-05-24
**分支**: fix/infra-stability
**关联 Issues**: #18, #19, #20

---

## Issue #18 — update_read_ack 404

### 根因

Gateway 的 `register_routes()` 漏掉了 `UpdateReadAck` 和 `SelectByClientMsgId` 两个路由。RPC 定义和 handler 实现均已完备，仅路由缺失。

### 修复

在 `gateway/source/gateway_server.h` 的 Message 路由段末尾（约 391 行，`ClearConversation` 之后），新增两行：

```cpp
route<msg::MessageService_Stub, msg::UpdateReadAckReq, msg::UpdateReadAckRsp>(
    "/service/message/update_read_ack", _message_svc, GatewayAuth::JWT_REQUIRED,
    &msg::MessageService_Stub::UpdateReadAck);
route<msg::MessageService_Stub, msg::SelectByClientMsgIdReq, msg::SelectByClientMsgIdRsp>(
    "/service/message/select_by_client_msg_id", _message_svc, GatewayAuth::JWT_REQUIRED,
    &msg::MessageService_Stub::SelectByClientMsgId);
```

无其他文件改动。

---

## Issue #19 — 消息推送防御

### 根因

`onDBMessage` 中 `if (_push_publisher)` 在 publisher 未初始化时静默跳过推送，无日志、不写出 outbox。落库的三层保障（日志 → 重试 → outbox 兜底）在 push 链路缺失第一层。

### 修复（对标落库模式，三层防御）

**改动文件**: `message/source/message_server.h`

**1. onDBMessage — publisher 为空时打 error 并写 outbox（约 line 570）**

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

**2. start_push_outbox_reaper — publisher 空指针保护（约 line 1077）**

```cpp
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
```

**3. 同理修改其他 push 发布点** — `publish_recalled_notify_`（line 769）、`publish_pin_notify_`（line 820）、`publish_reaction_notify_`（line 792），各自 `if (!_push_publisher) return;` 后加 else 分支打 LOG_ERROR。

---

## Issue #20 — Presence 推送

### 根因

四个独立 bug，均在 `push/source/push_server.h`：

| # | 问题 | 位置 |
|---|------|------|
| 1 | 连接后不推 PRESENCE_CHANGE_NOTIFY | `_handle_client_auth_` line 443 |
| 2 | 断开后不写 OFFLINE、不推送 | close handler lines 936-945 |
| 3 | 心跳不刷新 presence key TTL，导致 120s 后过期 | message handler line 974-976 |
| 4 | 断开后 presence key 残留 120s（问题 2 的副作用） | close handler |

### 修复

**改动文件**: `push/source/push_server.h`

**1. 连接时推送 — 在 `_handle_client_auth_` line 443 之后**

```cpp
_write_presence_online_(uid, did);
_notify_presence_change_(uid, "ONLINE");  // 新增
```

**2. 断开时写 OFFLINE 并推送 — 修改 close handler**

```cpp
_ws_server->set_close_handler([this](websocketpp::connection_hdl hdl) {
    auto conn = _ws_server->get_con_from_hdl(hdl);
    std::string uid, did, jti;
    if (_connections && _connections->client(conn, uid, did, jti)) {
        _connections->remove(conn);
        if (_online_route) _online_route->unbind(uid, did, _instance_id);
        if (_local_route_cache) _local_route_cache->invalidate("route:" + uid);
        _write_presence_offline_(uid, did);             // 新增
        _notify_presence_change_(uid, "OFFLINE");       // 新增
        LOG_DEBUG("WS 关闭 uid={} did={}", uid, did);
    }
});
```

**3. 心跳刷新 presence TTL — 在 message handler line 975 之后**

```cpp
if (notify.notify_type() == NotifyType::CLIENT_HEARTBEAT) {
    _online_route->touch(uid_known);
    _refresh_presence_ttl_(uid_known, did_known);  // 新增
}
```

**新增三个私有方法（PushServiceImpl 类内）:**

**`_write_presence_offline_`** — 对标 `_write_presence_online_`（line 486-498）:

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

**`_refresh_presence_ttl_`** — 心跳时续期 presence key:

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

**`_notify_presence_change_`** — 读取订阅者并推送 PRESENCE_CHANGE_NOTIFY。

采用与 `onPushMessage` 相同的路由分发策略：本地用户直接 `_local_send`，远程用户按实例分组后通过 `PushBatch` RPC 推送。

```cpp
void _notify_presence_change_(const std::string &uid, const std::string &state) {
    if (!_redis) return;
    try {
        // 1. 读取订阅者列表
        std::vector<std::string> subs;
        _redis->smembers("im:presence:sub:" + uid, std::inserter(subs, subs.end()));
        if (subs.empty()) return;

        // 2. 构造 PRESENCE_CHANGE_NOTIFY
        ::chatnow::push::NotifyMessage notify;
        notify.set_notify_type(::chatnow::push::NotifyType::PRESENCE_CHANGE_NOTIFY);
        auto* pc = notify.mutable_presence_change();
        pc->set_user_id(uid);
        pc->set_state(state);
        std::string payload = notify.SerializeAsString();

        // 3. 按实例分组：本地 vs 远程
        std::unordered_map<std::string, std::vector<std::string>> peer_to_uids;
        for (const auto& sub_uid : subs) {
            auto route = resolve_route(sub_uid);
            bool any_local = false;
            for (const auto& did : route.device_ids) {
                auto it = route.device_to_instance.find(did);
                std::string inst = (it != route.device_to_instance.end()) ? it->second : "";
                if (inst.empty() || inst == _instance_id) {
                    _local_send(sub_uid, did, payload);
                    any_local = true;
                } else {
                    peer_to_uids[inst].push_back(sub_uid);
                }
            }
        }

        // 4. 跨实例推送：每个对端一次 PushBatch
        long long now_ts = static_cast<long long>(time(nullptr));
        for (auto& kv : peer_to_uids) {
            const std::string& peer = kv.first;
            auto& uids = kv.second;
            // 去重
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

---

## 影响范围

| Issue | 文件 | 改动量 |
|-------|------|--------|
| #18 | `gateway/source/gateway_server.h` | +6 行 |
| #19 | `message/source/message_server.h` | ~30 行（else 分支 + null guard） |
| #20 | `push/source/push_server.h` | ~100 行（3 个新方法 + 3 处调用点，含跨实例推送逻辑） |

所有改动限于已有文件，不新增文件，不改变 proto，不改变外部接口。
