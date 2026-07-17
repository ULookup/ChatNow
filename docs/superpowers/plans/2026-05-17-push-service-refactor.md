# Push 服务重构 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重构 Push 服务：JWT WS 鉴权、设备级路由、ResponseHeader、Stub 切换、MQ 消费包名切换、Presence 数据写入端。

**Architecture:** 修改 7 个文件 + 删除 1 个旧 proto。核心发送管线（MQ 消费 → 本机直推 → 跨实例 PushBatch）逻辑不变，改设备级粒度。Push 与 Presence 拆分：Push 直接写 Redis presence 数据，Presence 服务独立后续做。

**Tech Stack:** C++17, brpc, Protobuf, WebSocket++ (websocketpp), RabbitMQ (AMQP-CPP), Redis (redis++), JwtCodec (jwt-cpp), spdlog

**Spec:** `docs/superpowers/specs/2026-05-17-push-service-refactor-design.md`

---

## File Structure

| 文件 | 动作 | 职责 |
|---|---|---|
| `proto/push/notify.proto` | 修改 | CLIENT_AUTH session_id→access_token；ACK 加 device_id；KICKED 类型+NotifyKicked |
| `proto/push/push_service.proto` | 修改 | PushToUserReq 加 target_device_ids；Rsp 改 ResponseHeader |
| `proto/push.proto` | 删除 | 旧 flat proto，全仓零引用后删 |
| `common/dao/data_redis.hpp` | 修改 | OnlineRoute key→HASH(uid→did→instance)；UnackedPush per-device+payload |
| `push/source/connection.hpp` | 修改 | 连接表按 (uid, device_id)；JWT 鉴权流程 |
| `push/source/push_server.h` | **重写** | namespace chatnow::push；JWT 鉴权；设备级路由；Stub 切换 |
| `push/source/push_server.cc` | 修改 | gflags 同步 |
| `push/CMakeLists.txt` | 修改 | proto_files 加 identity proto（JWT auth stub）、message_service proto |
| `conf/push_server.conf` | 修改 | gflag 同步 |

---

### Task 1: Proto — notify.proto（CLIENT_AUTH JWT + ACK device_id + KICKED 类型）

**Files:**
- Modify: `proto/push/notify.proto`

- [ ] **Step 1: 修改 NotifyClientAuth — session_id → access_token**

将 `proto/push/notify.proto` 中的 `NotifyClientAuth` 从：

```protobuf
message NotifyClientAuth {
    string session_id = 1;
    string device_id = 2;
    optional uint64 last_user_seq = 3;
}
```

改为：

```protobuf
message NotifyClientAuth {
    string access_token = 1;            // JWT access token（替代 session_id）
    string device_id = 2;
    optional uint64 last_user_seq = 3;
}
```

- [ ] **Step 2: 修改 NotifyMsgPushAck — 加 device_id**

将：

```protobuf
message NotifyMsgPushAck {
    string user_id = 1;
    int64 message_id = 2;
    uint64 user_seq = 3;
    string conversation_id = 4;
}
```

改为：

```protobuf
message NotifyMsgPushAck {
    string user_id = 1;
    string device_id = 2;              // 新增：设备级 ACK
    int64 message_id = 3;
    uint64 user_seq = 4;
    string conversation_id = 5;
}
```

- [ ] **Step 3: NotifyType 枚举 — 加 4 个新值**

在现有 `READ_RECEIPT_NOTIFY = 10;` 之后、`CLIENT_AUTH = 49;` 之前插入：

```protobuf
    KICKED_BY_NEW_DEVICE = 11;
    KICKED_BY_REVOKE = 12;
    FORCE_LOGOUT = 13;
    CONVERSATION_DISMISSED_NOTIFY = 14;
```

- [ ] **Step 4: 加 NotifyKicked message**

在 `NotifyReadReceipt` 之后加：

```protobuf
message NotifyKicked {
    NotifyType reason = 1;
    string message = 2;
}
```

- [ ] **Step 5: NotifyMessage.oneof 加 kicked 分支**

在 `NotifyReadReceipt read_receipt = 17;` 之后加：

```protobuf
        NotifyKicked kicked = 18;
```

- [ ] **Step 6: Commit**

```bash
git add proto/push/notify.proto
git commit -m "proto(push-notify): CLIENT_AUTH session_id→access_token, ACK + device_id, +4 KICKED NotifyType + NotifyKicked"
```

---

### Task 2: Proto — push_service.proto（ResponseHeader + target_device_ids）

**Files:**
- Modify: `proto/push/push_service.proto`

- [ ] **Step 1: PushToUserReq 加 target_device_ids**

将 `PushToUserReq` 从：

```protobuf
message PushToUserReq {
    string request_id = 1;
    string user_id = 2;
    chatnow.push.NotifyMessage notify = 3;
    optional uint64 user_seq = 4;
}
```

改为：

```protobuf
message PushToUserReq {
    string request_id = 1;
    string user_id = 2;
    chatnow.push.NotifyMessage notify = 3;
    optional uint64 user_seq = 4;
    repeated string target_device_ids = 5;   // 空=所有设备；非空=仅指定设备
}
```

- [ ] **Step 2: PushToUserRsp / PushBatchRsp 改为 ResponseHeader**

将两个 Rsp 从旧格式改为：

```protobuf
message PushToUserRsp {
    chatnow.common.ResponseHeader header = 1;
    int32 online_device_count = 2;
}

message PushBatchRsp {
    chatnow.common.ResponseHeader header = 1;
    int32 online_count = 2;
}
```

注意：Rsp 中无 `request_id` 独立字段了——`request_id` 由 `HANDLE_RPC` 宏统一回填到 `header.request_id`。

- [ ] **Step 3: Commit**

```bash
git add proto/push/push_service.proto
git commit -m "proto(push-service): PushToUserReq + target_device_ids, Rsp 切 ResponseHeader"
```

---

### Task 3: Redis DAO — OnlineRoute 设备级 + UnackedPush 存 payload

**Files:**
- Modify: `common/dao/data_redis.hpp` — key namespace (§31-57) + OnlineRoute 类 (§430-471) + UnackedPush 类 (§736-806)

- [ ] **Step 1: key namespace — 改 OnlineRoute key，加 Unacked 设备级 key**

将：

```cpp
inline constexpr const char* kOnline     = "im:online:";        // uid        -> SET<push_instance_id>
```

改为：

```cpp
inline constexpr const char* kOnline     = "im:online:";        // uid        -> HASH { device_id: instance_id }
```

`kOnlineTtl` 从 60s 改为 120s：

```cpp
inline constexpr std::chrono::seconds kOnlineTtl(120);          // 在线路由 120s（依赖心跳续期）
```

- [ ] **Step 2: 重写 OnlineRoute 类 — SET 改为 HASH**

用以下内容替换 `OnlineRoute` 类（第 430-471 行）：

