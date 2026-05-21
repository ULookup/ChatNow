# ES-MySQL 一致性 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Conversation 和 Identity 服务的 ES 写入增加重试 + Redis Outbox 兜底，消除 ES 写入静默失败导致的不一致。

**Architecture:** ES 直写 3 次指数退避重试，全部失败后事件入 Redis ESOutbox。独立 Reaper 线程每 5 秒从 Outbox 取出重放。ESOutbox key 按服务隔离。Reaper 使用独立 elasticlient::Client 保证线程安全。

**Tech Stack:** C++17, elasticlient, sw::redis++, jsoncpp, bvar

**Spec:** `docs/superpowers/specs/2026-05-21-es-mysql-consistency-design.md`

---

## File Map

| 文件 | 职责 |
|------|------|
| `common/dao/data_redis.hpp` | ESOutbox 支持自定义 Redis key |
| `common/infra/metrics.hpp` | 新增 `g_es_retry_total` bvar |
| `conversation/source/conversation_server.h` | 接入 Outbox + 重试 + Reaper |
| `identity/source/identity_server.h` | 接入 Outbox + 重试 + Reaper |

---

### Task 1: ESOutbox 支持自定义 key

**Files:**
- Modify: `common/dao/data_redis.hpp:834-861`

- [ ] **Step 1: 新增带 key 参数的构造函数**

在 `ESOutbox` 类中，`public:` 区域（`using ptr = ...` 之后，`ESOutbox(const RedisClient::ptr &c)` 之前）新增：

```cpp
    ESOutbox(const RedisClient::ptr &c, const std::string &key)
        : _c(c), _key(key) {}
```

- [ ] **Step 2: 将 `kEsOutboxKey` 改为成员变量**

把 `private:` 区的：

```cpp
    static constexpr const char *kEsOutboxKey     = "im:es:outbox";
```

改为：

```cpp
    std::string _key;
```

- [ ] **Step 3: 修改 `enqueue`、`peek`、`remove` 使用 `_key`**

将三个方法中的 `kEsOutboxKey` 替换为 `_key`：

```cpp
    void enqueue(const std::string &payload, long long score_ts) {
        try { _c->zadd(_key, payload, static_cast<double>(score_ts)); }
        catch(std::exception &e) { LOG_ERROR("ESOutbox.enqueue 失败: {}", e.what()); }
    }

    std::vector<std::string> peek(long limit = 50) {
        std::vector<std::string> res;
        try {
            _c->zrange(_key, 0, limit - 1, std::back_inserter(res));
        } catch(std::exception &e) { LOG_ERROR("ESOutbox.peek 失败: {}", e.what()); }
        return res;
    }

    void remove(const std::string &payload) {
        try { _c->zrem(_key, payload); }
        catch(std::exception &e) { LOG_ERROR("ESOutbox.remove 失败: {}", e.what()); }
    }
```

- [ ] **Step 4: 保留旧构造函数兼容性（Message 服务）**

旧构造函数 `ESOutbox(const RedisClient::ptr &c)` 保持不变，通过默认参数传给新构造函数：

```cpp
    ESOutbox(const RedisClient::ptr &c) : ESOutbox(c, "im:es:outbox") {}
```

确保 Message 服务编译不受影响。

- [ ] **Step 5: 提交**

```bash
git add common/dao/data_redis.hpp
git commit -m "feat(es): support custom Redis key in ESOutbox"
```

---

### Task 2: 新增 g_es_retry_total 指标

**Files:**
- Modify: `common/infra/metrics.hpp`

- [ ] **Step 1: 新增 bvar**

在 `g_degraded_es_write_total` 之后新增：

```cpp
inline bvar::Adder<long> g_es_retry_total;
```

- [ ] **Step 2: 提交**

```bash
git add common/infra/metrics.hpp
git commit -m "feat(metrics): add g_es_retry_total counter"
```

---

### Task 3: Conversation 服务 — 重试逻辑 + Outbox 序列化

