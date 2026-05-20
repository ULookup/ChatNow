#pragma once

/**
 * gateway_auth — JWT 鉴权中间件 + RpcMetadata 写入
 * 横切 spec §2.5
 *
 * 入口契约（每个 handler 顶部一行）：
 *
 *   chatnow::gateway::LogContextScope _trace_scope;
 *   chatnow::rpc::RpcMetadata meta;
 *   AuthInfo a;
 *   if (!chatnow::gateway::jwt_authenticate(request, response, _jwt_codec,
 *                                            _jwt_store, /*whitelisted=*\/false, a)) {
 *       return;  // 401 已写
 *   }
 *   ...
 *   chatnow::gateway::apply_auth_to_brpc(meta, a);
 *   chatnow::gateway::apply_metadata_to_brpc(meta, cntl);
 */

#include "auth/jwt_codec.hpp"
#include "auth/jwt_store.hpp"
#include "common/auth/metadata.pb.h"
#include "common/envelope.pb.h"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "log/log_context.hpp"
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

/* brief: 将 JWT claims 写入 RpcMetadata（user_id, device_id, jwt_jti）
 *   并补填 LogContext 的身份字段（trace_id 由 gateway_setup_trace 预先填入）。
 *
 *   前置条件：gateway_setup_trace 必须先于本函数调用，以保证 meta.trace_id() 非空。
 */
inline void apply_auth_to_brpc(::chatnow::rpc::RpcMetadata& meta,
                               const AuthInfo& a)
{
    if (!a.user_id.empty()) {
        meta.set_user_id(a.user_id);
    }
    if (!a.device_id.empty()) {
        meta.set_device_id(a.device_id);
    }
    if (a.authed && !a.jwt_jti.empty()) {
        meta.set_jwt_jti(a.jwt_jti);
    }
    ::chatnow::log::LogContext::set(
        meta.trace_id(),
        a.user_id, a.device_id);
}

/* brief: 把 RpcMetadata 序列化写入 brpc Controller 的 request_attachment
 */
inline void apply_metadata_to_brpc(const ::chatnow::rpc::RpcMetadata& meta,
                                   brpc::Controller& cntl)
{
    std::string data;
    meta.SerializeToString(&data);
    cntl.request_attachment().append(data);
}

}  // namespace chatnow::gateway
