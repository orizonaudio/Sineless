#pragma once

#include <cstddef>

// ThreadSanitizer's runtime defines the global allocation operators itself, linked whole and not
// replaceable on Linux, so a build under it counts nothing: kCountsAllocations is false there, and
// tests skip what they would check with a count. Allocation-free code is still checked by the
// other builds, and the audio thread's by RealtimeSanitizer.
#if defined(__SANITIZE_THREAD__)
#define ORIZON_UNDER_THREAD_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ORIZON_UNDER_THREAD_SANITIZER 1
#endif
#endif

namespace orizon::testing {

#ifdef ORIZON_UNDER_THREAD_SANITIZER
inline constexpr bool kCountsAllocations = false;
#else
inline constexpr bool kCountsAllocations = true;
#endif

// Counts global operator new/delete calls made on the current thread while an instance is alive.
// Linking this library replaces the global allocation operators of the test binary (unless
// kCountsAllocations is false).
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