**Files:**
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 新增 include**

在现有 include 区域（`#include "infra/metrics.hpp"` 之后）新增：

```cpp
#include <thread>
#include <chrono>
```

- [ ] **Step 2: 新增成员变量和构造函数参数**

在 `ConversationServiceImpl` 类中：

**构造函数签名** — 在末尾新增两个参数：

```cpp
    ConversationServiceImpl(const std::shared_ptr<elasticlient::Client> &es_client,
                            const std::shared_ptr<odb::core::database> &mysql_client,
                            const Members::ptr &members_cache,
                            const LastMessage::ptr &last_msg_cache,
                            const ServiceManager::ptr &channel_manager,
                            const std::string &identity_service_name,
                            const std::string &media_service_name,
                            const std::string &message_service_name,
                            const ConversationServiceConfig &cfg,
                            const ESOutbox::ptr &es_outbox,
                            const std::shared_ptr<elasticlient::Client> &es_reaper_client)
```

**初始化列表** — 新增两行：

```cpp
        : _es_conv(std::make_shared<ESConversation>(es_client)),
          ...
          _cfg(cfg),
          _es_outbox(es_outbox),
          _es_conv_reaper(std::make_shared<ESConversation>(es_reaper_client)) {}
```

**成员变量** — 在 `private:` 区末尾新增：

```cpp
    ESOutbox::ptr                          _es_outbox;
    ESConversation::ptr                    _es_conv_reaper;
```

- [ ] **Step 3: 新增 `retry_es_write_` 辅助方法**

在 `private:` 区，`_cfg` 声明之后新增：

```cpp
    /* brief: ES 直写 3 次指数退避重试，全失败入 Outbox */
    bool retry_es_write_(const std::string &outbox_payload,
                         std::function<bool()> es_op)
    {
        for (int i = 0; i < 3; ++i) {
            if (es_op()) return true;
            if (i < 2) {
                metrics::g_es_retry_total << 1;
                std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << i)));
            }
        }
        // 3 次全失败，入 Outbox
        if (_es_outbox) {
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            _es_outbox->enqueue(outbox_payload, static_cast<long long>(now_ms));
        }
        metrics::g_degraded_es_write_total << 1;
        return false;
    }
```

- [ ] **Step 4: 新增 Outbox payload 序列化辅助方法**

在 `parse_preview_json_` 之后新增三个静态方法：

```cpp
    static std::string serialize_member_ids_json_(const std::vector<std::string> &mids) {
        Json::Value arr(Json::arrayValue);
        for (const auto &uid : mids) arr.append(uid);
        std::string dst;
        Serialize(arr, dst);
        return dst;
    }

    /* Outbox payload formats:
     *   upsert:  {"op":"upsert","cid":"...","name":"...","type":0,"avatar":"...","status":0,"ut":123,"mids":[...]}
     *   delete:  {"op":"delete","cid":"..."}
     *   upd_mem: {"op":"upd_members","cid":"...","mids":[...]}
     */
    static std::string outbox_payload_upsert_(const chatnow::Conversation &c,
                                               const std::vector<std::string> &mids) {
        Json::Value root;
        root["op"] = "upsert";
        root["cid"] = c.conversation_id();
        root["name"] = c.conversation_name();
        root["type"] = static_cast<int>(c.conversation_type());
        root["avatar"] = c.avatar_id();
        root["status"] = static_cast<int>(c.status());
        static const boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
        root["ut"] = static_cast<Json::Int64>((c.update_time() - epoch).total_seconds());
        // member_ids JSON
        Json::Value marr(Json::arrayValue);
        for (const auto &uid : mids) marr.append(uid);
        root["mids"] = marr;
        std::string dst;
        Serialize(root, dst);
        return dst;
    }

    static std::string outbox_payload_delete_(const std::string &cid) {
        Json::Value root;
        root["op"] = "delete";
        root["cid"] = cid;
        std::string dst;
        Serialize(root, dst);
        return dst;
    }

    static std::string outbox_payload_upd_members_(const std::string &cid,
                                                    const std::vector<std::string> &mids) {
        Json::Value root;
        root["op"] = "upd_members";
        root["cid"] = cid;
        Json::Value marr(Json::arrayValue);
        for (const auto &uid : mids) marr.append(uid);
        root["mids"] = marr;
        std::string dst;
        Serialize(root, dst);
        return dst;
    }
```

