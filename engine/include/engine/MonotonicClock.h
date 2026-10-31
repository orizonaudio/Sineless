#pragma once

#include "engine/RealtimeSafety.h"

#include <cstdint>

namespace orizon::engine {

// Nanoseconds on the platform's monotonic clock: the clock std::chrono::steady_clock reads, from
// an unspecified start. A read, no system call where the platform allows: safe on the audio thread,
// where std::chrono::steady_clock::now(), which is not declared nonblocking, cannot be called.
[[nodiscard]] std::uint64_t monotonicNanos() noexcept ORIZON_NONBLOCKING;

} // namespace orizon::engine
