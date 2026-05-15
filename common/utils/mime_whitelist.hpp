#pragma once

/**
 * MimeWhitelist —— mime 类型白名单 + 每类 size 上限
 * ---
 * 配置形式（与本仓 jsoncpp 风格一致）：
 *   [
 *     {"prefix":"image/jpeg",      "max_mb":20},
 *     {"prefix":"video/mp4",       "max_mb":500},
 *     {"prefix":"application/pdf", "max_mb":100}
 *   ]
 *
 * 注意：当前实现用"完整匹配"而非"前缀匹配"，字段名 `prefix` 来自 spec §3.4
 * 命名习惯，便于以后扩展为前缀模糊匹配（例如 "image/*"）。
 */

#include <cstdint>
#include <json/json.h>
#include <sstream>
#include <string>
#include <unordered_map>

namespace chatnow {

class MimeWhitelist {
public:
    /* brief: 解析 JSON 字符串，成功返回 true，失败保持旧白名单不变 */
    bool load_json(const std::string& s) {
        Json::Value root;
        Json::CharReaderBuilder b;
        std::string err;
        std::istringstream is(s);
        if (!Json::parseFromStream(b, is, &root, &err)) return false;
        return load_value(root);
    }

    /* brief: 从已解析的 jsoncpp Value 加载（启动时常见用法） */
    bool load_value(const Json::Value& root) {
        if (!root.isArray()) return false;
        decltype(_max) m;
        for (Json::ArrayIndex i = 0; i < root.size(); ++i) {
            const auto& e = root[i];
            if (!e.isMember("prefix") || !e.isMember("max_mb")) return false;
            const auto& mime = e["prefix"];
            const auto& mb   = e["max_mb"];
            if (!mime.isString() || !mb.isIntegral()) return false;
            int64_t mb_v = mb.asInt64();
            if (mb_v <= 0) return false;
            m[mime.asString()] = mb_v * 1024 * 1024;
        }
        _max = std::move(m);
        return true;
    }

    /* brief: mime 是否允许 + size 是否 ≤ 上限 */
    bool is_allowed(const std::string& mime, int64_t size) const {
        auto it = _max.find(mime);
        if (it == _max.end()) return false;
        return size > 0 && size <= it->second;
    }

    /* brief: 取 mime 对应 size 上限（字节）；不存在返回 -1 */
    int64_t max_size(const std::string& mime) const {
        auto it = _max.find(mime);
        return it == _max.end() ? -1 : it->second;
    }

private:
    std::unordered_map<std::string, int64_t> _max;
};

}  // namespace chatnow
