#pragma once

// For tests that load the editor's page in WKWebView (the VST3 editor's and the Audio Unit view's):
// run the main run loop until a condition holds, and ask the page for its state.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace orizon::testing {

// Long enough for the page to load on a busy machine.
inline constexpr std::chrono::seconds kPageTimeout{15};

// Runs the main run loop until `condition` holds or `timeout` passes; whether it holds.
inline bool pumpUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    return condition();
}

// ----- Waits on the display -----
//
// Some waits are for what only the display makes happen: the trace layer's display link calling
// back, or the page's rendering update (requestAnimationFrame, ResizeObserver). The virtual
// display of a CI machine stops driving both for seconds now and then (no callbacks, no presented
// times), which no host's display does. A wait that times out asks whether the display ran
// meanwhile: if not, it skips on CI, saying so, and fails elsewhere, so a stall on a developer's
// Mac still shows. If the display ran, the test fails as any other.

// The share of the refreshes a wait spanned that shows the display ran: far above a stalled CI
// display's (under 6%: 11 callbacks where 180 were due), and below a link running on time, so
// neither a busy machine nor a screen reporting more than the link delivers reads as a stall.
inline constexpr double kRanFraction = 0.5;

// The refreshes `wait` spans at the screen's reported rate (60 Hz when it reports none).
inline double expectedRefreshes(std::chrono::milliseconds wait, double screenHertz)
{
    const double hertz = screenHertz > 0.0 ? screenHertz : 60.0;
    return static_cast<double>(wait.count()) / 1000.0 * hertz;
}

// Whether `given` callbacks of `expected` show the display ran.
inline bool displayRan(std::uint64_t given, double expected)
{
    return static_cast<double>(given) >= kRanFraction * expected;
}

// Whether the trace layer's display link ran over a wait: it gave at least kRanFraction of the
// refreshes, or the layer did not mean it to run (hidden, occluded, lost). A link the layer itself
// paused is not a stalled display, so a wait on it fails on CI too.
inline bool linkRan(std::uint64_t given, double expected, bool linkRunning)
{
    return !linkRunning || displayRan(given, expected);
}

// What the page's one requestAnimationFrame, asked after a wait timed out, says of its rendering
// updates. A read that failed measured nothing: it fails the test, never skips it.
enum class PageProbe : std::uint8_t
{
    kFired,
    kNotFired,
    kNotMeasured,
};

inline bool pageRan(PageProbe probe)
{
    return probe != PageProbe::kNotFired;
}

// For what needs both the link and the page (frames made on the link's request, reaching the page
// in its rendering update): the display ran only if both did.
inline bool linkAndPageRan(std::uint64_t given, double expected, bool linkRunning, PageProbe probe)
{
    return linkRan(given, expected, linkRunning) && pageRan(probe);
}

// A wait's outcome: whether its condition held, and if not, whether the display drove it.
struct DisplayWait
{
    bool met = false;
    bool displayRan = true;
    std::string message;
};

enum class DisplayVerdict : std::uint8_t
{
    kMet,
    kFailed,
    kSkipped,
};

// Met; else skipped where the display stalled on CI; else failed.
inline DisplayVerdict displayVerdict(const DisplayWait& wait, bool ci)
{
    if (wait.met) {
        return DisplayVerdict::kMet;
    }
    return !wait.displayRan && ci ? DisplayVerdict::kSkipped : DisplayVerdict::kFailed;
}

// Whether the tests run on CI (GitHub sets CI on every job).
inline bool onCi()
{
    const char* value = std::getenv("CI");
    return value != nullptr && *value != '\0' && std::string(value) != "false";
}

// Asserts a wait on the display in a test body: skips the test where the display stalled on CI.
#define ASSERT_DISPLAY_WAIT(wait)                                                                  \
    do {                                                                                           \
        const ::orizon::testing::DisplayWait displayWait_ = (wait);                                \
        switch (::orizon::testing::displayVerdict(displayWait_, ::orizon::testing::onCi())) {      \
        case ::orizon::testing::DisplayVerdict::kMet:                                              \
            break;                                                                                 \
        case ::orizon::testing::DisplayVerdict::kSkipped:                                          \
            GTEST_SKIP() << displayWait_.message;                                                  \
        case ::orizon::testing::DisplayVerdict::kFailed:                                           \
            FAIL() << displayWait_.message;                                                        \
        }                                                                                          \
    } while (false)

// Evaluates `script` in the page and returns its result as a string ("" if none yet).
inline std::string evaluate(WKWebView* webView, NSString* script)
{
    struct Result
    {
        bool done = false;
        std::string value;
    };
    auto result = std::make_shared<Result>();
    [webView evaluateJavaScript:script
              completionHandler:^(id value, NSError* /*error*/) {
                if ([value isKindOfClass:[NSString class]]) {
                    result->value = [value UTF8String];
                }
                result->done = true;
              }];
    pumpUntil([&] { return result->done; }, std::chrono::seconds(5));
    return result->value;
}

// Whether the page has drawn a frame from the engine.
inline bool hasSnapshot(WKWebView* webView)
{
    return evaluate(webView, @"String(window.orizonDebug?.snapshot?.type ?? '')") == "snapshot";
}

// Whether any raw page message has the given type.
inline bool saw(const std::vector<std::string>& messages, const std::string& type)
{
    return std::ranges::any_of(messages, [&](const std::string& message) {
        return message.find(R"("type":")" + type + "\"") != std::string::npos;
    });
}

} // namespace orizon::testing
