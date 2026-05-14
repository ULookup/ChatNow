#pragma once

/**
 * ===========================================================================
 * Redis 封装
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. 工厂支持连接池配置（pool_size / wait_timeout / connection_timeout）
 *   2. 用域命名空间隔离 key（KeyPrefix），避免不同业务键冲突
 *   3. 给 IM 关键路径补齐能力：
 *      - SeqGen      会话级 / 用户级单调递增 seq（取代 DB AUTO_INCREMENT 热点）
 *      - LastMessage 最近一条消息预览缓存
 *      - DeviceSet   用户在线设备集合（推送时一次拿到全部 token）
 *      - ReadAck     大群已读暂存（落库前的批量缓冲）
 *   4. Session/Status/Codes 全部带 TTL 保护，避免 OOM
 *   5. 所有 set 操作统一走 try/catch，错误打日志而非抛到调用栈顶
 * ===========================================================================
 */

#include <sw/redis++/redis++.h>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include "logger.hpp"

namespace chatnow
{

namespace key
{
    inline constexpr const char* kSession    = "im:sess:";          // session_id -> user_id
    inline constexpr const char* kStatus     = "im:status:";        // user_id    -> 1
    inline constexpr const char* kVerifyCode = "im:code:";          // code_id    -> 验证码
    inline constexpr const char* kSeqSession = "im:seq:ssid:";      // ssid       -> 会话级 seq
    inline constexpr const char* kSeqUser    = "im:seq:uid:";       // uid        -> 用户级 seq
    inline constexpr const char* kLastMsg    = "im:last:";          // ssid       -> 最后一条消息预览(JSON)
    inline constexpr const char* kDeviceSet  = "im:dev:";           // uid        -> SET<device_id>
    inline constexpr const char* kReadAck    = "im:read:";          // mid        -> SET<uid>
    inline constexpr const char* kMembers    = "im:members:";       // ssid       -> SET<user_id>
    inline constexpr const char* kRateUser   = "im:rl:user:";       // uid        -> 令牌桶
    inline constexpr const char* kRateSsid   = "im:rl:ssid:";       // ssid       -> 令牌桶
    inline constexpr const char* kOnline     = "im:online:";        // uid        -> SET<push_instance_id>
    inline constexpr const char* kPushRoute  = "im:push:route:";    // uid        -> push_instance_id (单设备)
    inline constexpr const char* kUnacked    = "im:unack:";         // uid        -> Sorted Set<msg_id, ts>
    inline constexpr const char* kPushOutbox     = "im:push:outbox";       // 全局 Sorted Set<serialized_payload, ts> 投递失败兜底
    inline constexpr const char* kPushOutboxLock = "im:push:outbox:lock";  // M3 reaper 单实例租约 key
} // namespace key

/* brief: 默认 TTL 常量 */
inline constexpr std::chrono::seconds kSessionTtl(24 * 3600 * 7);   // 登录态 7 天
inline constexpr std::chrono::seconds kStatusTtl(60 * 5);           // 在线态 5 分钟（依赖心跳续期）
inline constexpr std::chrono::seconds kCodeTtl(60 * 5);             // 验证码 5 分钟
inline constexpr std::chrono::seconds kLastMsgTtl(24 * 3600);       // 最近消息预览 24 小时
inline constexpr std::chrono::seconds kReadAckTtl(24 * 3600);       // 已读暂存 24 小时
inline constexpr std::chrono::seconds kMembersTtl(30 * 60);         // 成员缓存 30 分钟
inline constexpr std::chrono::seconds kOnlineTtl(60);               // 在线路由 60s（依赖心跳续期）
inline constexpr std::chrono::seconds kUnackedTtl(7 * 24 * 3600);   // 未 ack 重传缓冲 7 天


/* brief: Redis 工厂（带连接池） */
class RedisClientFactory
{
public:
    static std::shared_ptr<sw::redis::Redis> create(const std::string &host,
                                                    uint16_t port,
                                                    int db,
                                                    bool keep_alive,
                                                    int pool_size = 8)
    {
        sw::redis::ConnectionOptions copts;
        copts.host = host;
        copts.port = port;
        copts.db = db;
        copts.keep_alive = keep_alive;
        copts.connect_timeout = std::chrono::milliseconds(2000);
        copts.socket_timeout  = std::chrono::milliseconds(2000);

        sw::redis::ConnectionPoolOptions popts;
        popts.size              = pool_size;
        popts.wait_timeout      = std::chrono::milliseconds(500);
        popts.connection_lifetime = std::chrono::minutes(30);

        return std::make_shared<sw::redis::Redis>(copts, popts);
    }
};

// =============================================================================
// 登录态 / 在线态 / 验证码（沿用旧 API，但补齐 TTL）
// =============================================================================

class Session
{
public:
    using ptr = std::shared_ptr<Session>;
    Session(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /* brief: 写入登录态，TTL 7 天 */
    void append(const std::string &ssid, const std::string &uid,
                std::chrono::seconds ttl = kSessionTtl) {
        try { _c->set(key::kSession + ssid, uid, ttl); }
        catch(std::exception &e) { LOG_ERROR("Session.append 失败 {}: {}", ssid, e.what()); }
    }
    void remove(const std::string &ssid) {
        try { _c->del(key::kSession + ssid); }
        catch(std::exception &e) { LOG_ERROR("Session.remove 失败 {}: {}", ssid, e.what()); }
    }
    sw::redis::OptionalString uid(const std::string &ssid) {
        try { return _c->get(key::kSession + ssid); }
        catch(std::exception &e) { LOG_ERROR("Session.uid 失败 {}: {}", ssid, e.what()); return {}; }
    }
    /* brief: 续期（每次心跳调用） */
    void touch(const std::string &ssid, std::chrono::seconds ttl = kSessionTtl) {
        try { _c->expire(key::kSession + ssid, ttl); }
        catch(std::exception &e) { LOG_ERROR("Session.touch 失败 {}: {}", ssid, e.what()); }
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

class Status
{
public:
    using ptr = std::shared_ptr<Status>;
    Status(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}
    void append(const std::string &uid, std::chrono::seconds ttl = kStatusTtl) {
        try { _c->set(key::kStatus + uid, "1", ttl); }
        catch(std::exception &e) { LOG_ERROR("Status.append 失败 {}: {}", uid, e.what()); }
    }
    void remove(const std::string &uid) {
        try { _c->del(key::kStatus + uid); }
        catch(std::exception &e) { LOG_ERROR("Status.remove 失败 {}: {}", uid, e.what()); }
    }
    bool exists(const std::string &uid) {
        try { return _c->get(key::kStatus + uid).has_value(); }
        catch(std::exception &e) { LOG_ERROR("Status.exists 失败 {}: {}", uid, e.what()); return false; }
    }
    /* brief: 心跳续期 */
    void touch(const std::string &uid, std::chrono::seconds ttl = kStatusTtl) {
        try { _c->expire(key::kStatus + uid, ttl); }
        catch(std::exception &e) { LOG_ERROR("Status.touch 失败 {}: {}", uid, e.what()); }
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

class Codes
{
public:
    using ptr = std::shared_ptr<Codes>;
    Codes(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}
    void append(const std::string &cid, const std::string &code,
                std::chrono::seconds ttl = kCodeTtl) {
        try { _c->set(key::kVerifyCode + cid, code, ttl); }
        catch(std::exception &e) { LOG_ERROR("Codes.append 失败 {}: {}", cid, e.what()); }
    }
    void remove(const std::string &cid) {
        try { _c->del(key::kVerifyCode + cid); }
        catch(std::exception &e) { LOG_ERROR("Codes.remove 失败 {}: {}", cid, e.what()); }
    }
    sw::redis::OptionalString code(const std::string &cid) {
        try { return _c->get(key::kVerifyCode + cid); }
        catch(std::exception &e) { LOG_ERROR("Codes.code 失败 {}: {}", cid, e.what()); return {}; }
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

// =============================================================================
// IM 核心：分布式 seq 生成器
// =============================================================================

/**
 * SeqGen
 * ------------------------------------------------------------------
 * 取代 DB AUTO_INCREMENT 在分库分表场景的全局热点：
 *   - next_session_seq(ssid)  会话级单调递增；message.seq_id 来源
 *   - next_user_seq(uid)      用户级单调递增；user_timeline.user_seq 来源
 *
 * Redis INCR 是原子的，性能 ~10 万/s/分片；可按 ssid 哈希到不同 Redis 实例
 * 实现水平扩展。
 *
 * Redis 数据丢失保护：
 *   - 每次申请时 max(curr, db_max_seq+1) 兜底，启动时由消息服务从
 *     message 主表 SELECT MAX(seq_id) 回填一次（应用层保证）
 * ------------------------------------------------------------------
 */
class SeqGen
{
public:
    using ptr = std::shared_ptr<SeqGen>;
    SeqGen(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /* brief: 申请一个会话级 seq；失败返回 0（业务侧需视为 fatal） */
    unsigned long next_session_seq(const std::string &ssid) {
        try { return static_cast<unsigned long>(_c->incr(key::kSeqSession + ssid)); }
        catch(std::exception &e) {
            LOG_ERROR("SeqGen.next_session_seq 失败 {}: {}", ssid, e.what());
            return 0;
        }
    }
    /* brief: 申请一个用户级 seq */
    unsigned long next_user_seq(const std::string &uid) {
        try { return static_cast<unsigned long>(_c->incr(key::kSeqUser + uid)); }
        catch(std::exception &e) {
            LOG_ERROR("SeqGen.next_user_seq 失败 {}: {}", uid, e.what());
            return 0;
        }
    }
    /* brief: 批量申请用户级 seq（pipeline 一次往返）
     *  - 写扩散群对每个成员各申请一个 user_seq，N 大时构成 N 次 RTT
     *  - pipeline 把 N 次 INCR 合并为一次往返（仍是 N 次原子操作）
     *  - 任一失败返回空 vector，上层视为 fatal
     */
    std::vector<unsigned long> next_user_seq_batch(const std::vector<std::string> &uids) {
        std::vector<unsigned long> res;
        if(uids.empty()) return res;
        try {
            auto pipe = _c->pipeline();
            for(const auto &uid : uids) {
                pipe.incr(key::kSeqUser + uid);
            }
            auto reply = pipe.exec();
            res.reserve(uids.size());
            for(size_t i = 0; i < uids.size(); ++i) {
                res.push_back(static_cast<unsigned long>(reply.get<long long>(i)));
            }
        } catch(std::exception &e) {
            LOG_ERROR("SeqGen.next_user_seq_batch 失败 size={}: {}", uids.size(), e.what());
            return {};
        }
        return res;
    }
    /* brief: 启动回填 / Redis 数据丢失修复用：把当前 seq 拉到至少 base */
    void backfill_session(const std::string &ssid, unsigned long base) {
        try {
            // SET key base NX 之后 INCR；保证不回退
            auto cur = _c->get(key::kSeqSession + ssid);
            unsigned long cur_v = cur ? std::stoul(*cur) : 0;
            if(cur_v < base) _c->set(key::kSeqSession + ssid, std::to_string(base));
        } catch(std::exception &e) {
            LOG_ERROR("SeqGen.backfill_session 失败 {}: {}", ssid, e.what());
        }
    }
    void backfill_user(const std::string &uid, unsigned long base) {
        try {
            auto cur = _c->get(key::kSeqUser + uid);
            unsigned long cur_v = cur ? std::stoul(*cur) : 0;
            if(cur_v < base) _c->set(key::kSeqUser + uid, std::to_string(base));
        } catch(std::exception &e) {
            LOG_ERROR("SeqGen.backfill_user 失败 {}: {}", uid, e.what());
        }
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

// =============================================================================
// 最近一条消息预览缓存（替代 chat_session.last_message_* 行级写热点）
// =============================================================================

class LastMessage
{
public:
    using ptr = std::shared_ptr<LastMessage>;
    LastMessage(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /* brief: 写最后一条消息预览（已序列化 JSON 字符串）；TTL 24h */
    void set(const std::string &ssid, const std::string &preview_json,
             std::chrono::seconds ttl = kLastMsgTtl) {
        try { _c->set(key::kLastMsg + ssid, preview_json, ttl); }
        catch(std::exception &e) { LOG_ERROR("LastMessage.set 失败 {}: {}", ssid, e.what()); }
    }
    sw::redis::OptionalString get(const std::string &ssid) {
        try { return _c->get(key::kLastMsg + ssid); }
        catch(std::exception &e) { LOG_ERROR("LastMessage.get 失败 {}: {}", ssid, e.what()); return {}; }
    }
    void remove(const std::string &ssid) {
        try { _c->del(key::kLastMsg + ssid); }
        catch(std::exception &e) { LOG_ERROR("LastMessage.del 失败 {}: {}", ssid, e.what()); }
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

// =============================================================================
// 用户在线设备集合（推送下发入口）
// =============================================================================

class DeviceSet
{
public:
    using ptr = std::shared_ptr<DeviceSet>;
    DeviceSet(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /* brief: 用户某设备上线 */
    void add(const std::string &uid, const std::string &device_id) {
        try { _c->sadd(key::kDeviceSet + uid, device_id); }
        catch(std::exception &e) { LOG_ERROR("DeviceSet.add 失败 {}-{}: {}", uid, device_id, e.what()); }
    }
    /* brief: 用户某设备下线 */
    void remove(const std::string &uid, const std::string &device_id) {
        try { _c->srem(key::kDeviceSet + uid, device_id); }
        catch(std::exception &e) { LOG_ERROR("DeviceSet.rem 失败 {}-{}: {}", uid, device_id, e.what()); }
    }
    /* brief: 取用户当前所有在线设备 */
    std::vector<std::string> list(const std::string &uid) {
        std::vector<std::string> res;
        try { _c->smembers(key::kDeviceSet + uid, std::inserter(res, res.end())); }
        catch(std::exception &e) { LOG_ERROR("DeviceSet.list 失败 {}: {}", uid, e.what()); }
        return res;
    }
    /* brief: 用户是否有任意在线设备 */
    bool any(const std::string &uid) {
        try { return _c->scard(key::kDeviceSet + uid) > 0; }
        catch(std::exception &e) { LOG_ERROR("DeviceSet.any 失败 {}: {}", uid, e.what()); return false; }
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

// =============================================================================
// 群消息已读暂存（异步落库前的高 QPS 缓冲）
// =============================================================================

class ReadAck
{
public:
    using ptr = std::shared_ptr<ReadAck>;
    ReadAck(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /* brief: 用户对消息已读，幂等添加到 SET */
    void ack(unsigned long message_id, const std::string &uid,
             std::chrono::seconds ttl = kReadAckTtl) {
        try {
            std::string k = key::kReadAck + std::to_string(message_id);
            _c->sadd(k, uid);
            _c->expire(k, ttl);
        } catch(std::exception &e) {
            LOG_ERROR("ReadAck.ack 失败 mid={} uid={}: {}", message_id, uid, e.what());
        }
    }
    /* brief: 已读人数（角标"已读 X 人"用） */
    long count(unsigned long message_id) {
        try { return _c->scard(key::kReadAck + std::to_string(message_id)); }
        catch(std::exception &e) { LOG_ERROR("ReadAck.count 失败 {}: {}", message_id, e.what()); return 0; }
    }
    /* brief: 后台批量刷库后调用，移交所有权后清空 SET 防止重复刷库 */
    std::vector<std::string> drain(unsigned long message_id) {
        std::vector<std::string> res;
        try {
            std::string k = key::kReadAck + std::to_string(message_id);
            _c->smembers(k, std::inserter(res, res.end()));
            _c->del(k);
        } catch(std::exception &e) {
            LOG_ERROR("ReadAck.drain 失败 {}: {}", message_id, e.what());
        }
        return res;
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

// =============================================================================
// 群成员列表缓存（替代每条消息一次 ChatSession.GetMemberIdList RPC）
// =============================================================================

class Members
{
public:
    using ptr = std::shared_ptr<Members>;
    Members(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /* brief: 取群成员列表；空返回 → 调用方回查 RPC + warm() */
    std::vector<std::string> list(const std::string &ssid) {
        std::vector<std::string> res;
        try { _c->smembers(key::kMembers + ssid, std::inserter(res, res.end())); }
        catch(std::exception &e) { LOG_ERROR("Members.list 失败 {}: {}", ssid, e.what()); }
        return res;
    }
    /* brief: 缓存预热 / 重建 */
    void warm(const std::string &ssid, const std::vector<std::string> &uids,
              std::chrono::seconds ttl = kMembersTtl) {
        if(uids.empty()) return;
        try {
            std::string k = key::kMembers + ssid;
            _c->sadd(k, uids.begin(), uids.end());
            _c->expire(k, ttl);
        } catch(std::exception &e) {
            LOG_ERROR("Members.warm 失败 {}: {}", ssid, e.what());
        }
    }
    /* brief: 单成员加入/退出（增量维护） */
    void add(const std::string &ssid, const std::string &uid) {
        try { _c->sadd(key::kMembers + ssid, uid); }
        catch(std::exception &e) { LOG_ERROR("Members.add 失败 {}-{}: {}", ssid, uid, e.what()); }
    }
    void remove(const std::string &ssid, const std::string &uid) {
        try { _c->srem(key::kMembers + ssid, uid); }
        catch(std::exception &e) { LOG_ERROR("Members.remove 失败 {}-{}: {}", ssid, uid, e.what()); }
    }
    /* brief: 整组失效（解散群 / DDL 变更） */
    void invalidate(const std::string &ssid) {
        try { _c->del(key::kMembers + ssid); }
        catch(std::exception &e) { LOG_ERROR("Members.invalidate 失败 {}: {}", ssid, e.what()); }
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

// =============================================================================
// 在线路由表（多 Push 实例下：uid -> 持有 ws 连接的 push_instance_id 集合）
// =============================================================================

class OnlineRoute
{
public:
    using ptr = std::shared_ptr<OnlineRoute>;
    OnlineRoute(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /* brief: 用户在某 Push 实例上线 */
    void bind(const std::string &uid, const std::string &push_instance,
              std::chrono::seconds ttl = kOnlineTtl) {
        try {
            std::string k = key::kOnline + uid;
            _c->sadd(k, push_instance);
            _c->expire(k, ttl);
        } catch(std::exception &e) {
            LOG_ERROR("OnlineRoute.bind 失败 {}-{}: {}", uid, push_instance, e.what());
        }
    }
    /* brief: 心跳续期（每 N 秒由 push 实例对所有持连用户调用） */
    void touch(const std::string &uid, std::chrono::seconds ttl = kOnlineTtl) {
        try { _c->expire(key::kOnline + uid, ttl); }
        catch(std::exception &e) { LOG_ERROR("OnlineRoute.touch 失败 {}: {}", uid, e.what()); }
    }
    /* brief: 用户在某实例下线 */
    void unbind(const std::string &uid, const std::string &push_instance) {
        try { _c->srem(key::kOnline + uid, push_instance); }
        catch(std::exception &e) { LOG_ERROR("OnlineRoute.unbind 失败 {}-{}: {}", uid, push_instance, e.what()); }
    }
    /* brief: 取用户当前在哪些 Push 实例上有活跃连接 */
    std::vector<std::string> instances(const std::string &uid) {
        std::vector<std::string> res;
        try { _c->smembers(key::kOnline + uid, std::inserter(res, res.end())); }
        catch(std::exception &e) { LOG_ERROR("OnlineRoute.instances 失败 {}: {}", uid, e.what()); }
        return res;
    }
    /* brief: 是否有任意在线设备 */
    bool online(const std::string &uid) {
        try { return _c->scard(key::kOnline + uid) > 0; }
        catch(std::exception &e) { LOG_ERROR("OnlineRoute.online 失败 {}: {}", uid, e.what()); return false; }
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

// =============================================================================
// 令牌桶限流（基于 INCR + EXPIRE 简易实现；要求 Redis 7+ 推荐用 redis-cell）
// =============================================================================

class RateLimiter
{
public:
    using ptr = std::shared_ptr<RateLimiter>;
    RateLimiter(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /**
     * brief: 滑动窗口 incr-and-check
     *   - window_sec 内最多允许 max_count 次操作
     *   - 命中限制返回 false（业务可返回 429 / RATE_LIMITED）
     */
    bool allow(const std::string &key_full, int max_count, int window_sec) {
        try {
            long long cur = _c->incr(key_full);
            if(cur == 1) {
                _c->expire(key_full, std::chrono::seconds(window_sec));
            }
            return cur <= max_count;
        } catch(std::exception &e) {
            LOG_ERROR("RateLimiter.allow {}: {}", key_full, e.what());
            // 限流器失败时默认放行，避免雪崩
            return true;
        }
    }
    bool allow_user(const std::string &uid, int max_count, int window_sec) {
        return allow(key::kRateUser + uid, max_count, window_sec);
    }
    bool allow_session(const std::string &ssid, int max_count, int window_sec) {
        return allow(key::kRateSsid + ssid, max_count, window_sec);
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

// =============================================================================
// 推送投递 outbox 兜底（message → push_queue 投递失败时持久化，由后台 reaper 重投）
// =============================================================================

class PushOutbox
{
public:
    using ptr = std::shared_ptr<PushOutbox>;
    PushOutbox(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /* brief: 投递失败入队（payload 是 InternalMessage 序列化后的 binary） */
    void enqueue(const std::string &payload, long long score_ts) {
        try { _c->zadd(key::kPushOutbox, payload, static_cast<double>(score_ts)); }
        catch(std::exception &e) { LOG_ERROR("PushOutbox.enqueue 失败: {}", e.what()); }
    }
    /* brief: reaper 取一批待重投（按时间升序）；返回的项调用方在投递成功后 remove */
    std::vector<std::string> peek(long limit = 50) {
        std::vector<std::string> res;
        try {
            _c->zrange(key::kPushOutbox, 0, limit - 1, std::back_inserter(res));
        } catch(std::exception &e) { LOG_ERROR("PushOutbox.peek 失败: {}", e.what()); }
        return res;
    }
    void remove(const std::string &payload) {
        try { _c->zrem(key::kPushOutbox, payload); }
        catch(std::exception &e) { LOG_ERROR("PushOutbox.remove 失败: {}", e.what()); }
    }

    /* brief: M3 reaper 单实例租约 — SET NX EX 上锁；已持有则原子续约。
     *        必须 Lua 原子，否则 acquire 的 GET+EXPIRE 与 release 的 GET+DEL 都有 TOCTOU race，
     *        race 下两个实例可能同时认为自己持锁，导致 outbox 双发。
     *        返回是否拿到 / 续到锁。
     */
    bool try_acquire_reaper_lease(const std::string &owner, int ttl_sec) {
        static const char *kAcquireLua =
            "if redis.call('SET', KEYS[1], ARGV[1], 'NX', 'EX', ARGV[2]) then return 1 end "
            "if redis.call('GET', KEYS[1]) == ARGV[1] then "
            "    redis.call('EXPIRE', KEYS[1], ARGV[2]); return 1 "
            "end "
            "return 0";
        try {
            std::vector<std::string> keys = {key::kPushOutboxLock};
            std::vector<std::string> args = {owner, std::to_string(ttl_sec)};
            auto ret = _c->eval<long long>(kAcquireLua, keys.begin(), keys.end(),
                                           args.begin(), args.end());
            return ret == 1;
        } catch(std::exception &e) {
            LOG_ERROR("PushOutbox.try_acquire_reaper_lease 失败: {}", e.what());
            return false;
        }
    }

    /* brief: M3 reaper 主动释放租约（CAS：仅 owner 与自己一致时 DEL，原子） */
    void release_reaper_lease(const std::string &owner) {
        static const char *kReleaseLua =
            "if redis.call('GET', KEYS[1]) == ARGV[1] then "
            "    return redis.call('DEL', KEYS[1]) "
            "end "
            "return 0";
        try {
            std::vector<std::string> keys = {key::kPushOutboxLock};
            std::vector<std::string> args = {owner};
            _c->eval<long long>(kReleaseLua, keys.begin(), keys.end(),
                                args.begin(), args.end());
        } catch(std::exception &e) {
            LOG_ERROR("PushOutbox.release_reaper_lease 失败: {}", e.what());
        }
    }
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

// =============================================================================
// 推送未 ACK 缓冲（B5 用：超时未 ack 的消息进 Sorted Set，按时间戳重传）
// =============================================================================

class UnackedPush
{
public:
    using ptr = std::shared_ptr<UnackedPush>;
    UnackedPush(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    /* brief: 入待重传队列（score = 服务端时间戳秒级） */
    void push(const std::string &uid, unsigned long user_seq,
              long long score_ts, std::chrono::seconds ttl = kUnackedTtl) {
        try {
            std::string k = key::kUnacked + uid;
            _c->zadd(k, std::to_string(user_seq), static_cast<double>(score_ts));
            _c->expire(k, ttl);
        } catch(std::exception &e) {
            LOG_ERROR("UnackedPush.push 失败 {}: {}", uid, e.what());
        }
    }
    /* brief: 客户端 ACK 后移除 */
    void ack(const std::string &uid, unsigned long user_seq) {
        try { _c->zrem(key::kUnacked + uid, std::to_string(user_seq)); }
        catch(std::exception &e) { LOG_ERROR("UnackedPush.ack 失败 {}: {}", uid, e.what()); }
    }
    /* brief: 取所有"成熟可重传"的 user_seq（按时间升序，仅查询不修改）
     *  - max_age_sec：仅返回入队时间 ≤ now - max_age_sec 的项（避免立即重传刚入队的）
     *  - limit：最多返回 limit 条
     */
    std::vector<std::string> peek_due(const std::string &uid,
                                      long limit = 100,
                                      long max_age_sec = 5) {
        std::vector<std::string> res;
        if(limit <= 0) return res;
        try {
            long long now = static_cast<long long>(time(nullptr));
            using namespace sw::redis;
            _c->zrangebyscore(key::kUnacked + uid,
                              BoundedInterval<double>(0, static_cast<double>(now - max_age_sec),
                                                       BoundType::CLOSED),
                              LimitOptions{0, limit},
                              std::back_inserter(res));
        } catch(std::exception &e) {
            LOG_ERROR("UnackedPush.peek_due 失败 {}: {}", uid, e.what());
        }
        return res;
    }

    /* brief: 触发重发后把这批 user_seq 的 score 重置为 now（推迟下次重传时机）
     *  - 用 ZADD XX 仅当存在时才更新；若客户端已 ack，zrem 已经删除，本调用 no-op
     *  - 配合 peek_due 使用：peek_due → 实际重发 → bump_score 推迟同批的下次重发
     */
    void bump_score(const std::string &uid,
                    const std::vector<std::string> &user_seqs,
                    std::chrono::seconds ttl = kUnackedTtl) {
        if(user_seqs.empty()) return;
        try {
            std::string k = key::kUnacked + uid;
            long long now = static_cast<long long>(time(nullptr));
            using namespace sw::redis;
            std::vector<std::pair<std::string, double>> items;
            items.reserve(user_seqs.size());
            for(const auto &s : user_seqs) items.emplace_back(s, static_cast<double>(now));
            _c->zadd(k, items.begin(), items.end(), UpdateType::EXIST);
            // 续期 key TTL，避免长期未 ack 项随整 key 7 天到期消失
            _c->expire(k, ttl);
        } catch(std::exception &e) {
            LOG_ERROR("UnackedPush.bump_score 失败 {}: {}", uid, e.what());
        }
    }

private:
    std::shared_ptr<sw::redis::Redis> _c;
};

} // namespace chatnow
