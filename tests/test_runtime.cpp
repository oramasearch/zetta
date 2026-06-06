#include <gtest/gtest.h>

#include <memory>

#include "zetta/runtime.hpp"

namespace {

zetta::runtime::task<int> inner() { co_return 20; }

// co_await composes through the seam; result flows back out of block_on.
zetta::runtime::task<int> outer() {
    const int partial = co_await inner();
    co_return partial + 22;
}

// Take state by value (shared_ptr) into the frame — never by reference (the
// coroutine dangling-parameter footgun).
zetta::runtime::task<> set_flag(std::shared_ptr<bool> flag) {
    *flag = true;
    co_return;
}

}  // namespace

TEST(Runtime, BlockOnReturnsValueAndAwaitComposes) {
    EXPECT_EQ(zetta::runtime::block_on(outer()), 42);
}

TEST(Runtime, BlockOnRunsVoidTask) {
    auto flag = std::make_shared<bool>(false);
    zetta::runtime::block_on(set_flag(flag));
    EXPECT_TRUE(*flag);
}
