#pragma once

#include <optional>
#include <cstdint>
#include <string>

namespace chatnow::key {

inline constexpr const char* kSession    = "im:sess:";          // session_id -> user_id
inline constexpr const char* kStatus     = "im:status:";        // user_id    -> 1
inline constexpr const char* kVerifyCode = "im:code:";          // code_id    -> 验证码
inline constexpr const char* kSeqSession = "im:seq:ssid:";      // ssid       -> 会话级 seq
inline constexpr const char* kSeqUser    = "im:seq:uid:";       // uid        -> 用户级 seq
inline constexpr const char* kLastMsg    = "im:last:";          // ssid       -> 最后一条消息预览(JSON)
inline constexpr const char* kDeviceSet  = "im:dev:";           // uid        -> SET<device_id>
inline constexpr const char* kReadAck    = "im:read:";          // mid        -> SET<uid>
inline constexpr const char* kMembers    = "im:conversation:members:"; // cid -> SET<user_id>
inline constexpr const char* kMembersSentinel = "im:conversation:sentinel:"; // cid -> "1" (负缓存)
inline constexpr const char* kRateUser   = "im:rl:user:";       // uid        -> 令牌桶
inline constexpr const char* kRateSsid   = "im:rl:ssid:";       // ssid       -> 令牌桶
inline constexpr const char* kOnline     = "im:online:";        // uid        -> HASH { device_id: instance_id }
inline constexpr const char* kPushRoute  = "im:push:route:";    // uid        -> push_instance_id (单设备)
inline constexpr const char* kUnacked    = "im:unack:";         // uid        -> Sorted Set<msg_id, ts>
inline constexpr const char* kPushOutbox = "im:push:outbox";    // 全局 Sorted Set<serialized_payload, ts>
inline constexpr const char* kCrossOutbox = "im:push:cross_outbox";
inline constexpr const char* kESOutbox = "im:es:outbox";

inline constexpr const char* kPresence        = "im:presence:";         // {uid} → HASH {state,last_active,custom_status}
inline constexpr const char* kPresenceDevices = "im:presence:devices:"; // {uid} → SET<device_id>, TTL 120s
inline constexpr const char* kPresenceTyping  = "im:presence:typing:";  // {uid} → SET<conversation_id>, TTL 10s
inline constexpr const char* kPresenceSub     = "im:presence:sub:";     // {uid} → SET<user_id>

inline std::string hash_tag(const std::string &id) {
    return "{" + id + "}";
}

inline std::string seq_session_key(const std::string &ssid) {
    return std::string(kSeqSession) + hash_tag(ssid);
}

inline std::string seq_user_key(const std::string &uid) {
    return std::string(kSeqUser) + hash_tag(uid);
}

inline std::string members_key(const std::string &cid) {
    return std::string(kMembers) + hash_tag(cid);
}

inline std::string members_sentinel_key(const std::string &cid) {
    return std::string(kMembersSentinel) + hash_tag(cid);
}

inline std::string members_version_key(const std::string &cid) {
    return std::string("im:conversation:members_ver:") + hash_tag(cid);
}

inline std::string members_warm_lock_key(const std::string &cid) {
    return std::string("warm:members:") + hash_tag(cid);
}

inline std::string local_members_cache_key(const std::string &cid) {
    return std::string("local:members:") + hash_tag(cid);
}

inline std::string local_user_info_cache_key(const std::string &uid) {
    return std::string("local:user:") + hash_tag(uid);
}

inline uint32_t fnv1a_32(const std::string &value) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

inline uint32_t user_info_bucket(const std::string &uid) {
    return fnv1a_32(uid) % 64u;
}

inline std::string user_info_key(const std::string &uid) {
    auto bucket = std::to_string(user_info_bucket(uid));
    return "im:user:{" + bucket + "}:" + uid;
}

inline std::string local_route_cache_key(const std::string &uid) {
    return std::string("local:route:") + hash_tag(uid);
}

inline std::string online_key(const std::string &uid) {
    return std::string(kOnline) + hash_tag(uid);
}

inline std::string device_set_key(const std::string &uid) {
    return std::string(kDeviceSet) + hash_tag(uid);
}

inline std::string online_scan_pattern() {
    return std::string(kOnline) + "*";
}

inline std::optional<std::string> uid_from_online_key(const std::string &key) {
    const std::string prefix = kOnline;
    if (key.rfind(prefix, 0) != 0 || key.size() == prefix.size()) {
        return std::nullopt;
    }
    const auto tagged = key.substr(prefix.size());
    if (tagged.size() < 3 || tagged.front() != '{' || tagged.back() != '}') {
        return std::nullopt;
    }
    return tagged.substr(1, tagged.size() - 2);
}

inline std::string presence_key(const std::string &uid) {
    return std::string(kPresence) + hash_tag(uid);
}

inline std::string presence_device_key(const std::string &uid,
                                       const std::string &device_id) {
    return std::string("im:presence:device:") + hash_tag(uid) + ":" + device_id;
}

inline std::string presence_device_scan_pattern(const std::string &uid) {
    return std::string("im:presence:device:") + hash_tag(uid) + ":*";
}

inline std::string presence_sub_key(const std::string &uid) {
    return std::string(kPresenceSub) + hash_tag(uid);
}

inline std::string es_outbox_key() {
    return std::string(kESOutbox);
}

inline std::string es_outbox_key(const std::string &scope) {
    return es_outbox_key() + ":" + hash_tag(scope);
}

} // namespace chatnow::key
