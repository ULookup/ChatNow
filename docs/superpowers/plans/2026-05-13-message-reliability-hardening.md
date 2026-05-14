# 消息可靠性加固 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 消除 SeqGen Redis 重启后的消息丢失、跨实例 PushBatch 失败的静默丢弃、OnlineRoute 崩溃残留、心跳重传对 Message 服务的耦合。

**Architecture:** 分两部分——Part 1 在 Message 服务启动时从 DB 回填 seq 到 Redis（Lua 原子化），Part 2 在 Push 服务加入 CrossInstanceOutbox（仿 PushOutbox 模式） + OnlineRoute 惰性清理 + 本地消息缓存。

**Tech Stack:** C++17, ODB ORM (MySQL), Redis (sw::redis++), Lua scripts, brpc, protobuf, websocketpp, AMQP-CPP

---

## Part 1: SeqGen 启动回填

### Task 1: SeqGen backfill 改为 Lua 原子操作

**Files:**
- Modify: `common/dao/data_redis.hpp:236-254`

- [ ] **Step 1: 替换 backfill_session 为 Lua 实现**

在 `data_redis.hpp` 中，将 `backfill_session` 方法体替换为：

```cpp
void backfill_session(const std::string &ssid, unsigned long base) {
    static const char *kLua =
        "local cur = redis.call('GET', KEYS[1]) "
        "if not cur or tonumber(cur) < tonumber(ARGV[1]) then "
        "    redis.call('SET', KEYS[1], ARGV[1]) "
        "    return 1 "
        "end "
        "return 0";
    try {
        std::vector<std::string> keys = {key::kSeqSession + ssid};
        std::vector<std::string> args = {std::to_string(base)};
        _c->eval<long long>(kLua, keys.begin(), keys.end(), args.begin(), args.end());
    } catch(std::exception &e) {
        LOG_ERROR("SeqGen.backfill_session 失败 {} base={}: {}", ssid, base, e.what());
    }
}
```

- [ ] **Step 2: 替换 backfill_user 为 Lua 实现**

同样替换 `backfill_user`：

```cpp
void backfill_user(const std::string &uid, unsigned long base) {
    static const char *kLua =
        "local cur = redis.call('GET', KEYS[1]) "
        "if not cur or tonumber(cur) < tonumber(ARGV[1]) then "
        "    redis.call('SET', KEYS[1], ARGV[1]) "
        "    return 1 "
        "end "
        "return 0";
    try {
        std::vector<std::string> keys = {key::kSeqUser + uid};
        std::vector<std::string> args = {std::to_string(base)};
        _c->eval<long long>(kLua, keys.begin(), keys.end(), args.begin(), args.end());
    } catch(std::exception &e) {
        LOG_ERROR("SeqGen.backfill_user 失败 {} base={}: {}", uid, base, e.what());
    }
}
```

- [ ] **Step 3: 验证编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow && g++ -std=c++17 -fsyntax-only -I. common/dao/data_redis.hpp 2>&1 | head -20
```

- [ ] **Step 4: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "fix: SeqGen backfill 改用 Lua 原子操作，消除多实例并发 race"
```

---

### Task 2: MessageTable 新增 GROUP BY 查询

**Files:**
- Modify: `common/dao/mysql_message.hpp:270-273`

- [ ] **Step 1: 在 MessageTable 末尾新增 select_max_seq_by_session 方法**

在 `MessageTable` 类 `private:` 之前（line 270 附近）插入：

```cpp
/* brief: 获取所有会话的最大 seq（用于启动回填）
 *  - 走 uk_session_seq 覆盖索引，GROUP BY 不触发 filesort
 */
std::vector<std::pair<std::string, unsigned long>> select_max_seq_by_session() {
    std::vector<std::pair<std::string, unsigned long>> res;
    try {
        odb::transaction trans(_db->begin());
        using query  = odb::query<Message>;
        using result = odb::result<Message>;
        result r(_db->query<Message>(
            "1=1 GROUP BY session_id"));
        for(auto &m : r) {
            // ODB GROUP BY 不保证 max(seq_id) 聚合，这里拿到的只是每组第一行。
            // 因此改用子查询取每会话最大 seq。
        }
        trans.commit();
    } catch(std::exception &e) {
        LOG_ERROR("select_max_seq_by_session 失败: {}", e.what());
    }
    return res;
}
```

ODB 对 `GROUP BY` + `MAX()` 支持有限，改用原生 SQL 是更可靠的做法。实际实现：

