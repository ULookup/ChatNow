#pragma once

/**
 * forward_auth_metadata(in, out)
 * ---
 * 服务间内部 RPC 调用前调用此函数：把入站 Controller 的 4 个鉴权 metadata
 * 字段（x-trace-id / x-user-id / x-device-id / x-jwt-jti）原样复制到出站
 * Controller。
 *
 * 用于场景：A 服务的 RPC handler 中需要调 B 服务的 RPC，B 服务的 handler
 * 需要知道"原始客户端身份"。透传后 B 服务的 extract_auth(out_cntl)
 * 就能拿到与 A handler 相同的 user_id / device_id。
 */

#include "auth/metadata_keys.hpp"
#include <brpc/controller.h>

namespace chatnow::auth {

namespace detail {

inline void copy_header(brpc::Controller* in, brpc::Controller* out, const char* key) {
    if (!in || !out) return;
    const std::string* v = in->http_request().GetHeader(key);
    if (v && !v->empty()) {
        out->http_request().SetHeader(key, *v);
    }
}

}  // namespace detail

inline void forward_auth_metadata(brpc::Controller* in, brpc::Controller* out) {
    detail::copy_header(in, out, kMetaTraceId);
    detail::copy_header(in, out, kMetaUserId);
    detail::copy_header(in, out, kMetaDeviceId);
    detail::copy_header(in, out, kMetaJwtJti);
}

}  // namespace chatnow::auth