- [ ] **Step 5: 新增 Reaper 用 `replay_es_write_` 方法**

在 `retry_es_write_` 之后新增（public 区或 private 区，Reaper 需要访问）：

```cpp
    /* brief: 解析 Outbox payload 并重放 ES 写入（Reaper 线程调用，使用独立 _es_conv_reaper） */
    bool replay_es_write_(const std::string &payload) {
        Json::Value root;
        if (!UnSerialize(payload, root)) return false;
        std::string op = root.get("op", "").asString();
        std::string cid = root.get("cid", "").asString();

        if (op == "upsert") {
            std::string name = root.get("name", "").asString();
            int type = root.get("type", 0).asInt();
            std::string avatar = root.get("avatar", "").asString();
            int status = root.get("status", 0).asInt();
            long ut = root.get("ut", 0).asInt64();
            static const boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
            boost::posix_time::ptime update_time = epoch + boost::posix_time::seconds(ut);

            chatnow::Conversation ent(cid, name,
                static_cast<chatnow::ConversationType>(type),
                update_time, 0, static_cast<chatnow::ConversationStatus>(status));
            if (!avatar.empty()) ent.avatar_id(avatar);

            std::vector<std::string> mids;
            const auto &marr = root["mids"];
            for (Json::ArrayIndex i = 0; i < marr.size(); ++i)
                mids.push_back(marr[i].asString());

            return _es_conv_reaper->append_data(ent, mids);
        }
        if (op == "delete") {
            return _es_conv_reaper->remove(cid);
        }
        if (op == "upd_members") {
            std::vector<std::string> mids;
            const auto &marr = root["mids"];
            for (Json::ArrayIndex i = 0; i < marr.size(); ++i)
                mids.push_back(marr[i].asString());
            return _es_conv_reaper->update_member_ids(cid, mids);
        }
        return false;
    }
```

- [ ] **Step 6: 提交**

```bash
git add conversation/source/conversation_server.h
git commit -m "feat(conversation): add ES retry logic, outbox serialization, and replay"
```

---

### Task 4: Conversation 服务 — 改造 6 个 ES 写入点

**Files:**
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 改造 CreateConversation（lines 183-184）**

将：

```cpp
            if (!_es_conv->append_data(ent, all_member_ids))
                metrics::g_degraded_es_write_total << 1;
```

改为：

```cpp
            std::string ob_payload = outbox_payload_upsert_(ent, all_member_ids);
            retry_es_write_(ob_payload, [&]() {
                return _es_conv->append_data(ent, all_member_ids);
            });
```

- [ ] **Step 2: 改造 UpdateConversation（lines 227-228）**

将：

```cpp
            if (!_es_conv->append_data(*c, uids))
                metrics::g_degraded_es_write_total << 1;
```

改为：

```cpp
            std::string ob_payload = outbox_payload_upsert_(*c, uids);
            retry_es_write_(ob_payload, [&]() {
                return _es_conv->append_data(*c, uids);
            });
```

- [ ] **Step 3: 改造 DismissConversation（lines 257-258）**

将：

```cpp
            if (!_es_conv->remove(req->conversation_id()))
                metrics::g_degraded_es_write_total << 1;
```

改为：

```cpp
            std::string ob_payload = outbox_payload_delete_(req->conversation_id());
            retry_es_write_(ob_payload, [&]() {
                return _es_conv->remove(req->conversation_id());
            });
```