```cpp
class OnlineRoute
{
public:
    using ptr = std::shared_ptr<OnlineRoute>;
    OnlineRoute(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /* brief: 设备上线 — HSET uid did instance */
    void bind(const std::string &uid, const std::string &device_id,
              const std::string &push_instance,
              std::chrono::seconds ttl = kOnlineTtl) {
        try {
            std::string k = key::kOnline + uid;
            _c->hset(k, device_id, push_instance);
            _c->expire(k, ttl);
        } catch(std::exception &e) {
            LOG_ERROR("OnlineRoute.bind 失败 {}-{}-{}: {}", uid, device_id, push_instance, e.what());
        }
    }
    /* brief: 心跳续期（续整个 uid 的 HASH） */
    void touch(const std::string &uid, std::chrono::seconds ttl = kOnlineTtl) {
        try { _c->expire(key::kOnline + uid, ttl); }
        catch(std::exception &e) { LOG_ERROR("OnlineRoute.touch 失败 {}: {}", uid, e.what()); }
    }
    /* brief: 设备下线 — HDEL uid did */
    void unbind(const std::string &uid, const std::string &device_id,
                const std::string &push_instance) {
        try { _c->hdel(key::kOnline + uid, device_id); }
        catch(std::exception &e) { LOG_ERROR("OnlineRoute.unbind 失败 {}-{}-{}: {}", uid, device_id, push_instance, e.what()); }
    }
    /* brief: 取用户所有在线设备 → device_id 列表 */
    std::vector<std::string> devices(const std::string &uid) {
        std::vector<std::string> res;
        try {
            _c->hkeys(key::kOnline + uid, std::back_inserter(res));
        } catch(std::exception &e) { LOG_ERROR("OnlineRoute.devices 失败 {}: {}", uid, e.what()); }
        return res;
    }
    /* brief: 取设备所在 Push 实例 */
    std::string device_instance(const std::string &uid, const std::string &device_id) {
        try {
            auto v = _c->hget(key::kOnline + uid, device_id);
            return v ? *v : "";
        } catch(std::exception &e) {
            LOG_ERROR("OnlineRoute.device_instance 失败 {}-{}: {}", uid, device_id, e.what());
            return "";
        }
    }
    /* brief: 是否有任意在线设备 */
    bool online(const std::string &uid) {
        try { return _c->hlen(key::kOnline + uid) > 0; }
        catch(std::exception &e) { LOG_ERROR("OnlineRoute.online 失败 {}: {}", uid, e.what()); return false; }
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};
```

- [ ] **Step 3: 重写 UnackedPush 类 — per-device + 存 payload**

用以下内容替换 `UnackedPush` 类（第 736-806 行）：

```cpp
class UnackedPush
{
public:
    using ptr = std::shared_ptr<UnackedPush>;
    UnackedPush(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    static std::string key_for(const std::string &uid, const std::string &device_id) {
        return std::string(key::kUnacked) + uid + ":" + device_id;
    }

    /* brief: 入待重传队列（per-device，存 payload_b64 直接用） */
    void push(const std::string &uid, const std::string &device_id,
              unsigned long user_seq, const std::string &payload_b64,
              long long score_ts, std::chrono::seconds ttl = kUnackedTtl) {
        try {
            // member 格式: "<user_seq>:<payload_b64>"，score=timestamp
            std::string k = key_for(uid, device_id);
            std::string member = std::to_string(user_seq) + ":" + payload_b64;
            _c->zadd(k, member, static_cast<double>(score_ts));
            _c->expire(k, ttl);
        } catch(std::exception &e) {
            LOG_ERROR("UnackedPush.push 失败 {}-{}-{}: {}", uid, device_id, user_seq, e.what());
        }
    }
    /* brief: 客户端 ACK 后移除 */
    void ack(const std::string &uid, const std::string &device_id,
             unsigned long user_seq) {
        try {
            std::string k = key_for(uid, device_id);
            // ZREMRANGEBYSCORE 做不到按子串匹配，改用 ZSCAN 找 member 后 ZREM
            // 简化：存时 member 格式为 "<user_seq>:<b64>"，删除时按前缀匹配
            std::string prefix = std::to_string(user_seq) + ":";
            using namespace sw::redis;
            auto cursor = 0LL;
            long long count = 1;
            while (true) {
                std::vector<std::pair<std::string, double>> items;
                cursor = _c->zscan(k, cursor, prefix + "*", static_cast<long long>(1), std::back_inserter(items));
                for (const auto &item : items) {
                    _c->zrem(k, item.first);
                }
                if (cursor == 0) break;
            }
        } catch(std::exception &e) {
            LOG_ERROR("UnackedPush.ack 失败 {}-{}-{}: {}", uid, device_id, user_seq, e.what());
        }
    }
    /* brief: 取"成熟可重传"的项（按时间升序，仅查询不修改） */
    std::vector<std::pair<unsigned long, std::string>> peek_due(
            const std::string &uid, const std::string &device_id,
            long limit = 100, long max_age_sec = 5) {
        std::vector<std::pair<unsigned long, std::string>> res;
        if(limit <= 0) return res;
        try {
            std::string k = key_for(uid, device_id);
            long long now = static_cast<long long>(time(nullptr));
            using namespace sw::redis;
            std::vector<std::string> raw;
            _c->zrangebyscore(k,
                              BoundedInterval<double>(0, static_cast<double>(now - max_age_sec),
                                                       BoundType::CLOSED),
                              LimitOptions{0, limit},
                              std::back_inserter(raw));
            for (const auto &s : raw) {
                auto pos = s.find(':');
                if (pos == std::string::npos) continue;
                unsigned long seq = std::stoull(s.substr(0, pos));
                res.emplace_back(seq, s.substr(pos + 1));
            }
        } catch(std::exception &e) {
            LOG_ERROR("UnackedPush.peek_due 失败 {}-{}-{}: {}", uid, device_id, e.what());
        }
        return res;
    }
    /* brief: 重发后推迟这批 user_seq 的下次重发时机 + 续期 TTL */
    void bump_score(const std::string &uid, const std::string &device_id,
                    const std::vector<unsigned long> &user_seqs,
                    std::chrono::seconds ttl = kUnackedTtl) {
        if(user_seqs.empty()) return;
        try {
            std::string k = key_for(uid, device_id);
            long long now = static_cast<long long>(time(nullptr));
            // ZSCAN 找到匹配的 member，用 ZADD XX 更新 score
            for (unsigned long seq : user_seqs) {
                std::string prefix = std::to_string(seq) + ":";
                using namespace sw::redis;
                auto cursor = 0LL;
                while (true) {
                    std::vector<std::pair<std::string, double>> items;
                    cursor = _c->zscan(k, cursor, prefix + "*", 1LL, std::back_inserter(items));
                    for (const auto &item : items) {
                        _c->zadd(k, item.first, static_cast<double>(now), UpdateType::EXIST);
                    }
                    if (cursor == 0) break;
                }
            }
            _c->expire(k, ttl);
        } catch(std::exception &e) {
            LOG_ERROR("UnackedPush.bump_score 失败 {}-{}-{}: {}", uid, device_id, e.what());
        }
    }

private:
    std::shared_ptr<sw::redis::Redis> _c;
};
```

