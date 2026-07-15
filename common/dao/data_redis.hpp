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
#include <sw/redis++/redis_cluster.h>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "infra/logger.hpp"
#include "infra/metrics.hpp"
#include "utils/cache_version.hpp"
#include "utils/random_ttl.hpp"
#include "utils/redis_circuit_breaker.hpp"
#include "utils/redis_keys.hpp"

namespace chatnow
{

inline bool is_redis_pool_wait_error(const sw::redis::Error &error) noexcept {
    if (typeid(error) != typeid(sw::redis::Error)) return false;
    constexpr std::string_view prefix = "Failed to fetch a connection in ";
    constexpr std::string_view suffix = " milliseconds";
    const std::string_view message(error.what());
    if (message.size() <= prefix.size() + suffix.size() ||
        message.compare(0, prefix.size(), prefix) != 0 ||
        message.compare(message.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    const auto milliseconds = message.substr(
        prefix.size(), message.size() - prefix.size() - suffix.size());
    for (const char ch : milliseconds) {
        if (ch < '0' || ch > '9') return false;
    }
    return true;
}

class RedisPipeline {
public:
    RedisPipeline(sw::redis::Pipeline pipeline,
                  std::shared_ptr<RedisCircuitBreaker> breaker,
                  RedisCircuitBreaker::Permit permit)
        : _pipeline(std::move(pipeline)), _breaker(std::move(breaker)), _permit(permit) {}

    RedisPipeline(RedisPipeline &&other) noexcept
        : _pipeline(std::move(other._pipeline)),
          _breaker(std::move(other._breaker)),
          _permit(other._permit),
          _settled(other._settled) {
        other._settled = true;
    }
    RedisPipeline &operator=(RedisPipeline &&other) noexcept {
        if (this == &other) return *this;
        abandon_();
        _pipeline = std::move(other._pipeline);
        _breaker = std::move(other._breaker);
        _permit = other._permit;
        _settled = other._settled;
        other._settled = true;
        return *this;
    }
    RedisPipeline(const RedisPipeline &) = delete;
    RedisPipeline &operator=(const RedisPipeline &) = delete;
    ~RedisPipeline() { abandon_(); }

    template <typename... Args>
    RedisPipeline &hset(Args &&...args) {
        return queue_([&] { _pipeline.hset(std::forward<Args>(args)...); });
    }

    template <typename... Args>
    RedisPipeline &set(Args &&...args) {
        return queue_([&] { _pipeline.set(std::forward<Args>(args)...); });
    }

    template <typename... Args>
    RedisPipeline &expire(Args &&...args) {
        return queue_([&] { _pipeline.expire(std::forward<Args>(args)...); });
    }

    template <typename... Args>
    RedisPipeline &get(Args &&...args) {
        return queue_([&] { _pipeline.get(std::forward<Args>(args)...); });
    }

    sw::redis::QueuedReplies exec() {
        if (_settled) {
            throw std::logic_error("RedisPipeline::exec called more than once");
        }
        try {
            auto replies = _pipeline.exec();
            settle_success_();
            return replies;
        } catch (const sw::redis::IoError &) {
            record_connection_failure_();
            throw;
        } catch (const sw::redis::ClosedError &) {
            record_connection_failure_();
            throw;
        } catch (const sw::redis::ReplyError &) {
            settle_success_();
            throw;
        } catch (const sw::redis::Error &error) {
            if (is_redis_pool_wait_error(error)) record_connection_failure_();
            else abandon_();
            throw;
        } catch (...) {
            abandon_();
            throw;
        }
    }

private:
    template <typename F>
    RedisPipeline &queue_(F &&queue_command) {
        if (_settled) {
            throw std::logic_error("RedisPipeline command queued after settlement");
        }
        try {
            std::forward<F>(queue_command)();
            return *this;
        } catch (const sw::redis::IoError &) {
            record_connection_failure_();
            throw;
        } catch (const sw::redis::ClosedError &) {
            record_connection_failure_();
            throw;
        } catch (const sw::redis::Error &error) {
            if (is_redis_pool_wait_error(error)) record_connection_failure_();
            else abandon_();
            throw;
        } catch (...) {
            abandon_();
            throw;
        }
    }

    void record_transition_(RedisCircuitBreaker::Transition transition) noexcept {
        if (transition == RedisCircuitBreaker::Transition::Opened) {
            metrics::g_redis_circuit_open_total << 1;
        } else if (transition == RedisCircuitBreaker::Transition::Recovered) {
            metrics::g_redis_circuit_recovered_total << 1;
        }
    }

    void settle_success_() noexcept {
        if (_settled) return;
        _settled = true;
        record_transition_(_breaker->on_success(_permit));
    }

    void record_connection_failure_() noexcept {
        if (_settled) return;
        _settled = true;
        metrics::g_redis_call_failure_total << 1;
        record_transition_(_breaker->on_connection_failure(_permit));
    }

    void abandon_() noexcept {
        if (_settled || !_breaker) return;
        _settled = true;
        record_transition_(_breaker->on_abandoned(_permit));
    }

    sw::redis::Pipeline _pipeline;
    std::shared_ptr<RedisCircuitBreaker> _breaker;
    RedisCircuitBreaker::Permit _permit;
    bool _settled = false;
};

// 类型擦除 Redis 客户端适配器：根据持有的后端类型透明转发到
// sw::redis::Redis（单机）或 sw::redis::RedisCluster。所有 cache 类
// 统一使用 RedisClient::ptr，对调用方完全透明。每次调用一次分支
// 判断（~1ns）相对 Redis 网络延迟（~0.5ms）可忽略。
class RedisClient
{
public:
    using ptr = std::shared_ptr<RedisClient>;

    RedisClient(std::shared_ptr<sw::redis::Redis> r)
        : _r(std::move(r)), _breaker(std::make_shared<RedisCircuitBreaker>()) {}
    RedisClient(std::shared_ptr<sw::redis::RedisCluster> rc)
        : _rc(std::move(rc)), _breaker(std::make_shared<RedisCircuitBreaker>()) {}

    // --- String commands ---
    sw::redis::OptionalString get(const std::string &key) {
        return guarded_([&] { return _rc ? _rc->get(key) : _r->get(key); });
    }
    bool set(const std::string &key, const std::string &val,
             std::chrono::seconds ttl = std::chrono::seconds(0)) {
        return guarded_([&] { return _rc ? _rc->set(key, val, ttl) : _r->set(key, val, ttl); });
    }
    bool set(const std::string &key, const std::string &val,
             std::chrono::milliseconds ttl) {
        return guarded_([&] { return _rc ? _rc->set(key, val, ttl) : _r->set(key, val, ttl); });
    }
    bool set(const std::string &key, const std::string &val,
             std::chrono::seconds ttl, sw::redis::UpdateType type) {
        return guarded_([&] { return _rc ? _rc->set(key, val, ttl, type) : _r->set(key, val, ttl, type); });
    }
    bool set(const std::string &key, const std::string &val,
             std::chrono::milliseconds ttl, sw::redis::UpdateType type) {
        return guarded_([&] { return _rc ? _rc->set(key, val, ttl, type) : _r->set(key, val, ttl, type); });
    }
    long long del(const std::string &key) {
        return guarded_([&] { return _rc ? _rc->del(key) : _r->del(key); });
    }
    void expire(const std::string &key, std::chrono::seconds ttl) {
        guarded_void_([&] { _rc ? _rc->expire(key, ttl) : _r->expire(key, ttl); });
    }
    long long incr(const std::string &key) {
        return guarded_([&] { return _rc ? _rc->incr(key) : _r->incr(key); });
    }

    // --- Set commands ---
    template <typename T>
    long long sadd(const std::string &key, const T &member) {
        return guarded_([&] { return _rc ? _rc->sadd(key, member) : _r->sadd(key, member); });
    }
    template <typename It>
    long long sadd(const std::string &key, It first, It last) {
        return guarded_([&] { return _rc ? _rc->sadd(key, first, last) : _r->sadd(key, first, last); });
    }
    template <typename Out>
    void smembers(const std::string &key, Out out) {
        guarded_void_([&] { _rc ? _rc->smembers(key, out) : _r->smembers(key, out); });
    }
    template <typename T>
    long long srem(const std::string &key, const T &member) {
        return guarded_([&] { return _rc ? _rc->srem(key, member) : _r->srem(key, member); });
    }
    long long scard(const std::string &key) {
        return guarded_([&] { return _rc ? _rc->scard(key) : _r->scard(key); });
    }

    // --- Hash commands ---
    long long hset(const std::string &key, const std::string &field, const std::string &val) {
        return guarded_([&] { return _rc ? _rc->hset(key, field, val) : _r->hset(key, field, val); });
    }
    sw::redis::OptionalString hget(const std::string &key, const std::string &field) {
        return guarded_([&] { return _rc ? _rc->hget(key, field) : _r->hget(key, field); });
    }
    long long hdel(const std::string &key, const std::string &field) {
        return guarded_([&] { return _rc ? _rc->hdel(key, field) : _r->hdel(key, field); });
    }
    template <typename Out>
    void hkeys(const std::string &key, Out out) {
        guarded_void_([&] { _rc ? _rc->hkeys(key, out) : _r->hkeys(key, out); });
    }
    template <typename Out>
    void hgetall(const std::string &key, Out out) {
        guarded_void_([&] { _rc ? _rc->hgetall(key, out) : _r->hgetall(key, out); });
    }
    long long hlen(const std::string &key) {
        return guarded_([&] { return _rc ? _rc->hlen(key) : _r->hlen(key); });
    }

    // --- Sorted Set commands ---
    long long zadd(const std::string &key, const std::string &member, double score) {
        return guarded_([&] { return _rc ? _rc->zadd(key, member, score) : _r->zadd(key, member, score); });
    }
    long long zadd(const std::string &key, const std::string &member, double score,
                   sw::redis::UpdateType type) {
        return guarded_([&] { return _rc ? _rc->zadd(key, member, score, type) : _r->zadd(key, member, score, type); });
    }
    long long zrem(const std::string &key, const std::string &member) {
        return guarded_([&] { return _rc ? _rc->zrem(key, member) : _r->zrem(key, member); });
    }
    template <typename Out>
    void zrange(const std::string &key, long long start, long long stop, Out out) {
        guarded_void_([&] { _rc ? _rc->zrange(key, start, stop, out) : _r->zrange(key, start, stop, out); });
    }
    template <typename Out>
    void zrangebyscore(const std::string &key,
                       const sw::redis::BoundedInterval<double> &interval,
                       const sw::redis::LimitOptions &opts, Out out) {
        guarded_void_([&] {
            _rc ? _rc->zrangebyscore(key, interval, opts, out)
                : _r->zrangebyscore(key, interval, opts, out);
        });
    }

    // --- Lua scripting ---
    template <typename Ret, typename KeyIt, typename ArgIt>
    Ret eval(const std::string &script, KeyIt key_first, KeyIt key_last,
             ArgIt arg_first, ArgIt arg_last) {
        return guarded_([&]() -> Ret {
            return _rc ? _rc->eval<Ret>(script, key_first, key_last, arg_first, arg_last)
                       : _r->eval<Ret>(script, key_first, key_last, arg_first, arg_last);
        });
    }
    template <typename KeyIt, typename ArgIt, typename Out>
    void eval(const std::string &script, KeyIt key_first, KeyIt key_last,
              ArgIt arg_first, ArgIt arg_last, Out out) {
        guarded_void_([&] {
            _rc ? _rc->eval(script, key_first, key_last, arg_first, arg_last, out)
                : _r->eval(script, key_first, key_last, arg_first, arg_last, out);
        });
    }

    // --- Pipeline ---
    RedisPipeline pipeline(const sw::redis::StringView &hash_tag = {}) {
        auto permit = before_call_();
        try {
            return RedisPipeline(_rc ? _rc->pipeline(hash_tag) : _r->pipeline(),
                                 _breaker, permit);
        } catch (const sw::redis::IoError &) {
            record_connection_failure_(permit);
            throw;
        } catch (const sw::redis::ClosedError &) {
            record_connection_failure_(permit);
            throw;
        } catch (const sw::redis::ReplyError &) {
            record_success_(permit);
            throw;
        } catch (const sw::redis::Error &error) {
            if (is_redis_pool_wait_error(error)) record_connection_failure_(permit);
            else abandon_(permit);
            throw;
        } catch (...) {
            abandon_(permit);
            throw;
        }
    }

    template <typename Input, typename Output>
    void mget(Input first, Input last, Output out) {
        guarded_void_([&] {
            _rc ? _rc->mget(first, last, out) : _r->mget(first, last, out);
        });
    }

    // --- SCAN ---
    // 集群模式：for_each 一次遍历所有节点。集群不支持跨节点 cursor
    // 续扫，因此任意 cursor 都重启一次完整扫描并返回 0。
    template <typename Out>
    long long scan(long long cursor, const std::string &pattern, long long count, Out out) {
        return guarded_([&]() -> long long {
            if (_rc) {
                if (cursor != 0) {
                    LOG_WARN("RedisCluster scan cannot resume cursor {}; restarting full cluster scan", cursor);
                }
                _rc->for_each([&](sw::redis::Redis &r) {
                    long long cur = 0;
                    while (true) {
                        cur = r.scan(cur, pattern, count, out);
                        if (cur == 0) break;
                    }
                });
                return 0;
            }
            return static_cast<long long>(_r->scan(cursor, pattern, count, out));
        });
    }

    bool is_cluster() const { return _rc != nullptr; }

private:
    RedisCircuitBreaker::Permit before_call_() {
        try {
            return _breaker->before_call();
        } catch (const RedisCircuitOpen &) {
            metrics::g_redis_circuit_rejected_total << 1;
            throw;
        }
    }

    static void record_transition_(RedisCircuitBreaker::Transition transition) noexcept {
        if (transition == RedisCircuitBreaker::Transition::Opened) {
            metrics::g_redis_circuit_open_total << 1;
        } else if (transition == RedisCircuitBreaker::Transition::Recovered) {
            metrics::g_redis_circuit_recovered_total << 1;
        }
    }

    void record_success_(RedisCircuitBreaker::Permit permit) noexcept {
        record_transition_(_breaker->on_success(permit));
    }

    void abandon_(RedisCircuitBreaker::Permit permit) noexcept {
        record_transition_(_breaker->on_abandoned(permit));
    }

    void record_connection_failure_(RedisCircuitBreaker::Permit permit) noexcept {
        metrics::g_redis_call_failure_total << 1;
        record_transition_(_breaker->on_connection_failure(permit));
    }

    template <class F>
    auto guarded_(F &&fn) -> std::invoke_result_t<F &&> {
        auto permit = before_call_();
        try {
            decltype(auto) result = std::forward<F>(fn)();
            record_success_(permit);
            return std::forward<decltype(result)>(result);
        } catch (const sw::redis::IoError &) {
            record_connection_failure_(permit);
            throw;
        } catch (const sw::redis::ClosedError &) {
            record_connection_failure_(permit);
            throw;
        } catch (const sw::redis::ReplyError &) {
            record_success_(permit);
            throw;
        } catch (const sw::redis::Error &error) {
            if (is_redis_pool_wait_error(error)) record_connection_failure_(permit);
            else abandon_(permit);
            throw;
        } catch (...) {
            abandon_(permit);
            throw;
        }
    }

    template <class F>
    void guarded_void_(F &&fn) {
        auto permit = before_call_();
        try {
            std::forward<F>(fn)();
            record_success_(permit);
        } catch (const sw::redis::IoError &) {
            record_connection_failure_(permit);
            throw;
        } catch (const sw::redis::ClosedError &) {
            record_connection_failure_(permit);
            throw;
        } catch (const sw::redis::ReplyError &) {
            record_success_(permit);
            throw;
        } catch (const sw::redis::Error &error) {
            if (is_redis_pool_wait_error(error)) record_connection_failure_(permit);
            else abandon_(permit);
            throw;
        } catch (...) {
            abandon_(permit);
            throw;
        }
    }

    std::shared_ptr<sw::redis::Redis> _r;
    std::shared_ptr<sw::redis::RedisCluster> _rc;
    std::shared_ptr<RedisCircuitBreaker> _breaker;
};

/* brief: 默认 TTL 常量 */
inline constexpr std::chrono::seconds kSessionTtl(24 * 3600 * 7);   // 登录态 7 天
inline constexpr std::chrono::seconds kStatusTtl(60 * 5);           // 在线态 5 分钟（依赖心跳续期）
inline constexpr std::chrono::seconds kCodeTtl(60 * 5);             // 验证码 5 分钟
inline constexpr std::chrono::seconds kLastMsgTtl(24 * 3600);       // 最近消息预览 24 小时
inline constexpr std::chrono::seconds kReadAckTtl(24 * 3600);       // 已读暂存 24 小时
inline constexpr std::chrono::seconds kMembersTtl(30 * 60);         // 成员缓存 30 分钟
inline constexpr std::chrono::seconds kOnlineTtl(30);               // 在线路由 30s（依赖心跳续期，每 heartbeat 刷新）
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
        copts.connect_timeout = std::chrono::milliseconds(50);
        copts.socket_timeout  = std::chrono::milliseconds(50);

        sw::redis::ConnectionPoolOptions popts;
        popts.size              = pool_size;
        popts.wait_timeout      = std::chrono::milliseconds(20);
        popts.connection_lifetime = std::chrono::minutes(30);

        return std::make_shared<sw::redis::Redis>(copts, popts);
    }
};

/* brief: Redis Cluster 工厂 — 通过种子节点自动发现集群拓扑 */
class RedisClusterFactory
{
public:
    static std::shared_ptr<sw::redis::RedisCluster> create(
        const std::string &seed_nodes_csv,  // "host1:6379,host2:6379,host3:6379"
        int pool_size = 16,
        bool keep_alive = true)
    {
        // 解析所有种子节点
        std::vector<std::pair<std::string, uint16_t>> seeds;
        {
            std::istringstream ss(seed_nodes_csv);
            std::string token;
            while (std::getline(ss, token, ',')) {
                auto colon = token.find(':');
                if (colon == std::string::npos) continue;
                seeds.emplace_back(
                    token.substr(0, colon),
                    static_cast<uint16_t>(std::stoi(token.substr(colon + 1))));
            }
        }
        if (seeds.empty()) {
            throw std::runtime_error("RedisClusterFactory: 无有效种子节点");
        }

        sw::redis::ConnectionPoolOptions popts;
        popts.size = pool_size;
        popts.wait_timeout = std::chrono::milliseconds(20);
        popts.connection_lifetime = std::chrono::minutes(30);

        // 逐个尝试种子节点，直到成功连接（sw::redis++ RedisCluster 仅需一个种子
        // 即可通过 CLUSTER SLOTS 自动发现完整拓扑）
        std::string last_error;
        for (const auto &[host, port] : seeds) {
            try {
                sw::redis::ConnectionOptions copts;
                copts.host = host;
                copts.port = port;
                copts.keep_alive = keep_alive;
                copts.connect_timeout = std::chrono::milliseconds(50);
                copts.socket_timeout  = std::chrono::milliseconds(50);

                auto cluster = std::make_shared<sw::redis::RedisCluster>(copts, popts);
                // 验证连接可用（立即尝试一个轻量命令）
                cluster->for_each([](sw::redis::Redis &r) { r.ping("cluster-seed-check"); });
                LOG_INFO("RedisClusterFactory: 通过种子 {}:{} 成功连接集群", host, port);
                return cluster;
            } catch (std::exception &e) {
                last_error = e.what();
                LOG_WARN("RedisClusterFactory: 种子 {}:{} 连接失败 ({}), 尝试下一个",
                         host, port, last_error);
            }
        }
        throw std::runtime_error("RedisClusterFactory: 所有种子节点连接失败 — " + last_error);
    }
};

// =============================================================================
// 登录态 / 在线态 / 验证码（沿用旧 API，但补齐 TTL）
// =============================================================================

class Session
{
public:
    using ptr = std::shared_ptr<Session>;
    Session(const RedisClient::ptr &c) : _c(c) {}

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
    RedisClient::ptr _c;
};

class Status
{
public:
    using ptr = std::shared_ptr<Status>;
    Status(const RedisClient::ptr &c) : _c(c) {}
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
    RedisClient::ptr _c;
};

class Codes
{
public:
    using ptr = std::shared_ptr<Codes>;
    Codes(const RedisClient::ptr &c) : _c(c) {}
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
    RedisClient::ptr _c;
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
    SeqGen(const RedisClient::ptr &c) : _c(c) {}

