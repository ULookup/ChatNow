#include "dao/data_redis.hpp"

#include <cassert>
#include <iostream>

int main() {
    using chatnow::GenerationWriteResult;
    using chatnow::UserInfoL1Publication;

    assert(chatnow::user_info_l1_publication(GenerationWriteResult::Committed) ==
           UserInfoL1Publication::GenerationFenced);
    assert(chatnow::user_info_l1_publication(GenerationWriteResult::Conflict) ==
           UserInfoL1Publication::Denied);
    assert(chatnow::user_info_l1_publication(GenerationWriteResult::Unavailable) ==
           UserInfoL1Publication::ShortLivedFallback);

    // A generation-aware fill must never collapse a real generation conflict
    // into the Redis-unavailable fallback path.
    assert(chatnow::user_info_l1_publication(GenerationWriteResult::Conflict) !=
           chatnow::user_info_l1_publication(GenerationWriteResult::Unavailable));

    std::cout << "user info generation fence policy tests passed\n";
    return 0;
}