- [ ] **Step 4: 改造 QuitConversation（lines 285-287）**

将：

```cpp
            if (!updated_uids.empty() &&
                !_es_conv->update_member_ids(req->conversation_id(), updated_uids))
                metrics::g_degraded_es_write_total << 1;
```

改为：

```cpp
            if (!updated_uids.empty()) {
                std::string ob_payload = outbox_payload_upd_members_(
                    req->conversation_id(), updated_uids);
                retry_es_write_(ob_payload, [&]() {
                    return _es_conv->update_member_ids(req->conversation_id(), updated_uids);
                });
            }
```

- [ ] **Step 5: 改造 AddMembers（lines 349-351）**

将：

```cpp
            if (!updated_uids.empty() &&
                !_es_conv->update_member_ids(req->conversation_id(), updated_uids))
                metrics::g_degraded_es_write_total << 1;
```

改为：

```cpp
            if (!updated_uids.empty()) {
                std::string ob_payload = outbox_payload_upd_members_(
                    req->conversation_id(), updated_uids);
                retry_es_write_(ob_payload, [&]() {
                    return _es_conv->update_member_ids(req->conversation_id(), updated_uids);
                });
            }
```

- [ ] **Step 6: 改造 RemoveMembers（lines 384-386）**

将：

```cpp
                if (!updated_uids.empty() &&
                    !_es_conv->update_member_ids(req->conversation_id(), updated_uids))
                    metrics::g_degraded_es_write_total << 1;
```

改为：

```cpp
                if (!updated_uids.empty()) {
                    std::string ob_payload = outbox_payload_upd_members_(
                        req->conversation_id(), updated_uids);
                    retry_es_write_(ob_payload, [&]() {
                        return _es_conv->update_member_ids(req->conversation_id(), updated_uids);
                    });
                }
```

- [ ] **Step 7: 提交**

```bash
git add conversation/source/conversation_server.h
git commit -m "feat(conversation): add retry+outbox to all 6 ES write sites"
```

---

### Task 5: Conversation 服务 — Builder 接入 + Reaper 线程

**Files:**
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 在 ConversationServer 类中加入 Reaper 管理**

修改 `ConversationServer` 类（lines 902-923）：

```cpp
class ConversationServer
{
public:
    using ptr = std::shared_ptr<ConversationServer>;
    ConversationServer(const Discovery::ptr &service_discover,
                       const Registry::ptr &reg_client,
                       const std::shared_ptr<odb::core::database> &mysql_client,
                       const std::shared_ptr<brpc::Server> &server,
                       const ESOutbox::ptr &es_outbox,
                       const std::shared_ptr<ESConversation> &es_conv_reaper)
        : _service_discover(service_discover),
          _reg_client(reg_client),
          _mysql_client(mysql_client),
          _rpc_server(server),
          _es_outbox(es_outbox),
          _es_conv_reaper(es_conv_reaper) {}

    ~ConversationServer() {
        if (_es_reaper_running) *_es_reaper_running = false;
        if (_es_reaper_thread && _es_reaper_thread->joinable())
            _es_reaper_thread->join();
    }
    void start() { _rpc_server->RunUntilAskedToQuit(); }

    void start_es_outbox_reaper() {
        _es_reaper_running = std::make_shared<std::atomic<bool>>(true);
        _es_reaper_thread = std::make_shared<std::thread>([this]() {
            while (*_es_reaper_running) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                try {
                    auto items = _es_outbox->peek(50);
                    for (const auto &item : items) {
                        if (_es_conv_reaper->replay_es_write_(item))
                            _es_outbox->remove(item);
                    }
                } catch (std::exception &e) {
                    LOG_WARN("ESOutbox reaper 异常: {}", e.what());
                }
            }
        });
    }

private:
    Discovery::ptr _service_discover;
    Registry::ptr  _reg_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<brpc::Server>        _rpc_server;
    ESOutbox::ptr                        _es_outbox;
    std::shared_ptr<ESConversation>      _es_conv_reaper;
    std::shared_ptr<std::atomic<bool>>   _es_reaper_running;
    std::shared_ptr<std::thread>         _es_reaper_thread;
};
```

