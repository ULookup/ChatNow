#pragma once

/**
 * JWT 签发 / 验签 — 横切 spec §2.1 §2.2 §2.5
 *
 * 设计要点：
 *   - 仅 HS256（启动时 fail-fast 任何其他 alg）
 *   - 多 kid 密钥 map：签发用 current_kid，验签按 token 头部 kid 找密钥
 *   - payload: { sub, did, jti, exp, iat, kid, [typ=refresh] }
 *   - verify 不查 Redis 黑名单（黑名单查询是 JwtStore 的职责）
 *
 * 异常：所有失败路径 throw chatnow::ServiceError，code 来自 chatnow::error::kAuth*。
 *
 * 并发：JwtCodec 构造后 _config 不可变；sign/verify 内部线程安全。
 */

#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "utils/trace_id.hpp"

#include "jwt-cpp/jwt.h"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace chatnow::auth {

struct JwtConfig {
    std::string current_kid;                                 // e.g. "v1"
    std::unordered_map<std::string, std::string> keys;       // kid -> raw key bytes (>=32)
    int access_ttl_sec  = 7200;                              // 2h
    int refresh_ttl_sec = 2592000;                           // 30d

    // 启动时调用：current_kid 必须在 keys；每个 key 字节数 >=32；ttl 必须 >0
    // fail-fast：任何不合法 throw std::runtime_error 让 main() 崩
    void validate_or_throw() const;
};

struct JwtClaims {
    std::string sub;       // user_id
    std::string did;       // device_id
    std::string jti;       // 本 token 唯一 id
    std::string kid;       // 签发用 kid
    int64_t     iat_sec = 0;
    int64_t     exp_sec = 0;
    bool        is_refresh = false;
};

class JwtCodec {
public:
    explicit JwtCodec(JwtConfig cfg) : _config(std::move(cfg)) {
        _config.validate_or_throw();
    }

    std::string sign_access(const std::string& user_id,
                            const std::string& device_id,
                            std::string jti = "");

    std::string sign_refresh(const std::string& user_id,
                             const std::string& device_id,
                             std::string jti = "");

    // 验签 + 解析 claims；以下情况抛 ServiceError：
    //   非 HS256 / kid 缺失 / kid 未知 / 签名失败  -> kAuthTokenInvalid
    //   exp <= now                                 -> kAuthTokenExpired
    //   require_refresh 与 typ 不匹配              -> kAuthTokenInvalid
    JwtClaims verify(const std::string& token, bool require_refresh = false);

    int access_ttl_sec()  const { return _config.access_ttl_sec; }
    int refresh_ttl_sec() const { return _config.refresh_ttl_sec; }

    // 工具：32 字符小写 hex 随机 jti（复用 utils::gen_trace_id 的实现）
    static std::string random_jti() { return ::chatnow::utils::gen_trace_id(); }

private:
    JwtConfig _config;

    std::string sign_internal(const std::string& user_id,
                              const std::string& device_id,
                              std::string jti,
                              int ttl_sec,
                              bool is_refresh);
};

inline void JwtConfig::validate_or_throw() const {
    if (current_kid.empty()) {
        throw std::runtime_error("auth.jwt.current_kid is empty");
    }
    if (keys.find(current_kid) == keys.end()) {
        throw std::runtime_error("auth.jwt.current_kid not in keys: " + current_kid);
    }
    if (access_ttl_sec <= 0) {
        throw std::runtime_error("auth.jwt.access_ttl_sec must be > 0");
    }
    if (refresh_ttl_sec <= 0) {
        throw std::runtime_error("auth.jwt.refresh_ttl_sec must be > 0");
    }
    for (const auto& kv : keys) {
        if (kv.second.size() < 32) {
            throw std::runtime_error("auth.jwt.keys[" + kv.first +
                                     "] must be >=32 bytes, got " +
                                     std::to_string(kv.second.size()));
        }
    }
}