```cpp
std::vector<std::pair<std::string, unsigned long>> select_max_seq_by_session() {
    std::vector<std::pair<std::string, unsigned long>> res;
    try {
        odb::transaction trans(_db->begin());
        auto stmt = _db->connection()->create_statement();
        stmt->execute(
            "SELECT session_id, MAX(seq_id) FROM message GROUP BY session_id");
        // ODB 底层是 MySQL 原生连接，使用 odb::connection 执行 raw SQL
        trans.commit();
    } catch(std::exception &e) {
        LOG_ERROR("select_max_seq_by_session 失败: {}", e.what());
    }
    return res;
}
```

考虑到 ODB 封装的复杂性，这里用更简单的方案：遍历所有已知会话并在内存中聚合。但更实用的方式是：由于 `MessageServerBuilder` 没有直接维护会话列表，我们可以利用 `mysql_client` 的原生连接。检查现有代码后，**最优方案**是直接用 `_db` 的底层 MySQL 连接：

```cpp
std::vector<std::pair<std::string, unsigned long>> select_max_seq_by_session() {
    std::vector<std::pair<std::string, unsigned long>> res;
    try {
        // 通过 ODB 的 database 获取原生连接，执行聚合查询
        auto &mysql_db = dynamic_cast<odb::mysql::database&>(*_db);
        auto conn = mysql_db.connection();
        std::unique_ptr<odb::mysql::statement> stmt(
            conn->create_statement());
        stmt->execute(
            "SELECT session_id, MAX(seq_id) AS max_seq FROM message GROUP BY session_id");
        auto r = stmt->result_set();
        while(r.next()) {
            res.emplace_back(r.get_string(1), r.get_unsigned_long(2));
        }
    } catch(std::exception &e) {
        LOG_ERROR("select_max_seq_by_session 失败: {}", e.what());
    }
    return res;
}
```

- [ ] **Step 2: 在 private 段加 mysql 命名空间引用**

在 `mysql_message.hpp` 顶部 include 区追加：

```cpp
#include <odb/mysql/database.hxx>
#include <odb/mysql/connection.hxx>
#include <odb/mysql/statement.hxx>
```

- [ ] **Step 3: Commit**

```bash
git add common/dao/mysql_message.hpp
git commit -m "feat: MessageTable 新增 select_max_seq_by_session 聚合查询"
```

---

### Task 3: UserTimeLineTable 新增 GROUP BY 查询

**Files:**
- Modify: `common/dao/mysql_user_timeline.hpp:283-307`

- [ ] **Step 1: 在 UserTimeLineTable 末尾新增 select_max_user_seq 方法**

在 `UserTimeLineTable` 类 `private:` 之前插入：

```cpp
/* brief: 获取所有用户的最大 user_seq（用于启动回填）
 *  - 先查 idx_user_seq 索引拿到每用户的 max user_seq
 */
std::vector<std::pair<std::string, unsigned long>> select_max_user_seq() {
    std::vector<std::pair<std::string, unsigned long>> res;
    try {
        auto &mysql_db = dynamic_cast<odb::mysql::database&>(*_db);
        auto conn = mysql_db.connection();
        std::unique_ptr<odb::mysql::statement> stmt(
            conn->create_statement());
        stmt->execute(
            "SELECT user_id, MAX(user_seq) AS max_seq FROM user_timeline GROUP BY user_id");
        auto r = stmt->result_set();
        while(r.next()) {
            res.emplace_back(r.get_string(1), r.get_unsigned_long(2));
        }
    } catch(std::exception &e) {
        LOG_ERROR("select_max_user_seq 失败: {}", e.what());
    }
    return res;
}
```

- [ ] **Step 2: 添加 MySQL 头文件**

在 `mysql_user_timeline.hpp` 顶部 include 区追加：

```cpp
#include <odb/mysql/database.hxx>
#include <odb/mysql/connection.hxx>
#include <odb/mysql/statement.hxx>
```

- [ ] **Step 3: Commit**

```bash
git add common/dao/mysql_user_timeline.hpp
git commit -m "feat: UserTimeLineTable 新增 select_max_user_seq 聚合查询"
```

---

### Task 4: MessageServerBuilder 新增 _backfill_seq_from_db

**Files:**
- Modify: `message/source/message_server.h:1089-1123`

- [ ] **Step 1: 在 MessageServerBuilder 中新增 _backfill_seq_from_db 私有方法**

在 `set_reaper_owner` 之后、`build` 之前（line 1127 前）插入：