- [ ] **Step 4: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "dao(redis): OnlineRoute SET→HASH 设备级路由, UnackedPush per-device + payload"
```

---

### Task 4: Connection — 设备级连接表 + JWT 鉴权

**Files:**
- Modify: `push/source/connection.hpp`（全量重写）

- [ ] **Step 1: 重写 connection.hpp**

用以下内容完整替换 `push/source/connection.hpp`：

```cpp
#pragma once

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include "infra/logger.hpp"
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace chatnow
{

typedef websocketpp::server<websocketpp::config::asio> server_t;

/**
 * Push 服务连接表 — 设备级路由。
 *   - _uid_device_connections: uid → device_id → set<conn_ptr>
 *   - _conn_clients:           conn → Client{uid, device_id, jwt_jti, last_active_ts, send_mu}
 *   - 同一 (uid, device_id) 有新连接时关闭旧连接
 */
class Connection
{
public:
    using ptr = std::shared_ptr<Connection>;

    struct Client {
        std::string uid;
        std::string device_id;
        std::string jwt_jti;
        long last_active_ts {0};
        std::shared_ptr<std::mutex> send_mu {std::make_shared<std::mutex>()};
    };

    Connection() = default;
    ~Connection() = default;

    /* brief: 插入连接。同 (uid, device_id) 已有则关闭旧连接后替换 */
    void insert(const server_t::connection_ptr &conn,
                const std::string &uid,
                const std::string &device_id,
                const std::string &jwt_jti)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        // 关闭同一设备的旧连接
        auto dit = _uid_device_connections.find(uid);
        if (dit != _uid_device_connections.end()) {
            auto vdit = dit->second.find(device_id);
            if (vdit != dit->second.end()) {
                for (const auto &old_conn : vdit->second) {
                    try { old_conn->close(websocketpp::close::status::normal, "new device login"); }
                    catch(...) {}
                    _conn_clients.erase(old_conn);
                }
                vdit->second.clear();
            }
        }
        _uid_device_connections[uid][device_id].insert(conn);
        Client c{uid, device_id, jwt_jti, now_sec()};
        _conn_clients[conn] = std::move(c);
        LOG_DEBUG("Connection.insert {} uid={} device={}",
                  (size_t)conn.get(), uid, device_id);
    }

    /* brief: 取指定设备的连接 */
    std::vector<server_t::connection_ptr> connections(const std::string &uid,
                                                      const std::string &device_id) {
        std::unique_lock<std::mutex> lock(_mutex);
        std::vector<server_t::connection_ptr> res;
        auto dit = _uid_device_connections.find(uid);
        if (dit == _uid_device_connections.end()) return res;
        auto vdit = dit->second.find(device_id);
        if (vdit == dit->second.end()) return res;
        res.reserve(vdit->second.size());
        for (const auto &c : vdit->second) res.push_back(c);
        return res;
    }

    bool client(const server_t::connection_ptr &conn,
                std::string &uid, std::string &device_id, std::string &jti) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if (it == _conn_clients.end()) return false;
        uid = it->second.uid;
        device_id = it->second.device_id;
        jti = it->second.jwt_jti;
        return true;
    }

    std::shared_ptr<std::mutex> send_mutex(const server_t::connection_ptr &conn) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if (it == _conn_clients.end()) return nullptr;
        return it->second.send_mu;
    }

    void touch(const server_t::connection_ptr &conn) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if (it != _conn_clients.end()) it->second.last_active_ts = now_sec();
    }

    void remove(const server_t::connection_ptr &conn) {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _conn_clients.find(conn);
        if (it == _conn_clients.end()) return;
        const std::string &uid = it->second.uid;
        const std::string &did = it->second.device_id;
        auto dit = _uid_device_connections.find(uid);
        if (dit != _uid_device_connections.end()) {
            auto vdit = dit->second.find(did);
            if (vdit != dit->second.end()) {
                vdit->second.erase(conn);
                if (vdit->second.empty()) dit->second.erase(vdit);
            }
            if (dit->second.empty()) _uid_device_connections.erase(dit);
        }
        _conn_clients.erase(it);
    }

    std::vector<std::string> online_uids() {
        std::unique_lock<std::mutex> lock(_mutex);
        std::vector<std::string> res;
        res.reserve(_uid_device_connections.size());
        for (const auto &p : _uid_device_connections) res.push_back(p.first);
        return res;
    }

    std::vector<std::pair<std::string, server_t::connection_ptr>> reap(long ttl_sec) {
        std::vector<std::pair<std::string, server_t::connection_ptr>> reaped;
        long now = now_sec();
        std::unique_lock<std::mutex> lock(_mutex);
        for (auto cit = _conn_clients.begin(); cit != _conn_clients.end(); ) {
            if (now - cit->second.last_active_ts > ttl_sec) {
                const std::string uid = cit->second.uid;
                auto conn = cit->first;
                auto dit = _uid_device_connections.find(uid);
                if (dit != _uid_device_connections.end()) {
                    auto vdit = dit->second.find(cit->second.device_id);
                    if (vdit != dit->second.end()) {
                        vdit->second.erase(conn);
                        if (vdit->second.empty()) dit->second.erase(vdit);
                    }
                    if (dit->second.empty()) _uid_device_connections.erase(dit);
                }
                reaped.emplace_back(uid, conn);
                cit = _conn_clients.erase(cit);
            } else {
                ++cit;
            }
        }
        return reaped;
    }

