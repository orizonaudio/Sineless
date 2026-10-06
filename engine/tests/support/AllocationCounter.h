#pragma once

#include <cstddef>

namespace orizon::testing {

// Counts global operator new/delete calls made on the current thread while an instance is alive.
// Linking this library replaces the global allocation operators of the test binary.
class AllocationCounter
{
public:
    AllocationCounter();
    ~AllocationCounter();
    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;

    [[nodiscard]] std::size_t allocations() const;
    [[nodiscard]] std::size_t deallocations() const;
};

} // namespace orizon::testing