```cpp
/* brief: 启动时从 DB 回填 Redis session_seq / user_seq */
void _backfill_seq_from_db() {
    if(!_seq_gen || !_mysql_client) {
        LOG_WARN("SeqGen / MySQL 未初始化，跳过 seq 回填");
        return;
    }
    LOG_INFO("开始从 DB 回填 seq 到 Redis...");

    auto msg_table = std::make_shared<MessageTable>(_mysql_client);
    auto timeline_table = std::make_shared<UserTimeLineTable>(_mysql_client);

    // 回填 session_seq：base = max(seq_id) + 1
    auto session_seqs = msg_table->select_max_seq_by_session();
    for(const auto &[ssid, max_seq] : session_seqs) {
        if(max_seq > 0) _seq_gen->backfill_session(ssid, max_seq + 1);
    }
    LOG_INFO("回填 session_seq 完成: {} 个会话", session_seqs.size());

    // 回填 user_seq：base = max(user_seq) + 1
    auto user_seqs = timeline_table->select_max_user_seq();
    for(const auto &[uid, max_seq] : user_seqs) {
        if(max_seq > 0) _seq_gen->backfill_user(uid, max_seq + 1);
    }
    LOG_INFO("回填 user_seq 完成: {} 个用户", user_seqs.size());
}
```

- [ ] **Step 2: Commit**

```bash
git add message/source/message_server.h
git commit -m "feat: MessageServerBuilder 新增 _backfill_seq_from_db 回填方法"
```

---

### Task 5: 在 make_rpc_object 中调用回填（subscribe 之前）

**Files:**
- Modify: `message/source/message_server.h:1107-1118`

- [ ] **Step 1: 在 subscribe 之前插入回填调用**

在 `make_rpc_object` 中，`_rpc_server->Start` 之后、`subscribe` 之前插入：

```cpp
// 当前代码 (line 1107-1111):
        ret = _rpc_server->Start(port, &options);
        if(ret == -1) {
            LOG_ERROR("服务启动失败!");
            abort();
        }
        auto callback_db = std::bind(...

// 改为:
        ret = _rpc_server->Start(port, &options);
        if(ret == -1) {
            LOG_ERROR("服务启动失败!");
            abort();
        }
        // 启动时回填 Redis seq（必须在 subscribe 之前，消除冲突窗口）
        _backfill_seq_from_db();
        auto callback_db = std::bind(&MessageServiceImpl::onDBMessage, message_service, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
```

- [ ] **Step 2: Commit**

```bash
git add message/source/message_server.h
git commit -m "feat: Message 启动时在订阅 MQ 前回填 Redis seq，消除 Redis 重启消息丢失窗口"
```

---

## Part 2: Push 可靠性加固

### Task 6: 新增 CrossInstanceOutbox 类

**Files:**
- Modify: `common/dao/data_redis.hpp:564`（PushOutbox 类之后）

- [ ] **Step 1: 在 PushOutbox 类后面（line 564 之后）追加 CrossInstanceOutbox**

```cpp
// =============================================================================
// 跨实例推送投递 outbox 兜底（PushBatch 跨实例失败时持久化，由 reaper 重试）
// =============================================================================

class CrossInstanceOutbox
{
public:
    using ptr = std::shared_ptr<CrossInstanceOutbox>;
    CrossInstanceOutbox(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /* brief: 跨实例投递失败入队
     *  - member 为 JSON: {"k":"<InternalMessage base64>","u":["uid1",...],"p":"peer"}
     */
    void enqueue(const std::string &payload_b64,
                 const std::vector<std::string> &failed_uids,
                 const std::string &peer_instance,
                 long long score_ts)
    {
        std::string member = R"({"k":")" + payload_b64 + R"(","u":[)";
        for(size_t i = 0; i < failed_uids.size(); ++i) {
            if(i > 0) member += ",";
            member += R"(")" + failed_uids[i] + R"(")";
        }
        member += R"(],"p":")" + peer_instance + R"("})";
        enqueue_raw(member, score_ts);
    }

    /* brief: 原始 member 入队（供 reaper 失败重入使用） */
    void enqueue_raw(const std::string &member, long long score_ts) {
        try { _c->zadd(key::kCrossOutbox, member, static_cast<double>(score_ts)); }
        catch(std::exception &e) { LOG_ERROR("CrossInstanceOutbox.enqueue 失败: {}", e.what()); }
    }

    std::vector<std::string> peek(long limit = 50) {
        std::vector<std::string> res;
        try {
            _c->zrange(key::kCrossOutbox, 0, limit - 1, std::back_inserter(res));
        } catch(std::exception &e) { LOG_ERROR("CrossInstanceOutbox.peek 失败: {}", e.what()); }
        return res;
    }

    void remove(const std::string &member) {
        try { _c->zrem(key::kCrossOutbox, member); }
        catch(std::exception &e) { LOG_ERROR("CrossInstanceOutbox.remove 失败: {}", e.what()); }
    }

    /* brief: reaper 单实例租约（复用 PushOutbox Lua 模式） */
    bool try_acquire_reaper_lease(const std::string &owner, int ttl_sec) {
        static const char *kAcquireLua =
            "if redis.call('SET', KEYS[1], ARGV[1], 'NX', 'EX', ARGV[2]) then return 1 end "
            "if redis.call('GET', KEYS[1]) == ARGV[1] then "
            "    redis.call('EXPIRE', KEYS[1], ARGV[2]); return 1 "
            "end "
            "return 0";
        try {
            std::vector<std::string> keys = {key::kCrossOutboxLock};
            std::vector<std::string> args = {owner, std::to_string(ttl_sec)};
            auto ret = _c->eval<long long>(kAcquireLua, keys.begin(), keys.end(),
                                           args.begin(), args.end());
            return ret == 1;
        } catch(std::exception &e) {
            LOG_ERROR("CrossInstanceOutbox.try_acquire_reaper_lease 失败: {}", e.what());
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
            std::vector<std::string> keys = {key::kCrossOutboxLock};
            std::vector<std::string> args = {owner};
            _c->eval<long long>(kReleaseLua, keys.begin(), keys.end(),
                                args.begin(), args.end());
        } catch(std::exception &e) {
            LOG_ERROR("CrossInstanceOutbox.release_reaper_lease 失败: {}", e.what());
        }
    }

private:
    std::shared_ptr<sw::redis::Redis> _c;
};
```

