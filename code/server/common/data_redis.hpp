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
} // namespace key

/* brief: 默认 TTL 常量 */
inline constexpr std::chrono::seconds kSessionTtl(24 * 3600 * 7);   // 登录态 7 天
inline constexpr std::chrono::seconds kStatusTtl(60 * 5);           // 在线态 5 分钟（依赖心跳续期）
inline constexpr std::chrono::seconds kCodeTtl(60 * 5);             // 验证码 5 分钟
inline constexpr std::chrono::seconds kLastMsgTtl(24 * 3600);       // 最近消息预览 24 小时
inline constexpr std::chrono::seconds kReadAckTtl(24 * 3600);       // 已读暂存 24 小时


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

} // namespace chatnow