注意：`replay_es_write_` 是 `ConversationServiceImpl` 的方法，但 Reaper 在 `ConversationServer` 中。需要调整架构——把 `replay_es_write_` 变为 `ESConversation` 的方法，或者把 Reaper 放到 `ConversationServiceImpl` 中。

**更简单的方案：** 把 Reaper 放到 `ConversationServiceImpl` 中，因为 `replay_es_write_` 需要访问 ES 方法。ConversationServer 只负责生命周期。

修改 `ConversationServiceImpl`：

在 `public:` 区新增：

```cpp
    void start_es_outbox_reaper() {
        _es_reaper_running = std::make_shared<std::atomic<bool>>(true);
        _es_reaper_thread = std::make_shared<std::thread>([this]() {
            while (*_es_reaper_running) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                try {
                    auto items = _es_outbox->peek(50);
                    for (const auto &item : items) {
                        if (replay_es_write_(item))
                            _es_outbox->remove(item);
                    }
                } catch (std::exception &e) {
                    LOG_WARN("ESOutbox reaper 异常: {}", e.what());
                }
            }
        });
    }

    void stop_es_outbox_reaper() {
        if (_es_reaper_running) *_es_reaper_running = false;
        if (_es_reaper_thread && _es_reaper_thread->joinable())
            _es_reaper_thread->join();
    }
```

在 `private:` 区新增：

```cpp
    std::shared_ptr<std::atomic<bool>>   _es_reaper_running;
    std::shared_ptr<std::thread>         _es_reaper_thread;
```

`ConversationServer` 保持简洁，在析构时调 `stop_es_outbox_reaper()`：

```cpp
class ConversationServer
{
public:
    using ptr = std::shared_ptr<ConversationServer>;
    ConversationServer(const Discovery::ptr &service_discover,
                       const Registry::ptr &reg_client,
                       const std::shared_ptr<odb::core::database> &mysql_client,
                       const std::shared_ptr<brpc::Server> &server,
                       ConversationServiceImpl *impl)
        : _service_discover(service_discover),
          _reg_client(reg_client),
          _mysql_client(mysql_client),
          _rpc_server(server),
          _impl(impl) {}

    ~ConversationServer() {
        if (_impl) _impl->stop_es_outbox_reaper();
    }
    void start() {
        if (_impl) _impl->start_es_outbox_reaper();
        _rpc_server->RunUntilAskedToQuit();
    }

private:
    Discovery::ptr _service_discover;
    Registry::ptr  _reg_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<brpc::Server>        _rpc_server;
    ConversationServiceImpl*             _impl = nullptr;
};
```

- [ ] **Step 2: 修改 ConversationServerBuilder**

在 `make_redis_object` 末尾新增 Outbox 创建：

```cpp
    void make_redis_object(const std::string &host, uint16_t port, int db,
                           bool keep_alive, int pool_size) {
        // ... 现有代码 ...
        _members_cache = std::make_shared<Members>(_redis_client);
        _last_msg_cache = std::make_shared<LastMessage>(_redis_client);
        _es_outbox = std::make_shared<ESOutbox>(_redis_client, "im:es:outbox:conversation");
    }
```

在 `make_rpc_object` 中，传入 outbox 并为 Reaper 创建独立 ES Client：

```cpp
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        // ... 现有检查 ...
        if(!_es_outbox)   { LOG_ERROR("还未初始化ESOutbox");    abort(); }

        // Reaper 用独立 ES client
        auto es_reaper_client = ESClientFactory::create(_es_hosts);

        auto *impl = new ConversationServiceImpl(
            _es_client, _mysql_client, _members_cache, _last_msg_cache, _mm_channels,
            _identity_service_name, _media_service_name, _message_service_name, _cfg,
            _es_outbox, es_reaper_client);
        // ... 其余不变 ...
    }
```

