#pragma once

/**
 * gateway_auth — JWT 鉴权中间件 + brpc metadata 写入
 * 横切 spec §2.5
 *
 * 入口契约（每个 handler 顶部一行）：
 *
 *   chatnow::gateway::LogContextScope _trace_scope;
 *   AuthInfo a;
 *   if (!chatnow::gateway::jwt_authenticate(request, response, _jwt_codec,
 *                                            _jwt_store, /*whitelisted=*\/false, a)) {
 *       return;  // 401 已写
 *   }
 *   ...
 *   brpc::Controller cntl;
 *   chatnow::gateway::apply_auth_to_brpc(request, cntl, a);
 *
 * 白名单路径（Login/Register/SendVerifyCode/RefreshToken）传 whitelisted=true：
 *   - 不解析 Authorization header
 *   - 仍调 gateway_setup_trace 生成 trace_id（通过 apply_auth_to_brpc）
 *
 * 非白名单路径若 Authorization 缺失/无效/过期/被吊销 → jwt_authenticate
 * 写 401（HTTP）+ 简单 ResponseHeader 风格 body，并返回 false。
 */

#include "auth/jwt_codec.hpp"
#include "auth/jwt_store.hpp"
#include "auth/metadata_keys.hpp"
#include "common/envelope.pb.h"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "gateway_trace.hpp"
#include "infra/logger.hpp"

#include "httplib.h"
#include <brpc/controller.h>

#include <memory>
#include <string>

namespace chatnow::gateway {

struct AuthInfo {
    bool        authed = false;
    std::string user_id;
    std::string device_id;
    std::string jwt_jti;
};

/* brief: 解析 Authorization Bearer + 验签 + 黑名单检查
 *  whitelisted=true → 直接返回 true 且 authed=false（仅 trace_id 流程）
 *  失败时写 401 + ResponseHeader 风格 body，返回 false
 */
inline bool jwt_authenticate(const httplib::Request& request,
                             httplib::Response& response,
                             const std::shared_ptr<::chatnow::auth::JwtCodec>& codec,
                             const std::shared_ptr<::chatnow::auth::JwtStore>& store,
                             bool whitelisted,
                             AuthInfo& out)
{
    if (whitelisted) {
        out.authed = false;
        return true;
    }

    auto write_401 = [&](int32_t code, const std::string& msg) {
        ::chatnow::common::ResponseHeader rsp;
        rsp.set_success(false);
        rsp.set_error_code(code);
        rsp.set_error_message(msg);
        response.status = 401;
        response.set_content(rsp.SerializeAsString(), "application/x-protobuf");
    };

    auto it = request.headers.find("Authorization");
    if (it == request.headers.end()) {
        LOG_WARN("缺 Authorization header path={}", request.path);
        write_401(::chatnow::error::kAuthTokenInvalid, "missing Authorization");
        return false;
    }
    static const std::string kPrefix = "Bearer ";
    const std::string& auth_header = it->second;
    if (auth_header.size() <= kPrefix.size() ||
        auth_header.compare(0, kPrefix.size(), kPrefix) != 0) {
        write_401(::chatnow::error::kAuthTokenInvalid, "missing Bearer prefix");
        return false;
    }
    std::string token = auth_header.substr(kPrefix.size());

    try {
        auto claims = codec->verify(token, /*require_refresh=*/false);
        if (store->is_revoked(claims.jti)) {
            write_401(::chatnow::error::kAuthTokenInvalid, "token revoked");
            return false;
        }
        out.authed    = true;
        out.user_id   = claims.sub;
        out.device_id = claims.did;
        out.jwt_jti   = claims.jti;
        return true;
    } catch (const ::chatnow::ServiceError& e) {
        LOG_WARN("JWT 验签失败 path={} code={} msg={}",
                 request.path, e.code(), e.message());
        write_401(e.code(), e.message());
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("JWT 验签异常 path={}: {}", request.path, e.what());
        write_401(::chatnow::error::kSystemInternalError, "auth internal error");
        return false;
    }
}

/* brief: 调 gateway_setup_trace 解析 trace_id 并把 user_id/device_id/jwt_jti
 *        写入 brpc cntl HTTP header；返回 trace_id（调用方填回 X-Trace-Id 响应头）
 */
inline std::string apply_auth_to_brpc(const httplib::Request& request,
                                      brpc::Controller& cntl,
                                      const AuthInfo& a)
{
    std::string trace_id = ::chatnow::gateway::gateway_setup_trace(
        request, cntl, a.user_id, a.device_id);
    if (a.authed && !a.jwt_jti.empty()) {
        cntl.http_request().SetHeader(::chatnow::auth::kMetaJwtJti, a.jwt_jti);
    }
    return trace_id;
}

}  // namespace chatnow::gateway
