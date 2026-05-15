#pragma once

/**
 * error_codes.hpp —— C++ mirror of proto/common/error.proto ErrorCode enum.
 * ---
 * 手工同步：proto/common/error.proto 新增 code 时，请同步在此添加。
 *
 * 本头文件仅包含 P1 阶段实际被 C++ 字面量使用的 System 段错误码；
 * 其它段（auth / media / 等）由 P2 / P4 等 plan 在需要时按需扩展。
 * 这样可以在不引入 protobuf 依赖的前提下，避免 9001 等 magic number
 * 在多个 .hpp 中漂移。
 */

#include <cstdint>

namespace chatnow::error {

inline constexpr int32_t kOK                       = 0;

// 1000-1999 认证（与 proto/common/error.proto 同步）
inline constexpr int32_t kAuthInvalidCredentials    = 1001;
inline constexpr int32_t kAuthTokenExpired          = 1002;
inline constexpr int32_t kAuthTokenInvalid          = 1003;
inline constexpr int32_t kAuthUserNotFound          = 1004;
inline constexpr int32_t kAuthUserAlreadyExists     = 1005;
inline constexpr int32_t kAuthVerifyCodeInvalid     = 1006;
inline constexpr int32_t kAuthVerifyCodeExpired     = 1007;
inline constexpr int32_t kAuthRefreshTokenReused    = 1008;
inline constexpr int32_t kAuthDeviceRevoked         = 1009;

// 5000-5999 媒体（P4，与 proto/common/error.proto 同步）
inline constexpr int32_t kMediaFileTooLarge        = 5001;
inline constexpr int32_t kMediaUnsupportedFormat   = 5002;
inline constexpr int32_t kMediaUploadFailed        = 5003;
inline constexpr int32_t kMediaQuotaExceeded       = 5004;
inline constexpr int32_t kMediaHashMismatch        = 5005;
inline constexpr int32_t kMediaUploadIncomplete    = 5006;
inline constexpr int32_t kMediaPartNotFound        = 5007;
inline constexpr int32_t kMediaFileNotFound        = 5008;  // 本仓内部使用：file_id 不存在 / 已删除

inline constexpr int32_t kSystemInternalError      = 9001;
inline constexpr int32_t kSystemUnavailable        = 9002;
inline constexpr int32_t kSystemTimeout            = 9003;
inline constexpr int32_t kSystemInvalidArgument    = 9004;

}  // namespace chatnow::error
