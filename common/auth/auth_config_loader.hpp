#pragma once

/**
 * Load JwtConfig from the unified runtime Secret resolver.
 * Startup fails closed when the source is missing, malformed, or invalid.
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

#include <sstream>
#include <stdexcept>
#include <string>

namespace chatnow::auth {

inline JwtConfig parse_jwt_config(const std::string& document) {
    std::istringstream input(document);
    Json::Value root;
    Json::CharReaderBuilder b;
    std::string parse_errors;
    if (!Json::parseFromStream(b, input, &root, &parse_errors)) {
        throw std::runtime_error("auth_config: parse_failed");
    }
    if (!root.isObject() || !root.isMember("auth") || !root["auth"].isObject() ||
        !root["auth"].isMember("jwt") || !root["auth"]["jwt"].isObject()) {
        throw std::runtime_error("auth_config: schema_invalid");
    }
    const auto& j = root["auth"]["jwt"];

    JwtConfig cfg;
    if (!j.isMember("current_kid") || !j["current_kid"].isString()) {
        throw std::runtime_error("auth_config: schema_invalid");
    }
    cfg.current_kid = j.get("current_kid", "").asString();
    if (j.isMember("access_ttl_sec")) {
        if (!j["access_ttl_sec"].isInt()) {
            throw std::runtime_error("auth_config: schema_invalid");
        }
        cfg.access_ttl_sec = j["access_ttl_sec"].asInt();
    }
    if (j.isMember("refresh_ttl_sec")) {
        if (!j["refresh_ttl_sec"].isInt()) {
            throw std::runtime_error("auth_config: schema_invalid");
        }
        cfg.refresh_ttl_sec = j["refresh_ttl_sec"].asInt();
    }
    if (!j.isMember("keys") || !j["keys"].isObject()) {
        throw std::runtime_error("auth_config: schema_invalid");
    }
    for (const auto& kid : j["keys"].getMemberNames()) {
        if (!j["keys"][kid].isString()) {
            throw std::runtime_error("auth_config: schema_invalid");
        }
        cfg.keys[kid] = j["keys"][kid].asString();
    }
    try {
        cfg.validate_or_throw();
    } catch (const std::exception&) {
        throw std::runtime_error("auth_config: validation_failed");
    }
    return cfg;
}

}  // namespace chatnow::auth
