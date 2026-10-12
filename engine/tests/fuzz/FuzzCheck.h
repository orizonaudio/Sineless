#pragma once

#include <cstdio>
#include <cstdlib>

namespace orizon::fuzz {

// Stops the run like a crash when an invariant fails, so that libFuzzer saves the input.
inline void require(bool condition, const char* invariant)
{
    if (!condition) {
        std::fprintf(stderr, "Fuzz invariant failed: %s\n", invariant);
        std::abort();
    }
}

} // namespace orizon::fuzz
