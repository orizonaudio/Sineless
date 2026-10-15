#include "AllocationCounter.h"

#include <array>
#include <gtest/gtest.h>
#include <memory>

namespace orizon::testing {
namespace {

TEST(AllocationCounter, CountsAllocationsAndDeallocationsInScope)
{
    if (!kCountsAllocations) {
        GTEST_SKIP() << "ThreadSanitizer's build counts no allocations (AllocationCounter.h)";
    }
    std::size_t allocations = 0;
    std::size_t deallocations = 0;
    {
        AllocationCounter counter;
        // Plain allocations: library containers may add their own (MSVC's Debug vector allocates
        // an iterator-checking proxy).
        auto value = std::make_unique<int>(1);
        auto values = std::make_unique<std::array<int, 16>>();
        value.reset();
        allocations = counter.allocations();
        deallocations = counter.deallocations();
    }
    EXPECT_EQ(allocations, 2u);
    EXPECT_EQ(deallocations, 1u);
}

TEST(AllocationCounter, IgnoresAllocationsOutsideScope)
{
    if (!kCountsAllocations) {
        GTEST_SKIP() << "ThreadSanitizer's build counts no allocations (AllocationCounter.h)";
    }
    std::size_t allocations = 0;
    {
        AllocationCounter counter;
        allocations = counter.allocations();
    }
    auto value = std::make_unique<int>(1);
    EXPECT_EQ(allocations, 0u);
}

} // namespace
} // namespace orizon::testing