注意：需要在 Builder 中保存 `_es_hosts`。修改 `make_es_object`：

```cpp
    void make_es_object(const std::vector<std::string> host_list) {
        _es_client = ESClientFactory::create(host_list);
        _es_hosts = host_list;
    }
```

修改 `build()` — 传入 impl 指针：

```cpp
    ConversationServer::ptr build() {
        // ... 现有检查 ...
        return std::make_shared<ConversationServer>(_service_discover, _reg_client,
                                                    _mysql_client, _rpc_server, _service_impl);
    }
```

修改 `make_rpc_object` — 保存 impl 指针：

```cpp
        auto *impl = new ConversationServiceImpl(...);
        _service_impl = impl;
        // ... AddService ...
```

在 Builder `private:` 区新增成员：

```cpp
    ESOutbox::ptr                           _es_outbox;
    std::vector<std::string>                _es_hosts;
    ConversationServiceImpl*                _service_impl = nullptr;
```

- [ ] **Step 3: 提交**

```bash
git add conversation/source/conversation_server.h
git commit -m "feat(conversation): add ESOutbox reaper thread and builder integration"
```

---

### Task 6: Identity 服务 — 重试逻辑 + Outbox + 改造写入点

**Files:**
- Modify: `identity/source/identity_server.h`

- [ ] **Step 1: 新增 include**

在 `#include "dao/data_redis.hpp"` 之后新增：

```cpp
#include "infra/metrics.hpp"
#include <thread>
#include <chrono>
```

- [ ] **Step 2: 修改 IdentityServiceImpl 构造函数和成员**

构造函数签名新增 `es_outbox` 和 `es_reaper_client` 参数：

```cpp
    IdentityServiceImpl(const std::shared_ptr<odb::core::database> &mysql_client,
                        const std::shared_ptr<elasticlient::Client> &es_client,
                        const RedisClient::ptr &redis_client,
                        const std::shared_ptr<MailClient> &mail_client,
                        const std::shared_ptr<auth::JwtCodec> &jwt_codec,
                        const std::shared_ptr<auth::JwtStore> &jwt_store,
                        const std::string &media_public_url_prefix,
                        const ESOutbox::ptr &es_outbox,
                        const std::shared_ptr<elasticlient::Client> &es_reaper_client)
        : _mysql_user(std::make_shared<UserTable>(mysql_client)),
          _es_user(std::make_shared<ESUser>(es_client)),
          _redis_codes(std::make_shared<Codes>(redis_client)),
          _mail_client(mail_client),
          _jwt_codec(jwt_codec),
          _jwt_store(jwt_store),
          _media_public_url_prefix(media_public_url_prefix),
          _es_outbox(es_outbox),
          _es_user_reaper(std::make_shared<ESUser>(es_reaper_client))
    {
        _es_user->create_index();
    }
```

- [ ] **Step 3: 新增 `retry_es_write_` 辅助方法**

在 `private:` 区新增：

```cpp
    bool retry_es_write_(const std::string &outbox_payload,
                         std::function<bool()> es_op)
    {
        for (int i = 0; i < 3; ++i) {
            if (es_op()) return true;
            if (i < 2) {
                metrics::g_es_retry_total << 1;
                std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << i)));
            }
        }
        if (_es_outbox) {
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            _es_outbox->enqueue(outbox_payload, static_cast<long long>(now_ms));
        }
        metrics::g_degraded_es_write_total << 1;
        return false;
    }
```

- [ ] **Step 4: 新增 Outbox payload 序列化方法**

