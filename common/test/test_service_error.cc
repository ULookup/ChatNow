#include "error/service_error.hpp"
#include <gtest/gtest.h>

TEST(ServiceError, ConstructAndAccess) {
    chatnow::ServiceError e(1001, "bad credentials");
    EXPECT_EQ(e.code(), 1001);
    EXPECT_EQ(e.message(), "bad credentials");
    EXPECT_STREQ(e.what(), "bad credentials");
}

TEST(ServiceError, ThrowAndCatch) {
    try {
        throw chatnow::ServiceError(9001, "internal");
        FAIL() << "expected throw";
    } catch (const chatnow::ServiceError& e) {
        EXPECT_EQ(e.code(), 9001);
        EXPECT_EQ(e.message(), "internal");
    } catch (...) {
        FAIL() << "wrong exception type";
    }
}

TEST(ServiceError, MoveSemantics) {
    std::string msg(1024, 'x');
    chatnow::ServiceError e(4001, std::move(msg));
    EXPECT_EQ(e.message().size(), 1024u);
}
