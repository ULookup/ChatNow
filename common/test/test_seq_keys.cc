#include "utils/redis_keys.hpp"

#include <cassert>
#include <iostream>
#include <string>

static std::string hash_tag(const std::string &key) {
    auto l = key.find('{');
    auto r = key.find('}', l == std::string::npos ? 0 : l + 1);
    if (l == std::string::npos || r == std::string::npos || r <= l + 1) return "";
    return key.substr(l + 1, r - l - 1);
}

int main() {
    auto s1 = chatnow::key::seq_session_key("conv-a");
    auto s2 = chatnow::key::seq_session_key("conv-b");
    auto u1 = chatnow::key::seq_user_key("user-a");
    auto u2 = chatnow::key::seq_user_key("user-b");

    assert(s1 == "im:seq:ssid:{conv-a}");
    assert(u1 == "im:seq:uid:{user-a}");
    assert(hash_tag(s1) == "conv-a");
    assert(hash_tag(u1) == "user-a");
    assert(hash_tag(s1) != hash_tag(s2));
    assert(hash_tag(u1) != hash_tag(u2));
    assert(s1.find("{seq}") == std::string::npos);
    assert(u1.find("{seq}") == std::string::npos);

    std::cout << "seq key tests passed\n";
    return 0;
}
