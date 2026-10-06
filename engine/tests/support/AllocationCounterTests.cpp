#include "AllocationCounter.h"

#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace orizon::testing {
namespace {

TEST(AllocationCounter, CountsAllocationsAndDeallocationsInScope)
{
    std::size_t allocations = 0;
    std::size_t deallocations = 0;
    {
        AllocationCounter counter;
        auto value = std::make_unique<int>(1);
        std::vector<int> values(16);
        value.reset();
        allocations = counter.allocations();
        deallocations = counter.deallocations();
    }
    EXPECT_EQ(allocations, 2u);
    EXPECT_EQ(deallocations, 1u);
}

TEST(AllocationCounter, IgnoresAllocationsOutsideScope)
{
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