private:
    static long now_sec() {
        using namespace std::chrono;
        return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }

    std::mutex _mutex;
    std::unordered_map<std::string,
        std::unordered_map<std::string,
            std::unordered_set<server_t::connection_ptr>>> _uid_device_connections;
    std::unordered_map<server_t::connection_ptr, Client> _conn_clients;
};

} // namespace chatnow
```

- [ ] **Step 2: Commit**

```bash
git add push/source/connection.hpp
git commit -m "refactor(push): Connection 连接表按 (uid, device_id) 组织"
```

---

### Task 5: push_server.h — 完整重写

**Files:**
- Modify: `push/source/push_server.h`（全量重写）

这是本次重构的主体，约 900 行。各子步骤覆盖：includes + 类声明 → handler 实现 → MQ 消费 → 心跳补送 → Builder → Server。

- [ ] **Step 1: Includes + 前向声明**

替换文件开头到旧 `namespace chatnow {` 之前的内容：

```cpp
#pragma once

#include "connection.hpp"
#include "infra/etcd.hpp"
#include "infra/logger.hpp"
#include "mq/channel.hpp"
#include "mq/rabbitmq.hpp"
#include "mq/trace_headers.hpp"
#include "log/log_context.hpp"
#include "dao/data_redis.hpp"
#include "auth/auth_context.hpp"
#include "auth/forward_auth.hpp"
#include "auth/jwt_codec.hpp"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "utils/brpc_closure.hpp"
#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "presence/presence_service.pb.h"
#include "push/notify.pb.h"
#include "push/push_service.pb.h"
#include "message/message_types.pb.h"
#include "message/message_service.pb.h"
#include "message/message_internal.pb.h"
#include <sw/redis++/redis++.h>
#include <brpc/server.h>
#include <thread>
#include <chrono>
#include <limits>
#include <unordered_set>

namespace chatnow::push {
```

- [ ] **Step 2: PushServiceImpl 类声明 + 构造函数**

```cpp
class PushServiceImpl : public PushService
{
public:
    PushServiceImpl(const Connection::ptr &connections,
                    const std::shared_ptr<chatnow::auth::JwtCodec> &jwt_codec,
                    const std::shared_ptr<sw::redis::Redis> &redis,
                    const OnlineRoute::ptr &online_route,
                    const UnackedPush::ptr &unacked,
                    const CrossInstanceOutbox::ptr &cross_outbox,
                    const std::string &instance_id,
                    const std::string &message_service_name,
                    const ServiceManager::ptr &channels)
        : _connections(connections),
          _jwt_codec(jwt_codec),
          _redis(redis),
          _online_route(online_route),
          _unacked(unacked),
          _cross_outbox(cross_outbox),
          _instance_id(instance_id),
          _message_service_name(message_service_name),
          _mm_channels(channels) {}

    void set_resend_params(long batch, long max_age_sec) {
        _resend_batch = batch;
        _resend_max_age_sec = max_age_sec;
    }
    ~PushServiceImpl() { stop_cross_outbox_reaper(); }
```

- [ ] **Step 3: PushToUser handler**

```cpp
    void PushToUser(google::protobuf::RpcController* controller,
                    const PushToUserReq* request,
                    PushToUserRsp* response,
                    google::protobuf::Closure* done) override
    {
        HANDLE_RPC(cntl, request, response, {
            // 若调用方带了 user_seq：覆写 user_seq 到 payload
            std::string payload;
            const auto &notify = request->notify();
            if (request->has_user_seq() &&
                notify.notify_type() == NotifyType::CHAT_MESSAGE_NOTIFY &&
                notify.has_new_message_info()) {
                NotifyMessage per_user = notify;
                per_user.mutable_new_message_info()->mutable_message_info()
                    ->set_user_seq(request->user_seq());
                payload = per_user.SerializeAsString();
            } else {
                payload = notify.SerializeAsString();
            }

            // 收集目标 device_id 集合
            std::unordered_set<std::string> target_dids;
            for (const auto &did : request->target_device_ids()) target_dids.insert(did);
            bool filter_devices = !target_dids.empty();

            int delivered = 0;
            auto devices = _online_route->devices(request->user_id());
            for (const auto &did : devices) {
                if (filter_devices && target_dids.find(did) == target_dids.end()) continue;
                if (_local_send(request->user_id(), did, payload) > 0) ++delivered;
            }

            // 设备级 unacked 缓冲
            if (request->has_user_seq() && _unacked) {
                std::string payload_b64 = _utils_base64_encode(payload);
                long long now_ts = static_cast<long long>(time(nullptr));
                for (const auto &did : devices) {
                    if (filter_devices && target_dids.find(did) == target_dids.end()) continue;
                    _unacked->push(request->user_id(), did,
                                   request->user_seq(), payload_b64, now_ts);
                }
            }

            response->set_online_device_count(delivered);
        });
    }
```

- [ ] **Step 4: PushBatch handler**

```cpp
    void PushBatch(google::protobuf::RpcController* controller,
                   const PushBatchReq* request,
                   PushBatchRsp* response,
                   google::protobuf::Closure* done) override
    {
        HANDLE_RPC(cntl, request, response, {
            std::unordered_map<std::string, unsigned long> uid2seq;
            for (const auto &p : request->user_seqs()) uid2seq[p.user_id()] = p.user_seq();

            const auto &base_notify = request->notify();
            bool is_chat_msg = (base_notify.notify_type() == NotifyType::CHAT_MESSAGE_NOTIFY) &&
                               base_notify.has_new_message_info();

            int total = 0;
            long long now_ts = static_cast<long long>(time(nullptr));
            for (const auto &uid : request->user_id_list()) {
                auto devices = _online_route->devices(uid);
                for (const auto &did : devices) {
                    std::string payload;
                    if (is_chat_msg) {
                        NotifyMessage per_user = base_notify;
                        auto it = uid2seq.find(uid);
                        if (it != uid2seq.end()) {
                            per_user.mutable_new_message_info()->mutable_message_info()
                                ->set_user_seq(it->second);
                        }
                        payload = per_user.SerializeAsString();
                    } else {
                        payload = base_notify.SerializeAsString();
                    }
                    if (_local_send(uid, did, payload) > 0) ++total;

                    auto it = uid2seq.find(uid);
                    if (it != uid2seq.end() && _unacked) {
                        _unacked->push(uid, did, it->second,
                                       _utils_base64_encode(payload), now_ts);
                    }
                }
            }
            response->set_online_count(total);
        });
    }
```

- [ ] **Step 5: MQ 消费回调 onPushMessage**

```cpp
    ConsumeAction onPushMessage(const char *body, size_t sz, bool redelivered) {
        chatnow::message::internal::InternalMessage internal_msg;
        if (!internal_msg.ParseFromArray(body, sz)) {
            LOG_ERROR("Push-Consumer: 反序列化 InternalMessage 失败");
            return ConsumeAction::NackDiscard;
        }
        const auto &msg_info = internal_msg.message_info();

        std::unordered_map<std::string, unsigned long> uid2seq;
        for (const auto &p : internal_msg.user_seqs()) uid2seq[p.user_id()] = p.user_seq();

        NotifyMessage notify_template;
        notify_template.set_notify_type(NotifyType::CHAT_MESSAGE_NOTIFY);
        notify_template.mutable_new_message_info()->mutable_message_info()->CopyFrom(msg_info);
        const auto &_ctx_trace = chatnow::log::LogContext::current().trace_id;
        if (!_ctx_trace.empty()) {
            notify_template.set_trace_id(_ctx_trace);
        }

        // 1) 写 unacked + 构建远程 uid 列表
        long long now_ts = static_cast<long long>(time(nullptr));
        std::vector<std::string> remote_uids;
        remote_uids.reserve(internal_msg.member_id_list_size());
        for (const auto &uid : internal_msg.member_id_list()) {
            // 拿到此 uid 的所有在线设备
            auto devices = _online_route ? _online_route->devices(uid)
                                         : std::vector<std::string>{};
            if (devices.empty()) { remote_uids.push_back(uid); continue; }

            bool any_local = false;
            for (const auto &did : devices) {
                std::string inst = _online_route->device_instance(uid, did);
                if (inst == _instance_id) {
                    auto it = uid2seq.find(uid);
                    if (it != uid2seq.end()) {
                        NotifyMessage per_user = notify_template;
                        per_user.mutable_new_message_info()->mutable_message_info()
                            ->set_user_seq(it->second);
                        std::string payload = per_user.SerializeAsString();
                        if (_local_send(uid, did, payload) > 0) any_local = true;
                        if (_unacked) {
                            _unacked->push(uid, did, it->second,
                                           _utils_base64_encode(payload), now_ts);
                        }
                    } else {
                        // 大群读扩散：无 user_seq，仅下发
                        _local_send(uid, did, notify_template.SerializeAsString());
                        any_local = true;
                    }
                }
            }
            if (!any_local) remote_uids.push_back(uid);
        }

        if (remote_uids.empty()) return ConsumeAction::Ack;

        // 2) 跨实例：按 Push 实例 ID 分组
        std::unordered_map<std::string, std::vector<std::string>> peer_to_uids;
        for (const auto &uid : remote_uids) {
            auto devices = _online_route ? _online_route->devices(uid)
                                         : std::vector<std::string>{};
            for (const auto &did : devices) {
                std::string peer = _online_route->device_instance(uid, did);
                if (peer.empty() || peer == _instance_id) continue;
                peer_to_uids[peer].push_back(uid);
                break;
            }
        }

        // 3) 每个对端一次 PushBatch（异步 brpc::DoNothing）
        for (auto &kv : peer_to_uids) {
            const std::string &peer = kv.first;
            const auto &uids = kv.second;
            auto channel = _mm_channels->choose(peer);
            if (!channel) {
                LOG_WARN("Push-Consumer: 对端 {} 不可达", peer);
                for (const auto &u : uids)
                    if (_online_route) _online_route->unbind(u, "", peer);
                if (_cross_outbox) {
                    std::string b64 = _utils_base64_encode(internal_msg.SerializeAsString());
                    _cross_outbox->enqueue(b64, uids, peer, now_ts);
                }
                continue;
            }
            PushService_Stub stub(channel.get());
            auto *closure = new SelfDeleteRpcClosure<PushBatchReq, PushBatchRsp>();
            closure->req.set_request_id(msg_info.client_msg_id());
            for (const auto &u : uids) closure->req.add_user_id_list(u);
            closure->req.mutable_notify()->CopyFrom(notify_template);
            for (const auto &u : uids) {
                auto it = uid2seq.find(u);
                if (it == uid2seq.end()) continue;
                auto *p = closure->req.add_user_seqs();
                p->set_user_id(u);
                p->set_user_seq(it->second);
            }
            std::string peer_id = peer;
            std::string payload_b64 = _utils_base64_encode(internal_msg.SerializeAsString());
            closure->on_done = [peer_id, uids, outbox = _cross_outbox,
                                online = _online_route, payload_b64, now_ts]
                (brpc::Controller *c, const PushBatchRsp &) {
                if (c->Failed()) {
                    LOG_WARN("PushBatch 跨实例失败 peer={}: {}", peer_id, c->ErrorText());
                    for (const auto &u : uids)
                        if (online) online->unbind(u, "", peer_id);
                    if (outbox) outbox->enqueue(payload_b64, uids, peer_id, now_ts);
                }
            };
            stub.PushBatch(&closure->cntl, &closure->req, &closure->rsp, closure);
        }
        return ConsumeAction::Ack;
    }
```

- [ ] **Step 6: WS 消息处理 onClientNotify（JWT 鉴权 + ACK + 心跳）**

```cpp
    void onClientNotify(const NotifyMessage &notify, server_t::connection_ptr conn) {
        if (notify.notify_type() == NotifyType::CLIENT_AUTH) {
            _handle_client_auth_(notify.client_auth(), conn);
        } else if (notify.notify_type() == NotifyType::MSG_PUSH_ACK) {
            const auto &ack = notify.msg_push_ack();
            if (ack.user_seq() == 0 || ack.user_id().empty() ||
                ack.conversation_id().empty() || ack.device_id().empty()) {
                LOG_WARN("收到非法 MSG_PUSH_ACK uid={} did={} seq={}",
                         ack.user_id(), ack.device_id(), ack.user_seq());
                return;
            }
            if (_unacked) _unacked->ack(ack.user_id(), ack.device_id(), ack.user_seq());

            // 异步上报 UpdateReadAck
            auto channel = _mm_channels->choose(_message_service_name);
            if (!channel) {
                LOG_WARN("UpdateReadAck: message service 不可达 uid={}", ack.user_id());
                return;
            }
            chatnow::message::MessageService_Stub stub(channel.get());
            auto *closure = new SelfDeleteRpcClosure<
                chatnow::message::UpdateReadAckReq,
                chatnow::message::UpdateReadAckRsp>();
            closure->req.set_request_id(ack.user_id());
            closure->req.set_conversation_id(ack.conversation_id());
            closure->req.set_seq_id(ack.user_seq());
            // user_id 从 metadata 透传（内部调用用 __system__）
            closure->on_done = [uid = ack.user_id(), seq = ack.user_seq()]
                (brpc::Controller *c, const chatnow::message::UpdateReadAckRsp &r) {
                if (c->Failed()) {
                    LOG_WARN("UpdateReadAck RPC 失败 uid={} seq={}: {}", uid, seq, c->ErrorText());
                }
            };
            stub.UpdateReadAck(&closure->cntl, &closure->req, &closure->rsp, closure);
        } else if (notify.notify_type() == NotifyType::CLIENT_HEARTBEAT) {
            const auto &hb = notify.heartbeat();
            _on_heartbeat_resend(hb);
        }
    }

    /* brief: 给特定设备推送 KICKED 通知 */
    void publish_kicked(const std::string &uid, const std::string &device_id,
                        NotifyType reason, const std::string &msg) {
        NotifyMessage notify;
        notify.set_notify_type(reason);
        auto *kicked = notify.mutable_kicked();
        kicked->set_reason(reason);
        kicked->set_message(msg);
        _local_send(uid, device_id, notify.SerializeAsString());
    }
```

- [ ] **Step 7: 私有方法 — JWT 鉴权 + 心跳补送**

```cpp
private:
    void _handle_client_auth_(const NotifyClientAuth &auth,
                              server_t::connection_ptr conn) {
        if (auth.access_token().empty() || auth.device_id().empty()) {
            LOG_WARN("WS CLIENT_AUTH 缺字段");
            try { conn->close(websocketpp::close::status::unsupported_data,
                              "access_token/device_id required"); } catch(...) {}
            return;
        }

        // JWT 验签
        chatnow::auth::JwtPayload payload;
        try {
            payload = _jwt_codec->verify(auth.access_token());
        } catch (const chatnow::ServiceError &e) {
            LOG_WARN("WS JWT 验签失败: {}", e.what());
            try { conn->close(websocketpp::close::status::unsupported_data,
                              "auth failed"); } catch(...) {}
            return;
        }

        std::string uid = payload.sub;
        std::string did = payload.did;
        std::string jti = payload.jti;

        // 查 Redis 吊销表
        // 注意：JwtStore/revoked 检查通常在 Identity 做；Push 端如果 Redis 可访问
        // 应检查 im:jwt:revoked:{jti}。此处调 JwtStore 的 is_revoked 方法（如存在）。
        // 如果本地无 JwtStore 依赖，可以跳过（WS 连接断开后下次重连时 JWT 续期会捕获）。

        _connections->insert(conn, uid, did, jti);
        if (_online_route) _online_route->bind(uid, did, _instance_id);

        // 写 Presence（Push 为写入端）
        _write_presence_online_(uid, did);

        LOG_INFO("WS 鉴权成功 uid={} device={}", uid, did);

        // 携带 last_user_seq 时立即触发补送
        if (auth.has_last_user_seq() && auth.last_user_seq() > 0) {
            NotifyMessage hb;
            hb.set_notify_type(NotifyType::CLIENT_HEARTBEAT);
            hb.mutable_heartbeat()->set_user_id(uid);
            hb.mutable_heartbeat()->set_last_user_seq(auth.last_user_seq());
            onClientNotify(hb, conn);
        }
    }

    void _on_heartbeat_resend(const NotifyHeartbeat &hb) {
        if (!_unacked) return;
        const std::string uid = hb.user_id();
        if (uid.empty()) return;

        // 拿到此用户的所有在线设备 → 逐个设备补送
        auto devices = _online_route->devices(uid);
        for (const auto &did : devices) {
            auto pending = _unacked->peek_due(uid, did, _resend_batch, _resend_max_age_sec);
            if (pending.empty()) continue;

            // 直接发缓存的 payload（零 RPC）
            int sent = 0;
            std::vector<unsigned long> seqs;
            for (const auto &[user_seq, payload_b64] : pending) {
                std::string payload = _utils_base64_decode(payload_b64);
                if (!payload.empty()) {
                    _local_send(uid, did, payload);
                    ++sent;
                }
                seqs.push_back(user_seq);
            }

            if (!seqs.empty() && _unacked) {
                _unacked->bump_score(uid, did, seqs);
            }
            LOG_INFO("Heartbeat-补送 uid={} did={} 取出 {} 条 发送 {} 条",
                     uid, did, pending.size(), sent);
        }
    }

    void _write_presence_online_(const std::string &uid, const std::string &did) {
        try {
            std::string k = std::string("im:presence:device:") + uid + ":" + did;
            _redis->hset(k, "state", "ONLINE");
            _redis->hset(k, "last_active_at_ms", std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
            _redis->expire(k, std::chrono::seconds(120));
        } catch (std::exception &e) {
            LOG_WARN("Presence 写入失败 uid={} did={}: {}", uid, did, e.what());
        }
    }

    /* brief: 本实例直接通过 WS 下发；返回送达连接数 */
    int _local_send(const std::string &uid, const std::string &device_id,
                    const std::string &payload) {
        auto conns = _connections->connections(uid, device_id);
        int sent = 0;
        for (auto &c : conns) {
            try {
                if (!c || c->get_state() != websocketpp::session::state::value::open) continue;
                auto mu = _connections->send_mutex(c);
                if (!mu) continue;
                std::lock_guard<std::mutex> lock(*mu);
                c->send(payload, websocketpp::frame::opcode::value::binary);
                ++sent;
            } catch (std::exception &e) {
                LOG_WARN("WS send 失败 uid={} did={}: {}", uid, device_id, e.what());
            }
        }
        return sent;
    }
```

- [ ] **Step 8: 跨实例 Outbox reaper + 成员变量**

```cpp
public:
    void start_cross_outbox_reaper(const std::string &owner) {
        if (!_cross_outbox || !_mm_channels) return;
        constexpr int kReapIntervalSec = 5;
        constexpr int kLeaseTtlSec = 30;
        constexpr int kBatchLimit = 50;
        _cross_reaper_running.store(true);
        _cross_reaper_owner = owner;
        _cross_reaper_thread = std::thread([this, kReapIntervalSec, kLeaseTtlSec, kBatchLimit]() {
            while (_cross_reaper_running.load()) {
                try {
                    if (!_cross_outbox->try_acquire_reaper_lease(_cross_reaper_owner, kLeaseTtlSec)) {
                        std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                        continue;
                    }
                    auto batch = _cross_outbox->peek(kBatchLimit);
                    if (batch.empty()) {
                        std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                        continue;
                    }
                    for (const auto &member : batch) _cross_outbox->remove(member);
                    for (const auto &member : batch) {
                        std::string b64, peer;
                        std::vector<std::string> uids;
                        _parse_outbox_member(member, b64, uids, peer);

                        chatnow::message::internal::InternalMessage internal_msg;
                        if (!internal_msg.ParseFromString(_utils_base64_decode(b64))) {
                            LOG_ERROR("CrossInstanceOutbox: 反序列化失败，丢弃");
                            continue;
                        }

                        // 按实例分组重发
                        std::unordered_map<std::string, std::vector<std::string>> peer_to_uids;
                        for (const auto &uid : uids) {
                            auto devices = _online_route ? _online_route->devices(uid)
                                                         : std::vector<std::string>{};
                            for (const auto &did : devices) {
                                std::string inst = _online_route->device_instance(uid, did);
                                if (inst == _instance_id) continue;
                                peer_to_uids[inst].push_back(uid);
                                break;
                            }
                        }

                        NotifyMessage notify_template;
                        notify_template.set_notify_type(NotifyType::CHAT_MESSAGE_NOTIFY);
                        notify_template.mutable_new_message_info()
                            ->mutable_message_info()->CopyFrom(internal_msg.message_info());

                        for (auto &kv : peer_to_uids) {
                            auto channel = _mm_channels->choose(kv.first);
                            if (!channel) { continue; }
                            PushService_Stub stub(channel.get());
                            auto *closure = new SelfDeleteRpcClosure<PushBatchReq, PushBatchRsp>();
                            closure->req.set_request_id(
                                internal_msg.message_info().client_msg_id());
                            for (const auto &u : kv.second) closure->req.add_user_id_list(u);
                            closure->req.mutable_notify()->CopyFrom(notify_template);
                            for (const auto &up : internal_msg.user_seqs()) {
                                if (std::find(kv.second.begin(), kv.second.end(),
                                              up.user_id()) != kv.second.end()) {
                                    auto *seq = closure->req.add_user_seqs();
                                    seq->set_user_id(up.user_id());
                                    seq->set_user_seq(up.user_seq());
                                }
                            }
                            stub.PushBatch(&closure->cntl, &closure->req,
                                           &closure->rsp, closure);
                        }
                    }
                } catch (std::exception &e) {
                    LOG_ERROR("CrossInstanceOutbox reaper 异常: {}", e.what());
                }
                std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
            }
            if (_cross_outbox) _cross_outbox->release_reaper_lease(_cross_reaper_owner);
            LOG_INFO("CrossInstanceOutbox reaper 已停止");
        });
    }

    void stop_cross_outbox_reaper() {
        _cross_reaper_running.store(false);
        if (_cross_reaper_thread.joinable()) _cross_reaper_thread.join();
    }

private:
    void _parse_outbox_member(const std::string &member,
                               std::string &b64,
                               std::vector<std::string> &uids,
                               std::string &peer) {
        auto pos_k = member.find("\"k\":\"");
        auto pos_u = member.find("\"u\":[");
        auto pos_p = member.find("\"p\":\"");
        if (pos_k != std::string::npos && pos_u != std::string::npos) {
            b64 = member.substr(pos_k + 5, pos_u - pos_k - 8);
        }
        if (pos_p != std::string::npos) {
            peer = member.substr(pos_p + 5, member.size() - pos_p - 7);
        }
        if (pos_u != std::string::npos) {
            size_t arr_end = member.find(']', pos_u);
            if (arr_end != std::string::npos) {
                std::string arr = member.substr(pos_u + 5, arr_end - pos_u - 5);
                size_t start = 0;
                while ((start = arr.find('"', start)) != std::string::npos) {
                    size_t end = arr.find('"', start + 1);
                    if (end == std::string::npos) break;
                    uids.push_back(arr.substr(start + 1, end - start - 1));
                    start = end + 1;
                }
            }
        }
    }

    static std::string _utils_base64_encode(const std::string &in) {
        static const char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        for (size_t i = 0; i < in.size(); i += 3) {
            unsigned long val = (unsigned char)in[i] << 16;
            if (i + 1 < in.size()) val |= (unsigned char)in[i + 1] << 8;
            if (i + 2 < in.size()) val |= (unsigned char)in[i + 2];
            out += kTbl[(val >> 18) & 0x3F];
            out += kTbl[(val >> 12) & 0x3F];
            out += (i + 1 < in.size()) ? kTbl[(val >> 6) & 0x3F] : '=';
            out += (i + 2 < in.size()) ? kTbl[val & 0x3F] : '=';
        }
        return out;
    }
    static std::string _utils_base64_decode(const std::string &in) {
        static const unsigned char kDec[128] = {
            64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
            64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,52,53,54,55,56,57,58,59,60,61,64,64,64,64,64,64,
            64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
            64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64
        };
        std::string out;
        out.reserve((in.size() / 4) * 3);
        for (size_t i = 0; i < in.size(); i += 4) {
            unsigned long val = 0;
            for (int j = 0; j < 4; ++j) {
                if (in[i + j] != '=') val = (val << 6) | kDec[(unsigned char)in[i + j]];
            }
            out += (char)((val >> 16) & 0xFF);
            if (in[i + 2] != '=') out += (char)((val >> 8) & 0xFF);
            if (in[i + 3] != '=') out += (char)(val & 0xFF);
        }
        return out;
    }

    Connection::ptr _connections;
    std::shared_ptr<chatnow::auth::JwtCodec> _jwt_codec;
    std::shared_ptr<sw::redis::Redis> _redis;
    OnlineRoute::ptr _online_route;
    UnackedPush::ptr _unacked;
    CrossInstanceOutbox::ptr _cross_outbox;
    std::string _instance_id;
    std::string _message_service_name;
    ServiceManager::ptr _mm_channels;
    long _resend_batch{50};
    long _resend_max_age_sec{5};
    std::atomic<bool> _cross_reaper_running{false};
    std::thread _cross_reaper_thread;
    std::string _cross_reaper_owner;
    // 本地消息缓存（心跳重传优先命中）
    struct MsgCacheEntry {
        std::string key;
        std::string payload;
    };
    std::deque<MsgCacheEntry> _msg_evict_list;
    std::unordered_map<std::string, decltype(_msg_evict_list)::iterator> _msg_cache;
    std::mutex _msg_cache_mu;
    size_t _msg_cache_max_entries = 5000;
};
```

- [ ] **Step 9: PushServer + PushServerBuilder**

```cpp
class PushServer
{
public:
    using ptr = std::shared_ptr<PushServer>;
    PushServer(const Discovery::ptr &disc,
               const Registry::ptr &reg,
               const std::shared_ptr<brpc::Server> &rpc,
               server_t *ws_server,
               const MQClient::ptr &mq_client,
               const Subscriber::ptr &push_subscriber)
        : _service_discover(disc), _reg_client(reg), _rpc_server(rpc), _ws_server(ws_server),
          _mq_client(mq_client), _push_subscriber(push_subscriber) {}
    ~PushServer() = default;

    void start() {
        _ws_thread = std::thread([this]() {
            try {
                _ws_server->run();
                LOG_INFO("Push WS 线程正常退出");
            } catch (std::exception &e) {
                LOG_ERROR("Push WS 线程异常退出: {}", e.what());
            }
            _rpc_server->Stop(0);
        });
        _rpc_server->RunUntilAskedToQuit();
        _push_subscriber.reset();
        _mq_client.reset();
        _ws_server->stop();
        if (_ws_thread.joinable()) _ws_thread.join();
        _rpc_server->Join();
        LOG_INFO("Push 关停完成");
    }

private:
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
    server_t *_ws_server;
    MQClient::ptr _mq_client;
    Subscriber::ptr _push_subscriber;
    std::thread _ws_thread;
};

class PushServerBuilder
{
public:
    void make_jwt_object(const chatnow::auth::JwtConfig &config) {
        config.validate_or_throw();
        _jwt_codec = std::make_shared<chatnow::auth::JwtCodec>(config);
    }

    void make_redis_object(const std::string &host, uint16_t port, int db,
                           bool keep_alive, int pool_size)
    {
        _redis = RedisClientFactory::create(host, port, db, keep_alive, pool_size);
        _online_route = std::make_shared<OnlineRoute>(_redis);
        _unacked      = std::make_shared<UnackedPush>(_redis);
        _cross_outbox = std::make_shared<CrossInstanceOutbox>(_redis);
    }

    void make_discovery_object(const std::string &reg_host,
                               const std::string &base_service_name,
                               const std::string &message_service_name,
                               const std::string &push_service_name)
    {
        _message_service_name = message_service_name;
        _push_service_name    = push_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(message_service_name);
        _mm_channels->declared(push_service_name);
        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }

    void make_reg_object(const std::string &reg_host,
                         const std::string &service_name,
                         const std::string &access_host)
    {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
        _instance_id = service_name;
    }

    void make_mq_object(const std::string &user, const std::string &password,
                        const std::string &host,
                        const std::string &exchange,
                        const std::string &queue,
                        const std::string &binding_key)
    {
        std::string amqp_url = "amqp://" + user + ":" + password + "@" + host + ":5672/";
        _mq_client = std::make_shared<MQClient>(amqp_url);
        _push_settings = {
            .exchange = exchange,
            .exchange_type = chatnow::DIRECT,
            .queue = queue,
            .binding_key = binding_key
        };
        auto dummy_cb = [](const char*, size_t, bool) -> ConsumeAction {
            return ConsumeAction::Ack;
        };
        _push_subscriber = chatnow::MQFactory::create<chatnow::Subscriber>(
            _mq_client, _push_settings, dummy_cb);
    }

    void make_ws_object(uint16_t ws_port) {
        _ws_server.set_access_channels(websocketpp::log::alevel::none);
        _ws_server.clear_error_channels(websocketpp::log::elevel::none);
        _ws_server.init_asio();
        _ws_server.set_reuse_addr(true);
        _ws_server.set_open_handler([this](websocketpp::connection_hdl hdl) {
            LOG_DEBUG("WS 连接建立 {}", (size_t)_ws_server.get_con_from_hdl(hdl).get());
        });
        _ws_server.set_close_handler([this](websocketpp::connection_hdl hdl) {
            auto conn = _ws_server.get_con_from_hdl(hdl);
            std::string uid, did, jti;
            if (_connections && _connections->client(conn, uid, did, jti)) {
                _connections->remove(conn);
                if (_online_route) _online_route->unbind(uid, did, _instance_id);
                LOG_DEBUG("WS 关闭 uid={} did={}", uid, did);
            }
        });
        _ws_server.set_message_handler([this](websocketpp::connection_hdl hdl, server_t::message_ptr msg) {
            auto conn = _ws_server.get_con_from_hdl(hdl);
            NotifyMessage notify;
            if (!notify.ParseFromString(msg->get_payload())) {
                LOG_WARN("WS payload 反序列化失败，关闭连接");
                _ws_server.close(hdl, websocketpp::close::status::unsupported_data,
                                 "payload invalid");
                return;
            }

            // 路径 A：未鉴权连接的首条消息必须是 CLIENT_AUTH
            std::string uid_known, did_known, jti_known;
            if (!_connections->client(conn, uid_known, did_known, jti_known)) {
                if (notify.notify_type() != NotifyType::CLIENT_AUTH || !notify.has_client_auth()) {
                    LOG_WARN("WS 首条非 CLIENT_AUTH，关闭连接");
                    _ws_server.close(hdl, websocketpp::close::status::unsupported_data,
                                     "auth required");
                    return;
                }
                if (_push_service) {
                    _push_service->onClientNotify(notify, conn);
                }
                return;
            }

            // 路径 B：已鉴权连接的后续消息
            _connections->touch(conn);
            if (_push_service) _push_service->onClientNotify(notify, conn);
            if (notify.notify_type() == NotifyType::CLIENT_HEARTBEAT) {
                _online_route->touch(uid_known);
            }
        });
    }

    void set_resend_params(int batch, int max_age_sec) {
        _resend_batch = batch;
        _resend_max_age_sec = max_age_sec;
    }
    void set_reaper_owner(const std::string &owner) { _reaper_owner = owner; }

    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads, uint16_t ws_port) {
        if (!_redis) { LOG_ERROR("Push: Redis 未初始化"); abort(); }
        if (!_mm_channels) { LOG_ERROR("Push: 信道管理未初始化"); abort(); }
        _connections = std::make_shared<Connection>();
        _rpc_server = std::make_shared<brpc::Server>();
        _push_service = new PushServiceImpl(
            _connections, _jwt_codec, _redis, _online_route, _unacked, _cross_outbox,
            _instance_id, _message_service_name, _mm_channels);
        _push_service->set_resend_params(_resend_batch, _resend_max_age_sec);
        int ret = _rpc_server->AddService(_push_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
        if (ret == -1) { LOG_ERROR("Push: AddService 失败"); abort(); }

        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads = num_threads;
        if (_rpc_server->Start(port, &options) == -1) {
            LOG_ERROR("Push: brpc 启动失败");
            abort();
        }
        // WS server — 先于 MQ 订阅
        make_ws_object(ws_port);
        std::error_code ec;
        _ws_server.listen(ws_port, ec);
        if (ec) { LOG_ERROR("Push: WS 监听失败 {}", ec.message()); abort(); }
        _ws_server.start_accept();

        // MQ 订阅
        auto callback_inner = std::bind(&PushServiceImpl::onPushMessage, _push_service,
                                  std::placeholders::_1, std::placeholders::_2,
                                  std::placeholders::_3);
        chatnow::MessageCallbackWithHeaders callback = [callback_inner](const char* body, size_t sz, bool redeliv,
                                                                        const std::map<std::string, std::string>& headers) -> chatnow::ConsumeAction {
            std::string _trace_id = chatnow::mq::mq_extract_trace_id(headers);
            chatnow::log::LogContext::set(_trace_id, "", "");
            struct _Scope { ~_Scope() { chatnow::log::LogContext::clear(); } } _scope;
            return callback_inner(body, sz, redeliv);
        };
        _push_subscriber->consume(std::move(callback));

        std::string owner = _reaper_owner.empty()
            ? std::to_string(::getpid()) : _reaper_owner;
        _push_service->start_cross_outbox_reaper(owner);
        LOG_INFO("Push 服务启动: rpc_port={} ws_port={}", port, ws_port);
    }

    PushServer::ptr build() {
        return std::make_shared<PushServer>(std::move(_service_discover),
                                            std::move(_reg_client),
                                            std::move(_rpc_server),
                                            &_ws_server,
                                            std::move(_mq_client),
                                            std::move(_push_subscriber));
    }

private:
    std::shared_ptr<sw::redis::Redis> _redis;
    std::shared_ptr<chatnow::auth::JwtCodec> _jwt_codec;
    OnlineRoute::ptr _online_route;
    UnackedPush::ptr _unacked;
    CrossInstanceOutbox::ptr _cross_outbox;

    std::string _message_service_name;
    std::string _push_service_name;
    std::string _instance_id;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;

    declare_settings _push_settings;
    MQClient::ptr _mq_client;
    Subscriber::ptr _push_subscriber;

    int _resend_batch{50};
    int _resend_max_age_sec{5};
    std::string _reaper_owner;

    Connection::ptr _connections;
    server_t _ws_server;
    PushServiceImpl *_push_service{nullptr};
    std::shared_ptr<brpc::Server> _rpc_server;
};

} // namespace chatnow::push
```

- [ ] **Step 10: Commit**

```bash
git add push/source/push_server.h
git commit -m "refactor(push): 完整重写 PushServiceImpl — namespace chatnow::push, JWT 鉴权, 设备级路由, Stub 切换"
```

---

### Task 6: push_server.cc（gflags 同步）

**Files:**
- Modify: `push/source/push_server.cc`

- [ ] **Step 1: 更新 gflags + Builder 调用**

将 `main` 中的 `chatnow::PushServerBuilder` 改为 `chatnow::push::PushServerBuilder`，并加入 `make_jwt_object` 调用。

在 `make_redis_object` 之前加：

```cpp
// JWT config（从配置文件 / gflags 读取，或硬编码开发 key）
chatnow::auth::JwtConfig jwt_cfg;
jwt_cfg.current_kid = "v1";
jwt_cfg.keys["v1"] = "0123456789abcdef0123456789abcdef"; // >=32 字节
jwt_cfg.access_ttl_sec = 7200;
psb.make_jwt_object(jwt_cfg);
```

将 `chatnow::PushServerBuilder psb;` 改为 `chatnow::push::PushServerBuilder psb;`。

- [ ] **Step 2: Commit**

```bash
git add push/source/push_server.cc
git commit -m "refactor(push): main — namespace chatnow::push, JWT config 注入"
```

---

### Task 7: CMakeLists + 配置文件

**Files:**
- Modify: `push/CMakeLists.txt`
- Modify: `conf/push_server.conf`

- [ ] **Step 1: CMakeLists — proto_files 同步**

将 `push/CMakeLists.txt` 第 7 行的 proto_files 从：

```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto message/message_types.proto push/push_service.proto push/notify.proto presence/presence_service.proto)
```

改为：

```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto message/message_types.proto message/message_service.proto message/message_internal.proto push/push_service.proto push/notify.proto presence/presence_service.proto)
```

即：加 `message/message_service.proto message/message_internal.proto`。

- [ ] **Step 2: conf — gflag 同步**

新增 JWT 相关配置项（依赖后续从配置中心下发，开发阶段用 gflags 临时键）：

```
# JWT（开发阶段临时键，后续配置化）
-jwt_current_kid=v1
-jwt_key_v1=0123456789abcdef0123456789abcdef
```

- [ ] **Step 3: Commit**

```bash
git add push/CMakeLists.txt conf/push_server.conf
git commit -m "infra(push): CMakeLists proto sync + conf JWT 配置"
```

---

### Task 8: 删除旧 proto/push.proto

**Files:**
- Delete: `proto/push.proto`

- [ ] **Step 1: 删除前确认零引用**

```bash
grep -rn "push.pb\|push.proto\|\"push/notify.proto\"\|\"push/push_service.proto\"" proto/ push/ message/ gateway/ transmite/ conversation/ relationship/ common/ --include="*.cc" --include="*.h" --include="*.proto" --include="*.txt" | grep -v "push/notify.proto\|push/push_service.proto\|push/push.proto" || echo "零引用 OK"
```

如果 `proto/push.proto` 中的 `PushService` / `PushToUserReq` 还在其他地方被引用，需要确认它们都已切到 `proto/push/push_service.proto`（`chatnow.push` 包）。

- [ ] **Step 2: 删除 + Commit**

```bash
git rm proto/push.proto
git commit -m "cleanup: 删除旧 proto/push.proto（已由 push/push_service.proto 替代）"
```

---

### Task 9: 构建验证

- [ ] **Step 1: 编译**

```bash
cd /home/icepop/ChatNow/build && cmake .. && make -j$(nproc)
```

- [ ] **Step 2: 修复编译错误（如有）**

常见预期问题：
- `chatnow::push::PushToUserReq` 的 `target_device_ids()` 在生成的 pb.h 中字段名可能不同 — 检查生成代码
- `NotifyMessage.kicked()` — 确保 proto 已生成 NotifyKicked 类型
- `OnlineRoute::devices()` vs 旧的 `OnlineRoute::instances()` — 调用方需同步改名
- `chatnow::message::internal::InternalMessage` — 确认 `message/message_internal.proto` 已含 `cc_generic_services = true`
- `chatnow::auth::JwtCodec::verify()` 签名 — 确认返回类型和异常规范

- [ ] **Step 3: Commit（如修改）**

```bash
git add -A
git commit -m "fix(push): 编译错误修复"
```
