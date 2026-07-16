#include "dao/data_redis.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    using chatnow::GenerationWriteResult;
    using chatnow::UserInfoL1Publication;

    int failures = 0;
    auto expect = [&](bool condition, const char *message) {
        if (condition) return;
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    };

    chatnow::UserInfoCache missing_client(nullptr);
    expect(missing_client.set_if_generation("u1", "bytes", 0) ==
               GenerationWriteResult::Unavailable,
           "UserInfoCache must report a missing client as unavailable");
    chatnow::UserInfoCache lua_zero(nullptr, [] { return 0LL; });
    expect(lua_zero.set_if_generation("u1", "bytes", 0) ==
               GenerationWriteResult::Conflict,
           "Lua zero must be a generation conflict");
    chatnow::UserInfoCache lua_one(nullptr, [] { return 1LL; });
    expect(lua_one.set_if_generation("u1", "bytes", 0) ==
               GenerationWriteResult::Committed,
           "Lua one must be committed");
    chatnow::UserInfoCache circuit_open(nullptr, []() -> long long {
        throw chatnow::RedisCircuitOpen();
    });
    expect(circuit_open.set_if_generation("u1", "bytes", 0) ==
               GenerationWriteResult::Unavailable,
           "open Redis circuit must be unavailable");
    chatnow::UserInfoCache redis_error(nullptr, []() -> long long {
        throw std::runtime_error("redis unavailable");
    });
    expect(redis_error.set_if_generation("u1", "bytes", 0) ==
               GenerationWriteResult::Unavailable,
           "Redis exception must be unavailable");

    auto observe_publication = [&](GenerationWriteResult result) {
        int calls = 0;
        UserInfoL1Publication observed = UserInfoL1Publication::Denied;
        chatnow::publish_user_info_l1(result, [&](UserInfoL1Publication publication) {
            ++calls;
            observed = publication;
        });
        return std::pair<int, UserInfoL1Publication>{calls, observed};
    };
    const auto committed = observe_publication(GenerationWriteResult::Committed);
    expect(committed.first == 1 &&
               committed.second == UserInfoL1Publication::GenerationFenced,
           "committed write must call the generation-fenced L1 publisher");
    const auto conflict = observe_publication(GenerationWriteResult::Conflict);
    expect(conflict.first == 0,
           "generation conflict must not call the L1 publisher");
    const auto unavailable = observe_publication(GenerationWriteResult::Unavailable);
    expect(unavailable.first == 1 &&
               unavailable.second == UserInfoL1Publication::ShortLivedFallback,
           "unavailable Redis must call only the short-lived L1 fallback");

    if (failures == 0) {
        std::cout << "user info generation fence execution tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