    /* brief: 申请一个会话级 seq；失败返回 0（业务侧需视为 fatal） */
    unsigned long next_session_seq(const std::string &ssid) {
        try { return static_cast<unsigned long>(_c->incr(key::seq_session_key(ssid))); }
        catch(std::exception &e) {
            LOG_ERROR("SeqGen.next_session_seq 失败 {}: {}", ssid, e.what());
            return 0;
        }
    }
    /* brief: 申请一个用户级 seq */
    unsigned long next_user_seq(const std::string &uid) {
        try { return static_cast<unsigned long>(_c->incr(key::seq_user_key(uid))); }
        catch(std::exception &e) {
            LOG_ERROR("SeqGen.next_user_seq 失败 {}: {}", uid, e.what());
            return 0;
        }
    }
    /* brief: 批量申请用户级 seq
     *  - user seq key 按 uid hash tag 分散到 Redis Cluster slots，避免热点
     *  - 为保证 Cluster 正确性逐 key INCR；后续可按 slot 分组 pipeline 优化
     *  - 任一失败返回空 vector，上层视为 fatal
     */
    std::vector<unsigned long> next_user_seq_batch(const std::vector<std::string> &uids) {
        std::vector<unsigned long> res;
        if(uids.empty()) return res;
        try {
            res.reserve(uids.size());
            for(const auto &uid : uids) {
                res.push_back(static_cast<unsigned long>(_c->incr(key::seq_user_key(uid))));
            }
        } catch(std::exception &e) {
            LOG_ERROR("SeqGen.next_user_seq_batch 失败 size={}: {}", uids.size(), e.what());
            return {};
        }
        return res;
    }
    /* brief: 启动回填 / Redis 数据丢失修复用：把当前 seq 拉到至少 base（Lua 原子操作，消除多实例并发 race） */
    void backfill_session(const std::string &ssid, unsigned long base) {
        try {
            std::vector<std::string> keys = {key::seq_session_key(ssid)};
            std::vector<std::string> args = {std::to_string(base)};
            _c->eval<long long>(kBackfillLua, keys.begin(), keys.end(), args.begin(), args.end());
        } catch(std::exception &e) {
            LOG_ERROR("SeqGen.backfill_session 失败 {} base={}: {}", ssid, base, e.what());
        }
    }
    void backfill_user(const std::string &uid, unsigned long base) {
        try {
            std::vector<std::string> keys = {key::seq_user_key(uid)};
            std::vector<std::string> args = {std::to_string(base)};
            _c->eval<long long>(kBackfillLua, keys.begin(), keys.end(), args.begin(), args.end());
        } catch(std::exception &e) {
            LOG_ERROR("SeqGen.backfill_user 失败 {} base={}: {}", uid, base, e.what());
        }
    }
private:
    static constexpr const char *kBackfillLua =
        "local cur = redis.call('GET', KEYS[1]) "
        "if not cur or tonumber(cur) < tonumber(ARGV[1]) then "
        "    redis.call('SET', KEYS[1], ARGV[1]) "
        "    return 1 "
        "end "
        "return 0";
    RedisClient::ptr _c;
};

// =============================================================================
// 最近一条消息预览缓存（替代 chat_session.last_message_* 行级写热点）
// =============================================================================

class LastMessage
{
public:
    using ptr = std::shared_ptr<LastMessage>;
    LastMessage(const RedisClient::ptr &c) : _c(c) {}

