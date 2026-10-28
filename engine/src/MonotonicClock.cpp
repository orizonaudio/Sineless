#include "engine/MonotonicClock.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <ctime>
#endif

#if defined(__APPLE__)
// libc++'s steady_clock reads CLOCK_MONOTONIC_RAW (libcxx/src/chrono.cpp). The C library does not
// declare it nonblocking; a redeclaration may add the attribute (clang's FunctionEffectAnalysis).
extern "C" std::uint64_t clock_gettime_nsec_np(clockid_t clock) ORIZON_NONBLOCKING;
#elif !defined(_WIN32)
extern "C" int clock_gettime(clockid_t clock, struct timespec* time) noexcept ORIZON_NONBLOCKING;
#endif

namespace orizon::engine {

#if defined(_WIN32)
namespace {

// Read once, at load, as Microsoft advises ("Acquiring high-resolution time stamps"): never first
// on the audio thread, where a function-local static would take a guard.
const std::int64_t counterFrequency = [] {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return static_cast<std::int64_t>(frequency.QuadPart);
}();

} // namespace

std::uint64_t monotonicNanos() noexcept ORIZON_NONBLOCKING
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const auto ticks = static_cast<std::int64_t>(counter.QuadPart);
    // Whole seconds and the rest apart, so the product does not overflow.
    const std::int64_t seconds = ticks / counterFrequency;
    const std::int64_t rest = ticks % counterFrequency;
    return static_cast<std::uint64_t>(seconds) * 1'000'000'000U +
           static_cast<std::uint64_t>(rest * 1'000'000'000 / counterFrequency);
}

#elif defined(__APPLE__)

std::uint64_t monotonicNanos() noexcept ORIZON_NONBLOCKING
{
    return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
}

#else

std::uint64_t monotonicNanos() noexcept ORIZON_NONBLOCKING
{
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000U +
           static_cast<std::uint64_t>(time.tv_nsec);
}

#endif

} // namespace orizon::engine
