#pragma once

/**
 * AuthContext + extract_auth(cntl)
 * ---
 * RPC handler 入口统一调用 extract_auth(cntl) 解析 brpc HTTP headers metadata
 * 中的 x-user-id / x-device-id / x-trace-id（由 Gateway 写入）。
 *
 * 强校验：x-user-id 与 x-device-id 缺失 → throw ServiceError(SYSTEM_INTERNAL_ERROR)。
 *   理由：Gateway 必须写入；缺失说明调用方未透传或 Gateway 出 bug，
 *   不属于业务错误，对客户端而言是 9001 内部错误。
 * 例外：x-trace-id 缺失时使用空字符串（不抛错），理由：内部 worker
 *   可能不带 trace_id；扩散到日志时简单缺一行字段，不影响业务。
 */

#include "auth/metadata_keys.hpp"
#include "error/service_error.hpp"
#include <brpc/controller.h>
#include <string>

namespace chatnow::auth {

struct AuthContext {
    std::string user_id;
    std::string device_id;
    std::string trace_id;
    std::string jwt_jti;       // 可空
};

inline std::string read_header(brpc::Controller* cntl, const char* key) {
    if (!cntl) return "";
    const std::string* v = cntl->http_request().GetHeader(key);
    return v ? *v : "";
}

inline AuthContext extract_auth(brpc::Controller* cntl) {
    AuthContext ctx;
    ctx.user_id   = read_header(cntl, kMetaUserId);
    ctx.device_id = read_header(cntl, kMetaDeviceId);
    ctx.trace_id  = read_header(cntl, kMetaTraceId);
    ctx.jwt_jti   = read_header(cntl, kMetaJwtJti);

    if (ctx.user_id.empty() || ctx.device_id.empty()) {
        throw ServiceError(/*SYSTEM_INTERNAL_ERROR=*/9001,
                           "missing auth metadata: x-user-id / x-device-id required");
    }
    return ctx;
}

}  // namespace chatnow::auth