    /* brief: 写最后一条消息预览（已序列化 JSON 字符串）；TTL 24h */
    void set(const std::string &ssid, const std::string &preview_json,
             std::chrono::seconds ttl = kLastMsgTtl) {
        try { _c->set(key::kLastMsg + ssid, preview_json, randomized_ttl(ttl)); }
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
    RedisClient::ptr _c;
};

// =============================================================================
// 用户在线设备集合（推送下发入口）
// =============================================================================

class DeviceSet
{
public:
    using ptr = std::shared_ptr<DeviceSet>;
    DeviceSet(const RedisClient::ptr &c) : _c(c) {}

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
    RedisClient::ptr _c;
};

// =============================================================================
// 群消息已读暂存（异步落库前的高 QPS 缓冲）
// =============================================================================

class ReadAck
{
public:
    using ptr = std::shared_ptr<ReadAck>;
    ReadAck(const RedisClient::ptr &c) : _c(c) {}

    /* brief: 用户对消息已读，幂等添加到 SET */
    void ack(unsigned long message_id, const std::string &uid,
             std::chrono::seconds ttl = kReadAckTtl) {
        try {
            std::string k = key::kReadAck + std::to_string(message_id);
            _c->sadd(k, uid);
            _c->expire(k, randomized_ttl(ttl));
        } catch(std::exception &e) {
            LOG_ERROR("ReadAck.ack 失败 mid={} uid={}: {}", message_id, uid, e.what());
        }
    }
    /* brief: 已读人数（角标"已读 X 人"用） */
    long count(unsigned long message_id) {
        try { return _c->scard(key::kReadAck + std::to_string(message_id)); }
        catch(std::exception &e) { LOG_ERROR("ReadAck.count 失败 {}: {}", message_id, e.what()); return 0; }
    }
    /* brief: 后台批量刷库后调用，原子 SMEMBERS + DEL 防并发 ack 丢失 */
    std::vector<std::string> drain(unsigned long message_id) {
        std::vector<std::string> res;
        try {
            std::string k = key::kReadAck + std::to_string(message_id);
            // 原子 drain：先读全量再删，避免并发 ack() 在 SMEMBERS 与 DEL 之间被漏掉
            static const char *kDrainLua =
                "local members = redis.call('SMEMBERS', KEYS[1]) "
                "redis.call('DEL', KEYS[1]) "
                "return members";
            std::vector<std::string> keys = {k};
            std::vector<std::string> args;
            _c->eval(kDrainLua, keys.begin(), keys.end(), args.begin(), args.end(),
                     std::back_inserter(res));
        } catch(std::exception &e) {
            LOG_ERROR("ReadAck.drain 失败 {}: {}", message_id, e.what());
        }
        return res;
    }
private:
    RedisClient::ptr _c;
};

// =============================================================================
// 群成员列表缓存（替代每条消息一次 ChatSession.GetMemberIdList RPC）
// =============================================================================

class Members
{
public:
    using ptr = std::shared_ptr<Members>;
    Members(const RedisClient::ptr &c) : _c(c) {}