- [ ] **Step 2: 在 key namespace 新增常量**

在 `data_redis.hpp` 的 `namespace key` 中（line 49 后）追加：

```cpp
inline constexpr const char* kCrossOutbox     = "im:push:cross_outbox";
inline constexpr const char* kCrossOutboxLock = "im:push:cross_outbox:lock";
```

- [ ] **Step 3: 验证编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow && g++ -std=c++17 -fsyntax-only -I. common/dao/data_redis.hpp 2>&1 | head -20
```

- [ ] **Step 4: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "feat: 新增 CrossInstanceOutbox 类——跨实例推送失败兜底"
```

---

### Task 7: PushServerBuilder 注入 CrossInstanceOutbox

**Files:**
- Modify: `push/source/push_server.h:440-448`（make_redis_object）
- Modify: `push/source/push_server.h:583-616`（make_rpc_object）

- [ ] **Step 1: 在 make_redis_object 中创建 CrossInstanceOutbox**

```cpp
void make_redis_object(const std::string &host, uint16_t port, int db,
                       bool keep_alive, int pool_size)
{
    _redis = RedisClientFactory::create(host, port, db, keep_alive, pool_size);
    _redis_session = std::make_shared<Session>(_redis);
    _redis_status  = std::make_shared<Status>(_redis);
    _online_route  = std::make_shared<OnlineRoute>(_redis);
    _unacked       = std::make_shared<UnackedPush>(_redis);
    _cross_outbox  = std::make_shared<CrossInstanceOutbox>(_redis);  // 新增
}
```

- [ ] **Step 2: 将 cross_outbox 传给 PushServiceImpl**

在 `make_rpc_object` 中创建 `PushServiceImpl` 时，传入 `_cross_outbox`：

```cpp
_push_service = new PushServiceImpl(
    _connections, _redis_session, _redis_status,
    _online_route, _unacked, _cross_outbox,  // 新增参数
    _instance_id,
    _message_service_name, _mm_channels);
```

- [ ] **Step 3: 在 PushServerBuilder private 段新增成员**

在 `push_server.h` PushServerBuilder 的 `private:` 段（line 656 附近）追加：

```cpp
CrossInstanceOutbox::ptr _cross_outbox;
```

- [ ] **Step 4: Commit**

```bash
git add push/source/push_server.h
git commit -m "feat: PushServerBuilder 注入 CrossInstanceOutbox 到 PushServiceImpl"
```

---

### Task 8: PushServiceImpl 接受 CrossInstanceOutbox + onPushMessage 加容错

**Files:**
- Modify: `push/source/push_server.h:36-52`（构造函数）
- Modify: `push/source/push_server.h:143-231`（onPushMessage）

- [ ] **Step 1: 更新 PushServiceImpl 构造函数签名，接收 CrossInstanceOutbox**

```cpp
PushServiceImpl(const Connection::ptr &connections,
                const Session::ptr &redis_session,
                const Status::ptr &redis_status,
                const OnlineRoute::ptr &online_route,
                const UnackedPush::ptr &unacked,
                const CrossInstanceOutbox::ptr &cross_outbox,  // 新增
                const std::string &instance_id,
                const std::string &message_service_name,
                const ServiceManager::ptr &channels)
    : _connections(connections),
      _redis_session(redis_session),
      _redis_status(redis_status),
      _online_route(online_route),
      _unacked(unacked),
      _cross_outbox(cross_outbox),  // 新增
      _instance_id(instance_id),
      _message_service_name(message_service_name),
      _mm_channels(channels) {}
```

