#pragma once

/**
 * JWT Redis 三类 key 操作 — 横切 spec §2.3
 *
 *   im:jwt:revoked:{jti}                  -> "1"          TTL = 剩余寿命
 *   im:jwt:rt:{user_id}:{device_id}       -> refresh_jti  TTL = refresh 寿命
 *   im:jwt:rt_chain:{old_jti}             -> "rotated"    TTL = 24h
 *
 * 失败模式：底层 redis 抛异常时函数自身吞掉 + LOG_ERROR + 返回保守值
 *   - is_revoked 失败 → false（不阻断业务，避免雪崩）
 *   - 写失败 → 仅日志，调用方按业务决定
 *
 * 重放检测：rotate_refresh_or_detect_reuse 用 SET NX 原子保护链节点。
 */

#include "infra/logger.hpp"

#include <sw/redis++/redis++.h>

#include <chrono>
#include <memory>
#include <string>

namespace chatnow::auth {

namespace jwt_key {
inline constexpr const char* kRevokedPrefix  = "im:jwt:revoked:";
inline constexpr const char* kRefreshPrefix  = "im:jwt:rt:";
inline constexpr const char* kRtChainPrefix  = "im:jwt:rt_chain:";
}  // namespace jwt_key

class JwtStore {
public:
    using ptr = std::shared_ptr<JwtStore>;
    explicit JwtStore(std::shared_ptr<sw::redis::Redis> c) : _c(std::move(c)) {}

    void revoke(const std::string& jti, int ttl_sec);
    bool is_revoked(const std::string& jti);

    void put_active_refresh(const std::string& user_id,
                            const std::string& device_id,
                            const std::string& refresh_jti,
                            int ttl_sec);

    std::string get_active_refresh(const std::string& user_id,
                                   const std::string& device_id);

    void clear_active_refresh(const std::string& user_id,
                              const std::string& device_id);

    enum class RotateResult { kOk, kReuseDetected };

    /**
     * 滚动刷新核心（spec §2.3）：
     *   1. SET rt_chain:{old_jti} "rotated" NX EX 24h
     *      命中 NX 失败 → 旧 refresh 已被用过 → kReuseDetected
     *   2. 写 rt:{uid}:{did} = new_refresh_jti，TTL = new_refresh_ttl_sec
     */
    RotateResult rotate_refresh_or_detect_reuse(const std::string& user_id,
                                                const std::string& device_id,
                                                const std::string& old_refresh_jti,
                                                const std::string& new_refresh_jti,
                                                int new_refresh_ttl_sec);

private:
    std::shared_ptr<sw::redis::Redis> _c;

    static constexpr int kChainTtlSec = 24 * 3600;
};

inline void JwtStore::revoke(const std::string& jti, int ttl_sec) {
    if (jti.empty() || ttl_sec <= 0) return;
    try {
        _c->set(std::string(jwt_key::kRevokedPrefix) + jti, "1",
                std::chrono::seconds(ttl_sec));
    } catch (const std::exception& e) {
        LOG_ERROR("JwtStore.revoke 失败 jti={}: {}", jti, e.what());
    }
}

inline bool JwtStore::is_revoked(const std::string& jti) {
    if (jti.empty()) return false;
    try {
        auto v = _c->get(std::string(jwt_key::kRevokedPrefix) + jti);
        return v.has_value();
    } catch (const std::exception& e) {
        LOG_ERROR("JwtStore.is_revoked 失败 jti={}: {}", jti, e.what());
        return false;
    }
}

inline void JwtStore::put_active_refresh(const std::string& user_id,
                                         const std::string& device_id,
                                         const std::string& refresh_jti,
                                         int ttl_sec) {
    if (user_id.empty() || device_id.empty() || refresh_jti.empty()) return;
    try {
        _c->set(std::string(jwt_key::kRefreshPrefix) + user_id + ":" + device_id,
                refresh_jti,
                std::chrono::seconds(ttl_sec));
    } catch (const std::exception& e) {
        LOG_ERROR("JwtStore.put_active_refresh 失败 u={} d={}: {}",
                  user_id, device_id, e.what());
    }
}

inline std::string JwtStore::get_active_refresh(const std::string& user_id,
                                                const std::string& device_id) {
    try {
        auto v = _c->get(std::string(jwt_key::kRefreshPrefix) + user_id + ":" + device_id);
        return v.value_or("");
    } catch (const std::exception& e) {
        LOG_ERROR("JwtStore.get_active_refresh 失败 u={} d={}: {}",
                  user_id, device_id, e.what());
        return "";
    }
}

inline void JwtStore::clear_active_refresh(const std::string& user_id,
                                           const std::string& device_id) {
    try {
        _c->del(std::string(jwt_key::kRefreshPrefix) + user_id + ":" + device_id);
    } catch (const std::exception& e) {
        LOG_ERROR("JwtStore.clear_active_refresh 失败 u={} d={}: {}",
                  user_id, device_id, e.what());
    }
}

inline JwtStore::RotateResult JwtStore::rotate_refresh_or_detect_reuse(
        const std::string& user_id,
        const std::string& device_id,
        const std::string& old_refresh_jti,
        const std::string& new_refresh_jti,
        int new_refresh_ttl_sec) {
    try {
        auto ok = _c->set(std::string(jwt_key::kRtChainPrefix) + old_refresh_jti,
                          "rotated",
                          std::chrono::seconds(kChainTtlSec),
                          sw::redis::UpdateType::NOT_EXIST);
        if (!ok) {
            return RotateResult::kReuseDetected;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("JwtStore.rotate chain SET 失败 old_jti={}: {}",
                  old_refresh_jti, e.what());
        // 链路写失败：保守按"未被重放"放行，下次会再尝试
    }
    put_active_refresh(user_id, device_id, new_refresh_jti, new_refresh_ttl_sec);
    return RotateResult::kOk;
}

}  // namespace chatnow::auth