    struct Snapshot {
        std::vector<std::string> members;
        uint64_t version = 0;
        bool stable = true;
    };

    uint64_t version(const std::string &ssid) {
        try {
            auto v = _c->get(key::members_version_key(ssid));
            if (!v) return 0;
            auto parsed = parse_cache_version(*v);
            if (!parsed.has_value()) {
                LOG_ERROR("Members.version 无法解析 {}: {}", ssid, *v);
                return kUnknownCacheVersion;
            }
            return *parsed;
        } catch(std::exception &e) {
            LOG_ERROR("Members.version 失败 {}: {}", ssid, e.what());
            return kUnknownCacheVersion;
        }
    }

    /* brief: 取群成员列表；空返回 → 调用方回查 RPC + warm() */
    std::vector<std::string> list(const std::string &ssid) {
        std::vector<std::string> res;
        try { _c->smembers(key::members_key(ssid), std::inserter(res, res.end())); }
        catch(std::exception &e) { LOG_ERROR("Members.list 失败 {}: {}", ssid, e.what()); }
        return res;
    }

    Snapshot list_snapshot(const std::string &ssid) {
        Snapshot snap;
        try {
            auto before = version(ssid);
            _c->smembers(key::members_key(ssid), std::inserter(snap.members, snap.members.end()));
            auto after = version(ssid);
            snap.version = after;
            snap.stable = cache_snapshot_is_stable(before, after);
        } catch(std::exception &e) {
            LOG_ERROR("Members.list_snapshot 失败 {}: {}", ssid, e.what());
            snap.stable = false;
        }
        return snap;
    }