inline std::string JwtCodec::sign_internal(const std::string& user_id,
                                           const std::string& device_id,
                                           std::string jti,
                                           int ttl_sec,
                                           bool is_refresh) {
    if (jti.empty()) jti = random_jti();
    const auto& kid = _config.current_kid;
    const auto& key = _config.keys.at(kid);
    auto now = std::chrono::system_clock::now();

    auto builder = jwt::create()
        .set_type("JWT")
        .set_key_id(kid)
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::seconds(ttl_sec))
        .set_payload_claim("sub", jwt::claim(user_id))
        .set_payload_claim("did", jwt::claim(device_id))
        .set_payload_claim("jti", jwt::claim(jti));
    if (is_refresh) {
        builder = builder.set_payload_claim("typ", jwt::claim(std::string("refresh")));
    }
    return builder.sign(jwt::algorithm::hs256{key});
}

inline std::string JwtCodec::sign_access(const std::string& user_id,
                                         const std::string& device_id,
                                         std::string jti) {
    return sign_internal(user_id, device_id, std::move(jti),
                         _config.access_ttl_sec, /*is_refresh=*/false);
}

inline std::string JwtCodec::sign_refresh(const std::string& user_id,
                                          const std::string& device_id,
                                          std::string jti) {
    return sign_internal(user_id, device_id, std::move(jti),
                         _config.refresh_ttl_sec, /*is_refresh=*/true);
}

inline JwtClaims JwtCodec::verify(const std::string& token, bool require_refresh) {
    using namespace ::chatnow::error;

    auto decoded = [&]() {
        try {
            return jwt::decode(token);
        } catch (const std::exception&) {
            throw ServiceError(kAuthTokenInvalid, "token decode failed");
        }
    }();
    if (decoded.get_algorithm() != "HS256") {
        throw ServiceError(kAuthTokenInvalid, "alg not HS256");
    }
    if (!decoded.has_key_id()) {
        throw ServiceError(kAuthTokenInvalid, "missing kid");
    }
    const auto kid = decoded.get_key_id();
    auto it = _config.keys.find(kid);
    if (it == _config.keys.end()) {
        throw ServiceError(kAuthTokenInvalid, "unknown kid: " + kid);
    }

    // jwt-cpp 的 verify 同时校验签名 + exp。先单独检查 exp，给出明确错误码。
    if (decoded.has_expires_at()) {
        auto now = std::chrono::system_clock::now();
        if (decoded.get_expires_at() <= now) {
            // 仍需先校验签名，避免泄漏过期 token 也被接受的可能。
            try {
                auto v = jwt::verify().allow_algorithm(jwt::algorithm::hs256{it->second}).leeway(60);
                v.verify(decoded);
            } catch (const std::exception&) {
                throw ServiceError(kAuthTokenInvalid, "signature invalid");
            }
            throw ServiceError(kAuthTokenExpired, "token expired");
        }
    }

    try {
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{it->second})
            .leeway(0);
        verifier.verify(decoded);
    } catch (const std::exception& e) {
        throw ServiceError(kAuthTokenInvalid,
                           std::string("verify failed: ") + e.what());
    }

    JwtClaims out;
    out.kid = kid;
    try {
        out.sub = decoded.get_payload_claim("sub").as_string();
        out.did = decoded.get_payload_claim("did").as_string();
        out.jti = decoded.get_payload_claim("jti").as_string();
    } catch (const std::exception&) {
        throw ServiceError(kAuthTokenInvalid, "claims missing sub/did/jti");
    }
    out.iat_sec = std::chrono::duration_cast<std::chrono::seconds>(
        decoded.get_issued_at().time_since_epoch()).count();
    out.exp_sec = std::chrono::duration_cast<std::chrono::seconds>(
        decoded.get_expires_at().time_since_epoch()).count();

    bool has_typ_refresh = false;
    if (decoded.has_payload_claim("typ")) {
        try {
            has_typ_refresh = (decoded.get_payload_claim("typ").as_string() == "refresh");
        } catch (...) { has_typ_refresh = false; }
    }
    out.is_refresh = has_typ_refresh;
    if (require_refresh != has_typ_refresh) {
        throw ServiceError(kAuthTokenInvalid,
                           require_refresh ? "expected refresh token"
                                           : "unexpected refresh token");
    }
    return out;
}

}  // namespace chatnow::auth