```cpp
    static std::string outbox_payload_upsert_(const std::string &uid,
                                               const std::string &mail,
                                               const std::string &phone,
                                               const std::string &nickname,
                                               const std::string &description,
                                               const std::string &avatar_id,
                                               int status) {
        Json::Value root;
        root["op"] = "upsert";
        root["uid"] = uid;
        root["mail"] = mail;
        root["phone"] = phone;
        root["nick"] = nickname;
        root["desc"] = description;
        root["avatar"] = avatar_id;
        root["status"] = status;
        std::string dst;
        Serialize(root, dst);
        return dst;
    }
```

- [ ] **Step 5: 新增 `replay_es_write_` 方法**

```cpp
    bool replay_es_write_(const std::string &payload) {
        Json::Value root;
        if (!UnSerialize(payload, root)) return false;
        std::string op = root.get("op", "").asString();
        if (op == "upsert") {
            return _es_user_reaper->append_data(
                root.get("uid", "").asString(),
                root.get("mail", "").asString(),
                root.get("phone", "").asString(),
                root.get("nick", "").asString(),
                root.get("desc", "").asString(),
                root.get("avatar", "").asString(),
                root.get("status", 0).asInt());
        }
        return false;
    }
```

- [ ] **Step 6: 改造 Register handler（line 275）**

将：

```cpp
                _es_user->append_data(user_id, "", phone, nickname, "", "");
```

改为：

```cpp
                std::string ob_payload = outbox_payload_upsert_(
                    user_id, "", phone, nickname, "", "", 0);
                retry_es_write_(ob_payload, [&]() {
                    return _es_user->append_data(user_id, "", phone, nickname, "", "");
                });
```

- [ ] **Step 7: 改造 UpdateProfile handler（lines 421-423）**

将：

```cpp
            _es_user->append_data(user->user_id(), user->mail(), user->phone(),
                                  user->nickname(), user->description(),
                                  user->avatar_id());
```

改为：

```cpp
            std::string ob_payload = outbox_payload_upsert_(
                user->user_id(), user->mail(), user->phone(),
                user->nickname(), user->description(),
                user->avatar_id(), 0);
            retry_es_write_(ob_payload, [&]() {
                return _es_user->append_data(user->user_id(), user->mail(), user->phone(),
                                             user->nickname(), user->description(),
                                             user->avatar_id());
            });
```

- [ ] **Step 8: 新增 Reaper 管理方法（public 区）**

```cpp
    void start_es_outbox_reaper() {
        _es_reaper_running = std::make_shared<std::atomic<bool>>(true);
        _es_reaper_thread = std::make_shared<std::thread>([this]() {
            while (*_es_reaper_running) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                try {
                    auto items = _es_outbox->peek(50);
                    for (const auto &item : items) {
                        if (replay_es_write_(item))
                            _es_outbox->remove(item);
                    }
                } catch (std::exception &e) {
                    LOG_WARN("ESOutbox reaper 异常: {}", e.what());
                }
            }
        });
    }

    void stop_es_outbox_reaper() {
        if (_es_reaper_running) *_es_reaper_running = false;
        if (_es_reaper_thread && _es_reaper_thread->joinable())
            _es_reaper_thread->join();
    }
```

- [ ] **Step 9: 新增成员变量（private 区）**

```cpp
    ESOutbox::ptr                          _es_outbox;
    std::shared_ptr<ESUser>                _es_user_reaper;
    std::shared_ptr<std::atomic<bool>>     _es_reaper_running;
    std::shared_ptr<std::thread>           _es_reaper_thread;
```

- [ ] **Step 10: 提交**

```bash
git add identity/source/identity_server.h
git commit -m "feat(identity): add ES retry+outbox and reaper for Register/UpdateProfile"
```

---

### Task 7: Identity 服务 — Builder 接入 + Reaper 启动

**Files:**
- Modify: `identity/source/identity_server.h`

- [ ] **Step 1: 修改 IdentityServer 启动 Reaper**