    /* brief: 缓存预热 / 重建 */
    void warm(const std::string &ssid, const std::vector<std::string> &uids,
              std::chrono::seconds ttl = kMembersTtl) {
        auto observed = version(ssid);
        (void)warm_if_version(ssid, uids, observed, ttl);
    }

    bool warm_if_version(const std::string &ssid, const std::vector<std::string> &uids,
                         uint64_t observed_version,
                         std::chrono::seconds ttl = kMembersTtl) {
        if(uids.empty()) return false;
        if(!cache_version_is_known(observed_version)) return false;
        if(!cache_ttl_allows_write(ttl)) return false;
        try {
            std::vector<std::string> keys = {
                key::members_key(ssid),
                key::members_version_key(ssid),
                key::members_sentinel_key(ssid)
            };
            std::vector<std::string> args = {
                std::to_string(observed_version),
                std::to_string(randomized_ttl(ttl).count())
            };
            args.insert(args.end(), uids.begin(), uids.end());
            auto ok = _c->eval<long long>(kWarmIfVersionLua, keys.begin(), keys.end(),
                                          args.begin(), args.end());
            return ok == 1;
        } catch(std::exception &e) {
            LOG_ERROR("Members.warm_if_version 失败 {}: {}", ssid, e.what());
            return false;
        }
    }
    /* brief: 单成员加入/退出（增量维护） */
    void add(const std::string &ssid, const std::string &uid) {
        try {
            std::vector<std::string> keys = {
                key::members_key(ssid),
                key::members_version_key(ssid),
                key::members_sentinel_key(ssid),
            };
            std::vector<std::string> args = {
                uid,
                std::to_string(randomized_ttl(kMembersTtl).count()),
            };
            (void)_c->eval<long long>(kAddMemberLua, keys.begin(), keys.end(),
                                      args.begin(), args.end());
        }
        catch(std::exception &e) { LOG_ERROR("Members.add 失败 {}-{}: {}", ssid, uid, e.what()); }
    }
    void remove(const std::string &ssid, const std::string &uid) {
        try {
            std::vector<std::string> keys = {
                key::members_key(ssid),
                key::members_version_key(ssid),
                key::members_sentinel_key(ssid),
            };
            std::vector<std::string> args = {
                uid,
                std::to_string(randomized_ttl(kMembersTtl).count()),
            };
            (void)_c->eval<long long>(kRemoveMemberLua, keys.begin(), keys.end(),
                                      args.begin(), args.end());
        }
        catch(std::exception &e) { LOG_ERROR("Members.remove 失败 {}-{}: {}", ssid, uid, e.what()); }
    }
    /* brief: 整组失效（解散群 / DDL 变更），同时推进版本阻止旧 warm 回写 */
    void invalidate(const std::string &ssid) {
        try {
            std::vector<std::string> keys = {
                key::members_key(ssid),
                key::members_sentinel_key(ssid),
                key::members_version_key(ssid)
            };
            std::vector<std::string> args;
            _c->eval<long long>(kInvalidateLua, keys.begin(), keys.end(),
                                args.begin(), args.end());
        }
        catch(std::exception &e) { LOG_ERROR("Members.invalidate 失败 {}: {}", ssid, e.what()); }
    }
    void touch_ttl(const std::string &ssid, std::chrono::seconds ttl = kMembersTtl) {
        if(!cache_ttl_allows_write(ttl)) return;
        try { _c->expire(key::members_key(ssid), randomized_ttl(ttl)); }
        catch (std::exception &e) { LOG_ERROR("Members.touch_ttl 失败 {}: {}", ssid, e.what()); }
    }
    /* brief: 设置会话不存在哨兵（负缓存），独立 key，不与成员数据混合 */
    void set_sentinel(const std::string &ssid, std::chrono::seconds ttl = std::chrono::seconds(60)) {
        auto observed = version(ssid);
        (void)set_sentinel_if_version(ssid, observed, ttl);
    }

