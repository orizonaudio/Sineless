#pragma once

// What golden files written by the tests share: the tolerance their numbers are compared within,
// the form they are written in, and the variable that writes them again.

#include <cstdlib>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>
#include <string_view>

namespace orizon::engine {

// Approved numbers are compared within kRelativeTolerance of their size, or kAbsoluteTolerance,
// whichever is larger. CI runs on Linux and macOS clang (x64 and arm64) and Windows MSVC (x64): the
// engine is built without fast-math, so they differ only where one compiler fuses a
// multiply-add (arm64) and another does not, or a libm rounds a sine the other way, each a float
// ulp (6e-8 of the value) at a time. The longest float sums behind a frame (a correlation over a
// 24000-sample sweep, in eight lanes) round by about 1e-6 of their result at worst, so 1e-4
// leaves a hundred times that, and is still far tighter than any change a reader could see:
// 0.001 dB on a level, 1e-4 on a correlation. The absolute part covers near-zero values of any
// kind (a level, a correlation, a gain, a point of a trace), such as the chance correlation of
// unrelated noise, where a relative tolerance would allow nothing: 1e-6 is far below the smallest
// change a reader could see in any of them, and far above the rounding seen. Built with GCC at
// -O3 for an FMA-capable x64, the corpus differs from Clang's only in such near-zero
// correlations, by 3e-9.
constexpr double kRelativeTolerance = 1e-4;
constexpr double kAbsoluteTolerance = 1e-6;

// Sorted keys (nlohmann::json keeps objects sorted), two-space indents, LF line endings and a
// final newline, and exponents without a plus or leading zeros (1.2e-7, not 1.2e-07): as Prettier
// writes JSON, so `npm run format:check` passes on files the tests write.
inline std::string serialise(const nlohmann::json& value)
{
    static const std::regex kExponent(R"((\d)e\+?(-?)0*(\d))");
    return std::regex_replace(value.dump(2), kExponent, "$1e$2$3") + "\n";
}

// Whether the environment variable `name` is "1": the tests then write their golden files from
// what the code produces now, instead of comparing with them.
inline bool updating(const char* name)
{
#ifdef _MSC_VER
    // MSVC deprecates std::getenv (C4996, an error at /W4 /WX); _dupenv_s is its replacement.
    char* value = nullptr;
    std::size_t length = 0;
    const bool set = _dupenv_s(&value, &length, name) == 0 && value != nullptr;
    const bool update = set && std::string_view(value) == "1";
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s allocates with malloc.
    return update;
#else
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    return value != nullptr && std::string_view(value) == "1";
#endif
}

} // namespace orizon::engine