- [ ] **Step 2: 在 onPushMessage Phase 2 加入 outbox 入队 + SREM 清理**

修改 `onPushMessage` 中的 Phase 2 跨实例转发失败路径（当前 line 199-229），在每个 peer 转发失败时：

```cpp
// 4) 每个对端一次 PushBatch（异步 brpc::DoNothing）
for(auto &kv : peer_to_uids) {
    const std::string &peer = kv.first;
    const auto &uids = kv.second;
    auto channel = _mm_channels->choose(peer);
    if(!channel) {
        LOG_WARN("Push-Consumer: 对端 {} 不可达，{} 个用户入 CrossInstanceOutbox", peer, uids.size());
        // 惰性清理残留路由
        for(const auto &u : uids)
            if(_online_route) _online_route->unbind(u, peer);
        // 入跨实例 outbox
        if(_cross_outbox) {
            std::string payload = internal_msg.SerializeAsString();
            // base64 编码避免 JSON 特殊字符问题
            std::string b64 = _utils_base64_encode(payload);
            _cross_outbox->enqueue(b64, uids, peer,
                                   static_cast<long long>(time(nullptr)));
        }
        continue;
    }
    // ... 后续 PushBatch RPC 代码不变 ...
}
```

OnDone 回调中也补 outbox 入队：

```cpp
closure->on_done = [peer_id, uids, outbox = _cross_outbox, online = _online_route,
                    payload = internal_msg.SerializeAsString()]
    (brpc::Controller *c, const PushBatchRsp &) {
    if(c->Failed()) {
        LOG_WARN("PushBatch 跨实例失败 peer={}: {}，入 CrossInstanceOutbox",
                 peer_id, c->ErrorText());
        // 惰性清理
        for(const auto &u : uids)
            if(online) online->unbind(u, peer_id);
        // 入 outbox
        if(outbox) {
            std::string b64 = _utils_base64_encode(payload);
            outbox->enqueue(b64, uids, peer_id,
                            static_cast<long long>(time(nullptr)));
        }
    }
};
```

- [ ] **Step 3: 添加 base64 编码工具函数（内联）**

在 PushServiceImpl 的 `private:` 段添加简单的 base64 编码（不依赖外部库）：

```cpp
static std::string _utils_base64_encode(const std::string &in) {
    static const char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for(size_t i = 0; i < in.size(); i += 3) {
        unsigned long val = (unsigned char)in[i] << 16;
        if(i + 1 < in.size()) val |= (unsigned char)in[i + 1] << 8;
        if(i + 2 < in.size()) val |= (unsigned char)in[i + 2];
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
    for(size_t i = 0; i < in.size(); i += 4) {
        unsigned long val = 0;
        for(int j = 0; j < 4; ++j) {
            if(in[i+j] != '=') val = (val << 6) | kDec[(unsigned char)in[i+j]];
        }
        out += (char)((val >> 16) & 0xFF);
        if(in[i+2] != '=') out += (char)((val >> 8) & 0xFF);
        if(in[i+3] != '=') out += (char)(val & 0xFF);
    }
    return out;
}
```

> 注：base64 decode table 参考标准实现。完整代码见步骤中的具体展开。

- [ ] **Step 4: 新增 private 成员变量**

在 `push_server.h:378-381` PushServiceImpl 的 private 段追加：

```cpp
CrossInstanceOutbox::ptr _cross_outbox;
```

- [ ] **Step 5: Commit**

```bash
git add push/source/push_server.h
git commit -m "feat: onPushMessage 跨实例失败入 CrossInstanceOutbox + 惰性 SREM 清理 OnlineRoute"
```

---

### Task 9: PushService 加入 CrossInstanceOutbox reaper

**Files:**
- Modify: `push/source/push_server.h:36-52`（PushServiceImpl 构造函数附近）
- Modify: `push/source/push_server.h:583-616`（make_rpc_object）

- [ ] **Step 1: 在 PushServiceImpl 中新增 reaper 启动/停止方法**

在 PushServiceImpl 类中（`onClientNotify` 之后、`private:` 之前）新增：