    bool set_sentinel_if_version(const std::string &ssid, uint64_t observed_version,
                                 std::chrono::seconds ttl = std::chrono::seconds(60)) {
        if(!cache_version_is_known(observed_version)) return false;
        if(!cache_ttl_allows_write(ttl)) return false;
        try {
            std::vector<std::string> keys = {
                key::members_sentinel_key(ssid),
                key::members_version_key(ssid),
                key::members_key(ssid)
            };
            std::vector<std::string> args = {
                std::to_string(observed_version),
                std::to_string(randomized_ttl(ttl).count())
            };
            auto ok = _c->eval<long long>(kSentinelIfVersionLua, keys.begin(), keys.end(),
                                          args.begin(), args.end());
            return ok == 1;
        } catch (std::exception &e) {
            LOG_ERROR("Members.set_sentinel_if_version 失败 {}: {}", ssid, e.what());
            return false;
        }
    }

    /* brief: 检查哨兵是否存在（会话确认不存在） */
    bool is_sentinel(const std::string &ssid) {
        try {
            return _c->get(key::members_sentinel_key(ssid)).has_value();
        } catch (std::exception &e) {
            LOG_ERROR("Members.is_sentinel 失败 {}: {}", ssid, e.what());
            return false;
        }
    }

    void warm_sentinel(const std::string &ssid, std::chrono::seconds ttl = std::chrono::seconds(60)) {
        set_sentinel(ssid, ttl);
    }
private:
    static constexpr const char *kWarmIfVersionLua =
        "local cur = redis.call('GET', KEYS[2]) "
        "if not cur then cur = '0' end "
        "if cur ~= ARGV[1] then return 0 end "
        "redis.call('DEL', KEYS[1]) "
        "for i = 3, #ARGV do redis.call('SADD', KEYS[1], ARGV[i]) end "
        "redis.call('EXPIRE', KEYS[1], ARGV[2]) "
        "redis.call('DEL', KEYS[3]) "
        "return 1";

    static constexpr const char *kSentinelIfVersionLua =
        "local cur = redis.call('GET', KEYS[2]) "
        "if not cur then cur = '0' end "
        "if cur ~= ARGV[1] then return 0 end "
        "redis.call('DEL', KEYS[3]) "
        "redis.call('SET', KEYS[1], '1', 'EX', ARGV[2]) "
        "return 1";

    static constexpr const char *kAddMemberLua =
        "redis.call('SADD', KEYS[1], ARGV[1]) "
        "redis.call('DEL', KEYS[3]) "
        "redis.call('INCR', KEYS[2]) "
        "redis.call('EXPIRE', KEYS[1], ARGV[2]) "
        "return 1";

    static constexpr const char *kRemoveMemberLua =
        "redis.call('SREM', KEYS[1], ARGV[1]) "
        "redis.call('DEL', KEYS[3]) "
        "redis.call('INCR', KEYS[2]) "
        "redis.call('EXPIRE', KEYS[1], ARGV[2]) "
        "return 1";

    static constexpr const char *kInvalidateLua =
        "redis.call('INCR', KEYS[3]) "
        "redis.call('DEL', KEYS[1]) "
        "redis.call('DEL', KEYS[2]) "
        "return 1";

    RedisClient::ptr _c;
};

// =============================================================================
// 在线路由表（多 Push 实例下：uid -> 持有 ws 连接的 push_instance_id 集合）
// =============================================================================

class OnlineRoute
{
public:
    using ptr = std::shared_ptr<OnlineRoute>;
    OnlineRoute(const RedisClient::ptr &c) : _c(c) {}

