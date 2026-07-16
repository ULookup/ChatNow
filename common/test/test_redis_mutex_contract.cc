#include "utils/redis_mutex.hpp"

#include <type_traits>
#include <iostream>

int main() {
    static_assert(!std::is_copy_constructible<chatnow::RedisMutex>::value,
                  "RedisMutex must not be copy constructible");
    static_assert(!std::is_copy_assignable<chatnow::RedisMutex>::value,
                  "RedisMutex must not be copy assignable");
    static_assert(!std::is_move_constructible<chatnow::RedisMutex>::value,
                  "RedisMutex must not be move constructible");
    static_assert(!std::is_move_assignable<chatnow::RedisMutex>::value,
                  "RedisMutex must not be move assignable");

    std::cout << "redis mutex contract tests passed\n";
    return 0;
}