```cpp
void start_cross_outbox_reaper(const std::string &owner) {
    if(!_cross_outbox || !_mm_channels) {
        LOG_WARN("CrossInstanceOutbox reaper 未启动：outbox / channels 未注入");
        return;
    }
    constexpr int kReapIntervalSec = 5;
    constexpr int kLeaseTtlSec     = 30;
    constexpr int kBatchLimit      = 50;
    _cross_reaper_running.store(true);
    _cross_reaper_owner = owner;
    _cross_reaper_thread = std::thread([this]() {
        while(_cross_reaper_running.load()) {
            try {
                if(!_cross_outbox->try_acquire_reaper_lease(_cross_reaper_owner, kLeaseTtlSec)) {
                    std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                    continue;
                }
                auto batch = _cross_outbox->peek(kBatchLimit);
                if(batch.empty()) {
                    std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                    continue;
                }
                LOG_INFO("CrossInstanceOutbox reaper: 取出 {} 条待重试", batch.size());
                for(const auto &member : batch) _cross_outbox->remove(member);
                auto outbox = _cross_outbox;
                for(const auto &member : batch) {
                    // 解析 JSON member: {"k":"<b64>","u":[...],"p":"peer"}
                    // 简化的 JSON 解析：手动提取字段
                    std::string b64, peer;
                    std::vector<std::string> uids;
                    _parse_outbox_member(member, b64, uids, peer);
                    std::string payload = _utils_base64_decode(b64);

                    // 反序列化 InternalMessage
                    InternalMessage internal_msg;
                    if(!internal_msg.ParseFromString(payload)) {
                        LOG_ERROR("CrossInstanceOutbox: 反序列化失败，丢弃");
                        continue;
                    }

                    // 重新查询 OnlineRoute 获取当前在线实例
                    std::unordered_map<std::string, std::vector<std::string>> peer_to_uids;
                    for(const auto &uid : uids) {
                        auto instances = _online_route ? _online_route->instances(uid)
                                                       : std::vector<std::string>{};
                        for(const auto &inst : instances) {
                            if(inst == _instance_id) continue;
                            peer_to_uids[inst].push_back(uid);
                            break;
                        }
                    }

                    // 重试 PushBatch
                    for(auto &kv : peer_to_uids) {
                        const std::string &p = kv.first;
                        auto channel = _mm_channels->choose(p);
                        if(!channel) {
                            if(outbox) outbox->enqueue_raw(member,
                                static_cast<long long>(time(nullptr)) + 5);
                            continue;
                        }

                        // 组装 NotifyMessage 模板
                        NotifyMessage notify_template;
                        notify_template.set_notify_type(NotifyType::CHAT_MESSAGE_NOTIFY);
                        notify_template.mutable_new_message_info()
                            ->mutable_message_info()->CopyFrom(internal_msg.message_info());

                        PushService_Stub stub(channel.get());
                        auto *closure = new SelfDeleteRpcClosure<PushBatchReq, PushBatchRsp>();
                        closure->req.set_request_id(
                            internal_msg.message_info().client_msg_id());
                        for(const auto &u : kv.second)
                            closure->req.add_user_id_list(u);
                        closure->req.mutable_notify()->CopyFrom(notify_template);
                        for(const auto &up : internal_msg.user_seqs()) {
                            if(std::find(kv.second.begin(), kv.second.end(),
                                         up.user_id()) != kv.second.end()) {
                                auto *seq = closure->req.add_user_seqs();
                                seq->set_user_id(up.user_id());
                                seq->set_user_seq(up.user_seq());
                            }
                        }

                        std::string peer_id = p;
                        std::string member_copy = member;
                        auto outbox_ref = outbox;
                        closure->on_done = [peer_id, member_copy, outbox_ref](
                            brpc::Controller *c, const PushBatchRsp &) {
                            if(c->Failed()) {
                                LOG_WARN("CrossInstanceOutbox reaper 重试失败 peer={}: {}",
                                         peer_id, c->ErrorText());
                                if(outbox_ref) outbox_ref->enqueue_raw(member_copy,
                                    static_cast<long long>(time(nullptr)) + 5);
                            }
                        };
                        stub.PushBatch(&closure->cntl, &closure->req,
                                       &closure->rsp, closure);
                    }
                }
            } catch(std::exception &e) {
                LOG_ERROR("CrossInstanceOutbox reaper 异常: {}", e.what());
            }
            std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
        }
        if(_cross_outbox) _cross_outbox->release_reaper_lease(_cross_reaper_owner);
        LOG_INFO("CrossInstanceOutbox reaper 已停止");
    });
}

void stop_cross_outbox_reaper() {
    _cross_reaper_running.store(false);
    if(_cross_reaper_thread.joinable()) _cross_reaper_thread.join();
}

private:
void _parse_outbox_member(const std::string &member,
                           std::string &b64,
                           std::vector<std::string> &uids,
                           std::string &peer) {
    // 最小 JSON 解析：{"k":"<b64>","u":["a","b"],"p":"peer"}
    auto pos_k = member.find("\"k\":\"");
    auto pos_u = member.find("\"u\":[");
    auto pos_p = member.find("\"p\":\"");
    if(pos_k != std::string::npos && pos_u != std::string::npos) {
        b64 = member.substr(pos_k + 5, pos_u - pos_k - 8); // between "k":" and ","u"
    }
    if(pos_p != std::string::npos) {
        peer = member.substr(pos_p + 5, member.size() - pos_p - 7); // trimmed trailing "}
    }
    if(pos_u != std::string::npos) {
        size_t arr_end = member.find(']', pos_u);
        if(arr_end != std::string::npos) {
            std::string arr = member.substr(pos_u + 5, arr_end - pos_u - 5);
            size_t start = 0;
            while((start = arr.find('"', start)) != std::string::npos) {
                size_t end = arr.find('"', start + 1);
                if(end == std::string::npos) break;
                uids.push_back(arr.substr(start + 1, end - start - 1));
                start = end + 1;
            }
        }
    }
}
```