    /* brief: 设备上线 — HSET uid did instance */
    void bind(const std::string &uid, const std::string &device_id,
              const std::string &push_instance,
              std::chrono::seconds ttl = kOnlineTtl) {
        try {
            std::string k = key::online_key(uid);
            _c->hset(k, device_id, push_instance);
            _c->expire(k, randomized_ttl(ttl));
        } catch(std::exception &e) {
            LOG_ERROR("OnlineRoute.bind 失败 {}-{}-{}: {}", uid, device_id, push_instance, e.what());
        }
    }
    /* brief: 心跳续期（续整个 uid 的 HASH） */
    void touch(const std::string &uid, std::chrono::seconds ttl = kOnlineTtl) {
        try { _c->expire(key::online_key(uid), randomized_ttl(ttl)); }
        catch(std::exception &e) { LOG_ERROR("OnlineRoute.touch 失败 {}: {}", uid, e.what()); }
    }
    /* brief: 设备下线 — HDEL uid did */
    void unbind(const std::string &uid, const std::string &device_id,
                const std::string &push_instance) {
        try { _c->hdel(key::online_key(uid), device_id); }
        catch(std::exception &e) { LOG_ERROR("OnlineRoute.unbind 失败 {}-{}-{}: {}", uid, device_id, push_instance, e.what()); }
    }
    /* brief: 取用户所有在线设备 → device_id 列表 */
    std::vector<std::string> devices(const std::string &uid) {
        std::vector<std::string> res;
        try {
            _c->hkeys(key::online_key(uid), std::back_inserter(res));
        } catch(std::exception &e) { LOG_ERROR("OnlineRoute.devices 失败 {}: {}", uid, e.what()); }
        return res;
    }
    /* brief: 取用户所有在线设备及对应实例 → device_id → instance_id 映射（单次 HGETALL） */
    std::unordered_map<std::string, std::string> device_instances_map(const std::string &uid) {
        std::unordered_map<std::string, std::string> res;
        try {
            _c->hgetall(key::online_key(uid), std::inserter(res, res.end()));
        } catch (std::exception &e) {
            LOG_ERROR("OnlineRoute.device_instances_map 失败 {}: {}", uid, e.what());
        }
        return res;
    }
    /* brief: 取设备所在 Push 实例 */
    std::string device_instance(const std::string &uid, const std::string &device_id) {
        try {
            auto v = _c->hget(key::online_key(uid), device_id);
            return v ? *v : "";
        } catch(std::exception &e) {
            LOG_ERROR("OnlineRoute.device_instance 失败 {}-{}: {}", uid, device_id, e.what());
            return "";
        }
    }
    /* brief: 是否有任意在线设备 */
    bool online(const std::string &uid) {
        try { return _c->hlen(key::online_key(uid)) > 0; }
        catch(std::exception &e) { LOG_ERROR("OnlineRoute.online 失败 {}: {}", uid, e.what()); return false; }
    }
private:
    RedisClient::ptr _c;
};

// =============================================================================
// 令牌桶限流（Lua 原子补 token + 扣 token，避免固定窗口边界放大）
// =============================================================================

class RateLimiter
{
public:
    using ptr = std::shared_ptr<RateLimiter>;
    RateLimiter(const RedisClient::ptr &c) : _c(c) {}

    /**
     * brief: token bucket（Lua 原子读写）
     *   - window_sec 内最多补充 max_count 个 token，桶容量 max_count
     *   - 命中限制返回 false（业务可返回 429 / RATE_LIMITED）
     */
    bool allow(const std::string &key_full, int max_count, int window_sec) {
        try {
            std::vector<std::string> keys = {key_full};
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::vector<std::string> args = {
                std::to_string(max_count),
                std::to_string(window_sec),
                std::to_string(now_ms)
            };
            long long cur = _c->eval<long long>(kRateLimitScript, keys.begin(), keys.end(),
                                                args.begin(), args.end());
            return cur == 1;
        } catch(std::exception &e) {
            LOG_ERROR("RateLimiter.allow {}: {}", key_full, e.what());
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
    RedisClient::ptr _c;
    static const std::string kRateLimitScript;
};

inline const std::string RateLimiter::kRateLimitScript = R"(
    local capacity = tonumber(ARGV[1])
    local window_ms = tonumber(ARGV[2]) * 1000
    local now_ms = tonumber(ARGV[3])
    if capacity <= 0 or window_ms <= 0 then
        return 1
    end

    local interval_ms = math.floor(window_ms / capacity)
    if interval_ms < 1 then interval_ms = 1 end

    local tokens = tonumber(redis.call('HGET', KEYS[1], 'tokens'))
    local ts = tonumber(redis.call('HGET', KEYS[1], 'ts'))
    if tokens == nil or ts == nil or now_ms < ts then
        tokens = capacity
        ts = now_ms
    else
        local refill = math.floor((now_ms - ts) / interval_ms)
        if refill > 0 then
            tokens = math.min(capacity, tokens + refill)
            ts = ts + refill * interval_ms
        end
    end

    if tokens <= 0 then
        redis.call('HSET', KEYS[1], 'tokens', tokens, 'ts', ts)
        redis.call('PEXPIRE', KEYS[1], window_ms * 2)
        return 0
    end

    tokens = tokens - 1
    redis.call('HSET', KEYS[1], 'tokens', tokens, 'ts', ts)
    redis.call('PEXPIRE', KEYS[1], window_ms * 2)
    return 1
)";

// =============================================================================
// 推送投递 outbox 兜底（message → push_queue 投递失败时持久化，由后台 reaper 重投）
// =============================================================================

class PushOutbox
{
public:
    using ptr = std::shared_ptr<PushOutbox>;
    PushOutbox(const RedisClient::ptr &c) : _c(c) {}

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
private:
    RedisClient::ptr _c;
};

// =============================================================================
// 跨实例推送投递 outbox 兜底（PushBatch 跨实例失败时持久化，由 reaper 重试）
// =============================================================================

class CrossInstanceOutbox
{
public:
    using ptr = std::shared_ptr<CrossInstanceOutbox>;
    CrossInstanceOutbox(const RedisClient::ptr &c) : _c(c) {}

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

private:
    RedisClient::ptr _c;
};

// =============================================================================
// ES 索引投递 outbox 兜底（message.onDBMessage → es_index_exchange 投递失败时持久化）
// =============================================================================

class ESOutbox
{
public:
    using ptr = std::shared_ptr<ESOutbox>;
    ESOutbox(const RedisClient::ptr &c, const std::string &key)
        : _c(c), _key(key) {}
    ESOutbox(const RedisClient::ptr &c) : ESOutbox(c, key::es_outbox_key()) {}

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

private:
    RedisClient::ptr _c;
    std::string _key;
};

// =============================================================================
// 推送未 ACK 缓冲（B5 用：超时未 ack 的消息进 Sorted Set，按时间戳重传）
// =============================================================================

class UnackedPush
{
public:
    using ptr = std::shared_ptr<UnackedPush>;
    UnackedPush(const RedisClient::ptr &c) : _c(c) {}

    static std::string key_for(const std::string &uid, const std::string &device_id) {
        return std::string(key::kUnacked) + "{" + uid + ":" + device_id + "}";
    }
    static std::string idx_key_for(const std::string &uid, const std::string &device_id) {
        return std::string(key::kUnacked) + "idx:{" + uid + ":" + device_id + "}";
    }

