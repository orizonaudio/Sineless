#include "MacPageTesting.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace orizon::testing {
namespace {

using namespace std::chrono_literals;

std::string_view name(DisplayVerdict verdict)
{
    switch (verdict) {
    case DisplayVerdict::kMet:
        return "met";
    case DisplayVerdict::kFailed:
        return "failed";
    case DisplayVerdict::kSkipped:
        return "skipped";
    }
    return "?";
}

// A wait on the display is skipped only where the display stalled on CI: a failed condition with
// the display running fails everywhere, and a stall on a developer's Mac fails too.
TEST(DisplayWait, SkipsOnlyAStalledDisplayOnCi)
{
    struct Case
    {
        bool met;
        bool displayRan;
        bool ci;
        DisplayVerdict verdict;
    };
    for (const Case& c :
         {Case{.met = true, .displayRan = true, .ci = true, .verdict = DisplayVerdict::kMet},
          Case{.met = true, .displayRan = false, .ci = true, .verdict = DisplayVerdict::kMet},
          Case{.met = true, .displayRan = false, .ci = false, .verdict = DisplayVerdict::kMet},
          Case{.met = false, .displayRan = true, .ci = true, .verdict = DisplayVerdict::kFailed},
          Case{.met = false, .displayRan = true, .ci = false, .verdict = DisplayVerdict::kFailed},
          Case{.met = false, .displayRan = false, .ci = true, .verdict = DisplayVerdict::kSkipped},
          Case{.met = false,
               .displayRan = false,
               .ci = false,
               .verdict = DisplayVerdict::kFailed}}) {
        SCOPED_TRACE(std::string("met ") + (c.met ? "yes" : "no") + ", display ran " +
                     (c.displayRan ? "yes" : "no") + ", CI " + (c.ci ? "yes" : "no"));
        EXPECT_EQ(
            name(displayVerdict({.met = c.met, .displayRan = c.displayRan, .message = {}}, c.ci)),
            name(c.verdict));
    }
}

// The display ran when it gave at least kRanFraction of the refreshes a wait spanned: one callback
// fewer is a stall, none at all is one, and a wait that spanned none cannot have stalled.
TEST(DisplayWait, ReadsAStallFromTheShareOfRefreshesGiven)
{
    const double expected = expectedRefreshes(3s, 60.0);
    const auto atFraction = static_cast<std::uint64_t>(std::ceil(kRanFraction * expected));
    EXPECT_TRUE(displayRan(atFraction, expected));
    EXPECT_FALSE(displayRan(atFraction - 1, expected));
    EXPECT_FALSE(displayRan(0, expected));
    EXPECT_TRUE(displayRan(static_cast<std::uint64_t>(expected), expected));
    EXPECT_TRUE(displayRan(0, 0.0));
    // The failing CI run: under 11 callbacks where 180 were due.
    EXPECT_FALSE(displayRan(10, expected));
}

// A screen that reports no rate (no display attached) is taken as 60 Hz: a wait with no callbacks
// there still reads as a stall, and a link calling back at 60 Hz as running.
TEST(DisplayWait, JudgesAScreenThatReportsNoRateAsSixtyHertz)
{
    EXPECT_FALSE(displayRan(0, expectedRefreshes(1s, 0.0)));
    EXPECT_TRUE(displayRan(60, expectedRefreshes(1s, 0.0)));
}

// A link that gave no callbacks stalled only if the layer meant it to run: one the layer paused
// itself (hidden, occluded, lost) is the layer's doing, and a wait on it fails, on CI too, so a
// layer that never resumes is caught there.
TEST(DisplayWait, ReadsALinkTheLayerPausedAsRunning)
{
    const double expected = expectedRefreshes(1s, 60.0);
    const auto atFraction = static_cast<std::uint64_t>(std::ceil(kRanFraction * expected));
    EXPECT_FALSE(linkRan(0, expected, true));
    EXPECT_FALSE(linkRan(atFraction - 1, expected, true));
    EXPECT_TRUE(linkRan(atFraction, expected, true));
    EXPECT_TRUE(linkRan(0, expected, false));
    EXPECT_EQ(name(displayVerdict(
                  {.met = false, .displayRan = linkRan(0, expected, false), .message = {}}, true)),
              name(DisplayVerdict::kFailed));
}

// The page's probe: a frame that never came is a stall; a read that failed measured nothing and
// fails the test rather than skip it. A wait on both the link and the page stalled when either did.
TEST(DisplayWait, JudgesThePageByItsProbeAndAJointWaitByBoth)
{
    EXPECT_TRUE(pageRan(PageProbe::kFired));
    EXPECT_FALSE(pageRan(PageProbe::kNotFired));
    EXPECT_TRUE(pageRan(PageProbe::kNotMeasured));

    const double expected = expectedRefreshes(1s, 60.0);
    const auto running = static_cast<std::uint64_t>(expected);
    EXPECT_TRUE(linkAndPageRan(running, expected, true, PageProbe::kFired));
    EXPECT_FALSE(linkAndPageRan(running, expected, true, PageProbe::kNotFired));
    EXPECT_FALSE(linkAndPageRan(0, expected, true, PageProbe::kFired));
    EXPECT_FALSE(linkAndPageRan(0, expected, true, PageProbe::kNotFired));
    EXPECT_TRUE(linkAndPageRan(running, expected, true, PageProbe::kNotMeasured));
    EXPECT_TRUE(linkAndPageRan(0, expected, false, PageProbe::kFired));
}

} // namespace
} // namespace orizon::testing
