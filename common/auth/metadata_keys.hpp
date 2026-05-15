#pragma once

/**
 * brpc Controller HTTP headers metadata key 常量
 * ---
 * 这些 key 由 Gateway 在鉴权后写入，所有后端服务在 RPC handler 入口
 * 通过 extract_auth() 读取。服务间内部调用必须用 forward_auth_metadata()
 * 透传同名 key。
 *
 * 命名风格：小写 + 横杠（HTTP header 习惯）。brpc 内部对 header 名做大小写
 * 不敏感比较，但写入 / 读取统一用小写避免歧义。
 */

#include <string>

namespace chatnow::auth {

inline constexpr const char* kMetaTraceId  = "x-trace-id";
inline constexpr const char* kMetaUserId   = "x-user-id";
inline constexpr const char* kMetaDeviceId = "x-device-id";
inline constexpr const char* kMetaJwtJti   = "x-jwt-jti";
inline constexpr const char* kMetaClientVer = "x-client-ver";  // 可选

// 内部调用占位值（spec §2.7）
inline constexpr const char* kSystemUserId   = "__system__";
inline constexpr const char* kInternalDeviceId = "__internal__";

}  // namespace chatnow::auth