    /* brief: 入待重传队列（per-device，存 payload_b64 直接用） */
    void push(const std::string &uid, const std::string &device_id,
              unsigned long user_seq, const std::string &payload_b64,
              long long score_ts, std::chrono::seconds ttl = kUnackedTtl) {
        try {
            std::string k = key_for(uid, device_id);
            std::string ik = idx_key_for(uid, device_id);
            std::string member = std::to_string(user_seq) + ":" + payload_b64;
            _c->zadd(k, member, static_cast<double>(score_ts));
            _c->hset(ik, std::to_string(user_seq), payload_b64);
            _c->expire(k, ttl);
            _c->expire(ik, ttl);
        } catch(std::exception &e) {
            LOG_ERROR("UnackedPush.push 失败 {}-{}-{}: {}", uid, device_id, user_seq, e.what());
        }
    }
    /* brief: 客户端 ACK 后移除（per-device，O(1) via HASH index） */
    void ack(const std::string &uid, const std::string &device_id,
             unsigned long user_seq) {
        try {
            std::string k = key_for(uid, device_id);
            std::string ik = idx_key_for(uid, device_id);
            auto payload = _c->hget(ik, std::to_string(user_seq));
            if (payload) {
                std::string member = std::to_string(user_seq) + ":" + *payload;
                _c->zrem(k, member);
                _c->hdel(ik, std::to_string(user_seq));
            }
        } catch(std::exception &e) {
            LOG_ERROR("UnackedPush.ack 失败 {}-{}-{}: {}", uid, device_id, user_seq, e.what());
        }
    }
    /* brief: 取"成熟可重传"的项（per-device，返回 user_seq+payload 对） */
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
    /* brief: 重发后推迟这批 user_seq 的下次重发时机 + 续期 TTL（O(1) via HASH index） */
    void bump_score(const std::string &uid, const std::string &device_id,
                    const std::vector<unsigned long> &user_seqs,
                    std::chrono::seconds ttl = kUnackedTtl) {
        if(user_seqs.empty()) return;
        try {
            std::string k = key_for(uid, device_id);
            std::string ik = idx_key_for(uid, device_id);
            long long now = static_cast<long long>(time(nullptr));
            for (unsigned long seq : user_seqs) {
                auto payload = _c->hget(ik, std::to_string(seq));
                if (payload) {
                    std::string member = std::to_string(seq) + ":" + *payload;
                    _c->zadd(k, member, static_cast<double>(now), sw::redis::UpdateType::EXIST);
                }
            }
            _c->expire(k, ttl);
            _c->expire(ik, ttl);
        } catch(std::exception &e) {
            LOG_ERROR("UnackedPush.bump_score 失败 {}-{}-{}: {}", uid, device_id, e.what());
        }
    }

private:
    RedisClient::ptr _c;
};

// =============================================================================
// Presence 状态管理（Push 进程内调用，无 RPC 开销）
// =============================================================================

class PresenceRedis
{
public:
    using ptr = std::shared_ptr<PresenceRedis>;
    PresenceRedis(const RedisClient::ptr &r) : _r(r) {}

    /* 设置状态 */
    void set_state(const std::string &uid, const std::string &state) {
        try { _r->hset(key::kPresence + uid, "state", state); }
        catch(std::exception &e) { LOG_ERROR("PresenceRedis.set_state 失败 {}: {}", uid, e.what()); }
    }

    /* 获取状态 */
    std::string get_state(const std::string &uid) {
        try {
            auto v = _r->hget(key::kPresence + uid, "state");
            return v ? *v : "offline";
        } catch(std::exception &e) {
            LOG_ERROR("PresenceRedis.get_state 失败 {}: {}", uid, e.what());
            return "offline";
        }
    }

    /* 更新最后活跃时间 */
    void touch_active(const std::string &uid, int64_t ts_ms) {
        try { _r->hset(key::kPresence + uid, "last_active", std::to_string(ts_ms)); }
        catch(std::exception &e) { LOG_ERROR("PresenceRedis.touch_active 失败 {}: {}", uid, e.what()); }
    }

    /* 设置自定义状态 */
    void set_custom_status(const std::string &uid, const std::string &text) {
        try { _r->hset(key::kPresence + uid, "custom_status", text); }
        catch(std::exception &e) { LOG_ERROR("PresenceRedis.set_custom_status 失败 {}: {}", uid, e.what()); }
    }

    /* 添加在线设备：与 Push._write_presence_online_ 使用相同的 per-device HASH 模式 */
    void add_device(const std::string &uid, const std::string &device_id) {
        try {
            auto k = key::presence_device_key(uid, device_id);
            _r->hset(k, "state", "ONLINE");
            _r->expire(k, std::chrono::seconds(120));
        } catch(std::exception &e) {
            LOG_ERROR("PresenceRedis.add_device 失败 {}-{}: {}", uid, device_id, e.what());
        }
    }

    /* 获取在线设备列表：SCAN 匹配 im:presence:device:{uid}:* */
    std::vector<std::string> get_devices(const std::string &uid) {
        std::vector<std::string> out;
        try {
            auto cursor = 0ULL;
            while (true) {
                std::vector<std::string> batch;
                cursor = _r->scan(cursor, key::presence_device_scan_pattern(uid), 100,
                                 std::back_inserter(batch));
                for (auto& k : batch) {
                    auto pos = k.rfind(':');
                    if (pos != std::string::npos) out.push_back(k.substr(pos + 1));
                }
                if (cursor == 0) break;
            }
        } catch(std::exception &e) {
            LOG_ERROR("PresenceRedis.get_devices 失败 {}: {}", uid, e.what());
        }
        return out;
    }

    /* 输入中指示 */
    void set_typing(const std::string &uid, const std::string &conv_id) {
        try {
            auto k = key::kPresenceTyping + uid;
            _r->sadd(k, conv_id);
            _r->expire(k, std::chrono::seconds(10));
        } catch(std::exception &e) {
            LOG_ERROR("PresenceRedis.set_typing 失败 {}-{}: {}", uid, conv_id, e.what());
        }
    }

    /* 订阅状态 */
    void subscribe(const std::string &uid, const std::string &target_uid) {
        try { _r->sadd(key::kPresenceSub + uid, target_uid); }
        catch(std::exception &e) { LOG_ERROR("PresenceRedis.subscribe 失败 {}-{}: {}", uid, target_uid, e.what()); }
    }

    void unsubscribe(const std::string &uid, const std::string &target_uid) {
        try { _r->srem(key::kPresenceSub + uid, target_uid); }
        catch(std::exception &e) { LOG_ERROR("PresenceRedis.unsubscribe 失败 {}-{}: {}", uid, target_uid, e.what()); }
    }

private:
    RedisClient::ptr _r;
};

} // namespace chatnow