```cpp
class IdentityServer
{
public:
    using ptr = std::shared_ptr<IdentityServer>;

    IdentityServer(const Discovery::ptr &service_discover,
            const Registry::ptr &reg_client,
            const std::shared_ptr<elasticlient::Client> &es_client,
            const std::shared_ptr<odb::core::database> &mysql_client,
            const RedisClient::ptr &redis_client,
            const std::shared_ptr<brpc::Server> &server,
            IdentityServiceImpl *impl)
        : _service_discover(service_discover),
        _reg_client(reg_client),
        _es_client(es_client),
        _mysql_client(mysql_client),
        _redis_client(redis_client),
        _rpc_server(server),
        _impl(impl) {}

    ~IdentityServer() {
        if (_impl) _impl->stop_es_outbox_reaper();
    }
    void start() {
        if (_impl) _impl->start_es_outbox_reaper();
        _rpc_server->RunUntilAskedToQuit();
    }
private:
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    RedisClient::ptr _redis_client;
    IdentityServiceImpl* _impl = nullptr;
};
```

- [ ] **Step 2: 修改 IdentityServerBuilder**

`make_redis_object` 末尾新增 Outbox：

```cpp
    void make_redis_object(...) {
        // ... 现有代码 ...
        _es_outbox = std::make_shared<ESOutbox>(_redis_client, "im:es:outbox:identity");
    }
```

`make_es_object` 保存 host_list：

```cpp
    void make_es_object(const std::vector<std::string> host_list) {
        _es_client = ESClientFactory::create(host_list);
        _es_hosts = host_list;
    }
```

`make_rpc_object` 传入 outbox + reaper client：

```cpp
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        // ... 现有检查 ...
        if(!_es_outbox) { LOG_ERROR("还未初始化ESOutbox"); abort(); }

        auto es_reaper_client = ESClientFactory::create(_es_hosts);

        IdentityServiceImpl *identity_service = new IdentityServiceImpl(
            _mysql_client, _es_client, _redis_client, _mail_client,
            _jwt_codec, _jwt_store, _media_public_url_prefix,
            _es_outbox, es_reaper_client);
        _service_impl = identity_service;
        // ... AddService ...
    }
```

`build()` 传入 impl：

```cpp
    IdentityServer::ptr build() {
        // ... 现有检查 ...
        IdentityServer::ptr server = std::make_shared<IdentityServer>(
            _service_discover, _reg_client, _es_client, _mysql_client, _redis_client,
            _rpc_server, _service_impl);
        return server;
    }
```

Builder `private:` 区新增：

```cpp
    ESOutbox::ptr                           _es_outbox;
    std::vector<std::string>                _es_hosts;
    IdentityServiceImpl*                    _service_impl = nullptr;
```

- [ ] **Step 3: 提交**

```bash
git add identity/source/identity_server.h
git commit -m "feat(identity): add ESOutbox reaper and builder integration"
```

---

### Task 8: 编译验证

- [ ] **Step 1: 编译**

```bash
cd build && cmake .. && make -j$(sysctl -n hw.logicalcpu)
```

确认 0 错误 0 警告。

- [ ] **Step 2: 确认 Message 服务编译不受影响**

检查 Message 服务的 ESOutbox 用法（`ESOutbox(const RedisClient::ptr &c)`）仍然编译通过。

- [ ] **Step 3: 提交（如有编译修复）**

```bash
git add -u
git commit -m "fix: compile fixes for ES retry+outbox"
```

---

## 验证检查点

全部任务完成后：

```bash
# 1. 编译检查
cd build && cmake .. && make -j$(sysctl -n hw.logicalcpu)

# 2. 确认无新增编译警告
```

---

## 任务依赖

```
Task1 (ESOutbox key) ──┐
Task2 (metrics)        ├── Task3 (Conversation 重试) ── Task4 (改造写入点) ── Task5 (Reaper)
                       │
                       ├── Task6 (Identity 重试+写入) ── Task7 (Identity Builder+Reaper)
                       │
                       └── Task8 (编译验证) —— 最后执行
```

Task3/4/5 有严格顺序依赖。Task6/7 有严格顺序依赖。两组之间可并行。