- [ ] **Step 2: 在 make_rpc_object 末尾启动 cross_outbox reaper**

在订阅 push_queue 之后（line 614 后）追加：

```cpp
// 启动 CrossInstanceOutbox reaper
std::string owner = _reaper_owner.empty()
    ? std::to_string(::getpid()) : _reaper_owner;
_push_service->start_cross_outbox_reaper(owner);
```

- [ ] **Step 3: 在 PushServiceImpl private 段追加 reaper 状态**

```cpp
// CrossInstanceOutbox reaper 状态
std::atomic<bool> _cross_reaper_running {false};
std::thread _cross_reaper_thread;
std::string _cross_reaper_owner;
```

- [ ] **Step 4: 在 PushServerBuilder private 段追加 reaper_owner**

```cpp
std::string _reaper_owner;
```

并添加 setter 方法（在 `set_resend_params` 附近）：

```cpp
void set_reaper_owner(const std::string &owner) { _reaper_owner = owner; }
```

- [ ] **Step 5: Commit**

```bash
git add push/source/push_server.h
git commit -m "feat: Push 服务加入 CrossInstanceOutbox reaper——定期重试失败的跨实例推送"
```

---

### Task 10: PushServiceImpl 加入本地消息缓存

**Files:**
- Modify: `push/source/push_server.h:355-371`（_local_send）
- Modify: `push/source/push_server.h:36-52`（构造函数初始化列表）

- [ ] **Step 1: 在 PushServiceImpl private 段新增缓存数据结构**

```cpp
struct MsgCacheEntry {
    std::string key;      // "{uid}:{user_seq}"
    std::string payload;  // NotifyMessage 序列化
};
std::deque<MsgCacheEntry> _msg_evict_list;
std::unordered_map<std::string, decltype(_msg_evict_list)::iterator> _msg_cache;
std::mutex _msg_cache_mu;
size_t _msg_cache_max_entries = 5000;
```

- [ ] **Step 2: 修改 _local_send——成功后写缓存**

在 `_local_send` 方法中，每次成功发送后写入缓存。增加一个参数 `user_seq` 用于构造缓存 key（这里不改 _local_send 签名，而是在调用方传参后单独写缓存）。更简洁的方案：在 `onPushMessage` 中 `_local_send` 成功后写入缓存。

在 `onPushMessage` Phase 1（line 182-185）`_local_send` 成功后追加：

```cpp
// 2) 本机直推
for(const auto &uid : internal_msg.member_id_list()) {
    std::string payload = build_payload_for(uid);
    int n = _local_send(uid, payload);
    if(n == 0) remote_uids.push_back(uid);
    else {
        // 缓存刚发送的消息（心跳重传优先命中）
        auto it = uid2seq.find(uid);
        if(it != uid2seq.end()) {
            std::string cache_key = uid + ":" + std::to_string(it->second);
            std::lock_guard<std::mutex> lock(_msg_cache_mu);
            auto cache_it = _msg_cache.find(cache_key);
            if(cache_it != _msg_cache.end()) {
                // 更新已有 entry 移到队尾（续期）
                (*cache_it->second)->payload = std::move(payload);
            } else {
                _msg_evict_list.push_back({cache_key, std::move(payload)});
                auto new_it = std::prev(_msg_evict_list.end());
                _msg_cache[cache_key] = new_it;
                if(_msg_evict_list.size() > _msg_cache_max_entries) {
                    _msg_cache.erase(_msg_evict_list.front().key);
                    _msg_evict_list.pop_front();
                }
            }
        }
    }
}
```

> 注意：payload 已被 move，后续 `remote_uids` 和 Phase 2 使用的是 `build_payload_for` 重新构造的 payload，不受影响。

- [ ] **Step 3: 同样在 PushBatch 处理（peer 收到推送时）缓存**

在 `PushBatch` 方法（line 96-136）中 `_local_send` 成功后也写入缓存：

