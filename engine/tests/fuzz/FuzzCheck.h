#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace orizon::fuzz {

// Stops the run like a crash when an invariant fails, so that libFuzzer saves the input.
inline void require(bool condition, const char* invariant)
{
    if (!condition) {
        std::fprintf(stderr, "Fuzz invariant failed: %s\n", invariant);
        std::abort();
    }
}

// How many times each oracle has checked a frame, by name. An oracle checks only when its
// conditions hold, so a seed meant for one can be required to reach it (FuzzReplay.cpp's
// --require-oracle): a change that stops the conditions from ever holding fails the replay.
inline std::map<std::string, std::size_t, std::less<>>& oracleChecks()
{
    static std::map<std::string, std::size_t, std::less<>> checks;
    return checks;
}

inline void counted(std::string_view oracle)
{
    ++oracleChecks().try_emplace(std::string(oracle), 0).first->second;
}

} // namespace orizon::fuzz
