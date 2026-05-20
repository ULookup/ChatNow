#pragma once

/**
 * AuthContext + extract_auth(cntl)
 * ---
 * RPC handler 入口统一调用 extract_auth(cntl) 解析 brpc request_attachment
 * 中的 RpcMetadata（由 Gateway 写入）。
 *
 * 强校验：x-user-id 与 x-device-id 缺失 → throw ServiceError(SYSTEM_INTERNAL_ERROR)。
 *   理由：Gateway 必须写入；缺失说明调用方未透传或 Gateway 出 bug，
 *   不属于业务错误，对客户端而言是 9001 内部错误。
 * 例外：x-trace-id 缺失时使用空字符串（不抛错），理由：内部 worker
 *   可能不带 trace_id；扩散到日志时简单缺一行字段，不影响业务。
 */

#include "common/auth/metadata.pb.h"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "infra/logger.hpp"
#include <brpc/controller.h>
#include <string>

namespace chatnow::auth {

struct AuthContext {
    std::string user_id;
    std::string device_id;
    std::string trace_id;
    std::string jwt_jti;       // 可空
};

inline AuthContext extract_auth(brpc::Controller* cntl) {
    chatnow::rpc::RpcMetadata meta;
    bool ok = false;
    if (cntl) {
        ok = meta.ParseFromString(cntl->request_attachment().to_string());
    }
    if (!ok) {
        LOG_WARN("Failed to parse RpcMetadata from attachment");
        throw ServiceError(::chatnow::error::kSystemInternalError,
                           "missing auth metadata: user_id/device_id required");
    }

    AuthContext ctx;
    ctx.user_id   = meta.user_id();
    ctx.device_id = meta.device_id();
    ctx.trace_id  = meta.trace_id();
    ctx.jwt_jti   = meta.jwt_jti();

    if (ctx.user_id.empty() || ctx.device_id.empty()) {
        throw ServiceError(::chatnow::error::kSystemInternalError,
                           "missing auth metadata: user_id/device_id required");
    }
    return ctx;
}

}  // namespace chatnow::auth