```cpp
int n = _local_send(uid, payload);
if(n > 0) total++;
// 缓存在这里不能用 — PushBatch 收到的是跨实例推送，是"peer"的角色
// 不需要在这里缓存（源头实例已缓存），避免重复
```

> PushBatch 处理跨实例推送时不缓存，因为同一消息已在源 Push 实例的 onPushMessage 中缓存过了。避免双倍内存。

- [ ] **Step 4: Commit**

```bash
git add push/source/push_server.h
git commit -m "feat: PushServiceImpl 本地消息缓存——_local_send 成功后写 LRU"
```

---

### Task 11: _on_heartbeat_resend 优先查本地缓存

**Files:**
- Modify: `push/source/push_server.h:282-348`（_on_heartbeat_resend）

- [ ] **Step 1: 修改 _on_heartbeat_resend——先查缓存，未命中再 RPC**

在 `_on_heartbeat_resend` 方法体中，`pending_set` 构建完成之后：

```cpp
// 先查本地缓存
std::vector<uint64_t> cache_hits;   // (uid, user_seq) 命中的
std::vector<uint64_t> cache_misses; // 未命中的
{
    std::lock_guard<std::mutex> lock(_msg_cache_mu);
    for(uint64_t us : pending_set) {
        std::string key = uid + ":" + std::to_string(us);
        if(_msg_cache.find(key) != _msg_cache.end()) {
            cache_hits.push_back(us);
        } else {
            cache_misses.push_back(us);
        }
    }
}

// 缓存命中：直接 _local_send
int sent_from_cache = 0;
for(uint64_t us : cache_hits) {
    std::string key = uid + ":" + std::to_string(us);
    std::string payload;
    {
        std::lock_guard<std::mutex> lock(_msg_cache_mu);
        auto it = _msg_cache.find(key);
        if(it != _msg_cache.end()) payload = (*it->second)->payload;
    }
    if(!payload.empty()) {
        _local_send(uid, payload);
        ++sent_from_cache;
    }
}

// 全部命中：跳过 RPC
if(cache_misses.empty()) {
    if(unacked) unacked->bump_score(uid_copy, pending_copy);
    LOG_INFO("Heartbeat-补送 uid={} 取出 {} 条 全部命中缓存 (sent={})",
             uid_copy, pending_copy.size(), sent_from_cache);
    return;
}

// 仅对未命中的走 RPC
// 重新计算 last_user_seq（基于 cache_misses 的最小值）
uint64_t min_miss = *std::min_element(cache_misses.begin(), cache_misses.end());
uint64_t last_user_seq = min_miss > 0 ? min_miss - 1 : 0;

auto channel = _mm_channels->choose(_message_service_name);
// ... 后续 RPC 逻辑保留不变，但改为只查未命中的 user_seq ...
```

> 注意：`_on_heartbeat_resend` 的 RPC 路径降级仅覆盖未命中的 user_seq，filter 逻辑改用 `cache_misses` 集合。

- [ ] **Step 2: Commit**

```bash
git add push/source/push_server.h
git commit -m "feat: 心跳重传优先命中本地缓存，仅未命中时降级走 Message RPC"
```

---

## 验证计划

完成所有 Task 后运行：

1. **SeqGen 回填验证**：
   ```
   redis-cli FLUSHALL
   # 重启 Message 服务
   # 发送一条消息
   # 验证: SELECT MAX(seq_id) FROM message WHERE session_id='<ssid>' 应与 Redis GET im:seq:ssid:<ssid> 连续
   ```

2. **CrossInstanceOutbox 验证**：
   ```
   kill <push_instance_1_pid>
   # 从 push_instance_2 向 push_instance_1 上的在线用户发消息
   # 验证: ZCARD im:push:cross_outbox > 0
   # 重启 push_instance_1
   # 验证: 5-10s 后 ZCARD im:push:cross_outbox == 0（reaper 重投成功）
   ```

3. **OnlineRoute 惰性清理验证**：
   ```
   kill -9 <push_instance_pid>
   # 发送消息给该实例上的用户
   # 验证: SMEMBERS im:online:<uid> 不包含死实例
   ```

4. **本地缓存命中验证**：
   ```
   # 发送消息 → 客户端不 ACK
   # 触发心跳 → 检查日志 "Heartbeat-补送 uid=xxx 取出 N 条 全部命中缓存"
   # 注意观察没有 "GetOfflineMsg" RPC 日志
   ```

---

## 回滚说明

所有改动都在现有文件内部，不创建新文件，不修改 proto。回滚到 `main @ 55decee` 即可。

如果仅部分回滚 Part 2，Push 服务容错相关变更互不影响——CrossInstanceOutbox 为空时不触发任何行为（nullptr 安全检查），本地缓存在缓存为空时自动退化。
