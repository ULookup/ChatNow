#include "utils/cache_version.hpp"
#include "utils/redis_keys.hpp"

#include <cassert>
#include <chrono>
#include <limits>
#include <iostream>
#include <string>

static std::string hash_tag(const std::string &key) {
    auto l = key.find('{');
    auto r = key.find('}', l == std::string::npos ? 0 : l + 1);
    if (l == std::string::npos || r == std::string::npos || r <= l + 1) return "";
    return key.substr(l + 1, r - l - 1);
}

int main() {
    auto members = chatnow::key::members_key("c1");
    auto sentinel = chatnow::key::members_sentinel_key("c1");
    auto version = chatnow::key::members_version_key("c1");
    auto warm_lock = chatnow::key::members_warm_lock_key("c1");
    auto other = chatnow::key::members_key("c2");
    auto local_members = chatnow::key::local_members_cache_key("c1");
    auto local_user = chatnow::key::local_user_info_cache_key("u1");
    auto local_route = chatnow::key::local_route_cache_key("u1");
    auto online = chatnow::key::online_key("u1");
    auto presence = chatnow::key::presence_key("u1");
    auto presence_device = chatnow::key::presence_device_key("u1", "d1");
    auto presence_sub = chatnow::key::presence_sub_key("u1");
    auto es_identity = chatnow::key::es_outbox_key("identity");

    assert(members == "im:conversation:members:{c1}");
    assert(hash_tag(members) == "c1");
    assert(hash_tag(sentinel) == "c1");
    assert(hash_tag(version) == "c1");
    assert(hash_tag(warm_lock) == "c1");
    assert(warm_lock == "warm:members:{c1}");
    assert(hash_tag(other) == "c2");
    assert(local_members == "local:members:{c1}");
    assert(local_user == "local:user:{u1}");
    assert(local_route == "local:route:{u1}");
    assert(online == "im:online:{u1}");
    assert(chatnow::key::online_scan_pattern() == "im:online:*");
    assert(chatnow::key::uid_from_online_key(online).value_or("") == "u1");
    assert(!chatnow::key::uid_from_online_key("other:u1").has_value());
    assert(presence == "im:presence:{u1}");
    assert(hash_tag(presence) == "u1");
    assert(presence_device == "im:presence:device:{u1}:d1");
    assert(hash_tag(presence_device) == "u1");
    assert(chatnow::key::presence_device_scan_pattern("u1") == "im:presence:device:{u1}:*");
    assert(presence_sub == "im:presence:sub:{u1}");
    assert(chatnow::key::es_outbox_key() == "im:es:outbox");
    assert(es_identity == "im:es:outbox:{identity}");
    assert(hash_tag(es_identity) == "identity");

    assert(chatnow::cache_version_accepts_warm(7, 7));
    assert(!chatnow::cache_version_accepts_warm(7, 8));
    assert(!chatnow::cache_version_accepts_warm(8, 7));
    assert(!chatnow::cache_version_accepts_warm(chatnow::kUnknownCacheVersion, 7));
    assert(!chatnow::cache_version_accepts_warm(7, chatnow::kUnknownCacheVersion));
    assert(chatnow::cache_snapshot_is_stable(7, 7));
    assert(!chatnow::cache_snapshot_is_stable(7, 8));
    assert(!chatnow::cache_snapshot_is_stable(chatnow::kUnknownCacheVersion,
                                             chatnow::kUnknownCacheVersion));
    assert(!chatnow::cache_snapshot_is_stable(chatnow::kUnknownCacheVersion, 7));
    assert(!chatnow::cache_snapshot_is_stable(7, chatnow::kUnknownCacheVersion));
    assert(chatnow::cache_l1_version_matches(7, 7));
    assert(!chatnow::cache_l1_version_matches(7, 8));
    assert(!chatnow::cache_l1_version_matches(chatnow::kUnknownCacheVersion,
                                             chatnow::kUnknownCacheVersion));
    assert(!chatnow::cache_l1_version_matches(chatnow::kUnknownCacheVersion, 7));
    assert(!chatnow::cache_l1_version_matches(7, chatnow::kUnknownCacheVersion));

    auto parsed = chatnow::parse_cache_version("42");
    assert(parsed.has_value() && *parsed == 42);
    assert(chatnow::parse_cache_version("0").value_or(1) == 0);
    assert(!chatnow::parse_cache_version("").has_value());
    assert(!chatnow::parse_cache_version("x").has_value());
    assert(!chatnow::parse_cache_version("42x").has_value());
    assert(!chatnow::parse_cache_version(" 42").has_value());
    assert(!chatnow::parse_cache_version("42 ").has_value());
    assert(!chatnow::parse_cache_version("+42").has_value());
    assert(!chatnow::parse_cache_version("-1").has_value());
    assert(!chatnow::parse_cache_version(
        std::to_string(std::numeric_limits<uint64_t>::max())).has_value());

    assert(chatnow::cache_sentinel_confirms_empty(true, true));
    assert(!chatnow::cache_sentinel_confirms_empty(true, false));
    assert(!chatnow::cache_sentinel_confirms_empty(false, true));
    assert(!chatnow::cache_sentinel_confirms_empty(false, false));

    assert(chatnow::cache_ttl_allows_write(std::chrono::seconds(1)));
    assert(!chatnow::cache_ttl_allows_write(std::chrono::seconds(0)));
    assert(!chatnow::cache_ttl_allows_write(std::chrono::seconds(-1)));

    std::cout << "members cache rules tests passed\n";
    return 0;
}
