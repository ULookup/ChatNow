#pragma once

/**
 * 从 JSON 文件加载 JwtConfig — 横切 spec §2.2
 * 启动时 fail-fast：文件不存在 / JSON 解析失败 / validate 失败 → throw std::runtime_error
 *
 * JSON shape:
 * {
 *   "auth": {
 *     "jwt": {
 *       "current_kid": "v1",
 *       "keys": { "v1": "<>=32 字节字符串>" },
 *       "access_ttl_sec":  7200,
 *       "refresh_ttl_sec": 2592000
 *     }
 *   }
 * }
 *
 * 注意：keys 的 value 视作 raw bytes 直接喂给 HS256（不做 base64 解码）。
 */

#include "auth/jwt_codec.hpp"

#include <json/json.h>

#include <fstream>
#include <stdexcept>
#include <string>

namespace chatnow::auth {

inline JwtConfig load_jwt_config_from_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        throw std::runtime_error("auth_config: cannot open " + path);
    }
    Json::Value root;
    Json::CharReaderBuilder b;
    std::string err;
    if (!Json::parseFromStream(b, ifs, &root, &err)) {
        throw std::runtime_error("auth_config: parse failed: " + err);
    }
    if (!root.isMember("auth") || !root["auth"].isMember("jwt")) {
        throw std::runtime_error("auth_config: missing auth.jwt section in " + path);
    }
    const auto& j = root["auth"]["jwt"];

    JwtConfig cfg;
    cfg.current_kid = j.get("current_kid", "").asString();
    if (j.isMember("access_ttl_sec"))  cfg.access_ttl_sec  = j["access_ttl_sec"].asInt();
    if (j.isMember("refresh_ttl_sec")) cfg.refresh_ttl_sec = j["refresh_ttl_sec"].asInt();
    if (!j.isMember("keys") || !j["keys"].isObject()) {
        throw std::runtime_error("auth_config: missing auth.jwt.keys map");
    }
    for (const auto& kid : j["keys"].getMemberNames()) {
        cfg.keys[kid] = j["keys"][kid].asString();
    }
    cfg.validate_or_throw();
    return cfg;
}

}  // namespace chatnow::auth
