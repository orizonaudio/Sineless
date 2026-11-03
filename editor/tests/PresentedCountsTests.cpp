#include "editor/PresentedCounts.h"

#include <gtest/gtest.h>
#include <vector>

namespace orizon::editor {
namespace {

// Presented times one display period apart, at `rate`, with every `missEvery`-th refresh showing
// no new frame (0: none missed).
PresentedCounts::Totals steady(double rate, int frames, int missEvery = 0)
{
    PresentedCounts counts;
    const double period = 1.0 / rate;
    double time = 1000.0;
    for (int frame = 0; frame < frames; ++frame) {
        counts.add(time, period);
        time += period;
        if (missEvery > 0 && frame % missEvery == missEvery - 1) {
            time += period; // A refresh that showed the frame before again.
        }
    }
    return counts.totals();
}

// Every refresh a new frame: nothing held, at each rate.
TEST(PresentedCounts, CountsASteadyRunAsNothingHeld)
{
    for (const double rate : {50.0, 60.0, 120.0}) {
        const auto totals = steady(rate, 600);
        EXPECT_EQ(totals.presented, 600U) << rate;
        EXPECT_EQ(totals.dropped, 0U) << rate;
        EXPECT_EQ(totals.refreshesWithoutNewFrame, 0U) << rate;
    }
}

// A refresh that showed the previous frame again is counted, at each rate.
TEST(PresentedCounts, CountsEachRefreshWithoutANewFrame)
{
    for (const double rate : {50.0, 60.0, 120.0}) {
        // Every tenth frame is held a refresh: 59 times, the last with no frame after it.
        const auto totals = steady(rate, 600, 10);
        EXPECT_EQ(totals.refreshesWithoutNewFrame, 59U) << rate;
    }
}

// An interval is rounded to whole refreshes, half up: a late timestamp within half a period is
// still the next refresh, beyond it the one after.
TEST(PresentedCounts, RoundsIntervalsToWholeRefreshes)
{
    const double period = 1.0 / 60.0;
    PresentedCounts below;
    below.add(10.0, period);
    below.add(10.0 + 1.49 * period, period);
    EXPECT_EQ(below.totals().refreshesWithoutNewFrame, 0U);
    PresentedCounts above;
    above.add(10.0, period);
    above.add(10.0 + 1.51 * period, period);
    EXPECT_EQ(above.totals().refreshesWithoutNewFrame, 1U);
}

// A dropped drawable (presented time 0.0) is counted dropped and is no time: the interval runs on
// from the last frame shown.
TEST(PresentedCounts, CountsADroppedDrawableWithoutEndingTheInterval)
{
    const double period = 1.0 / 60.0;
    PresentedCounts counts;
    counts.add(10.0, period);
    counts.add(0.0, period);
    counts.add(10.0 + 2.0 * period, period);
    const auto totals = counts.totals();
    EXPECT_EQ(totals.presented, 2U);
    EXPECT_EQ(totals.dropped, 1U);
    EXPECT_EQ(totals.refreshesWithoutNewFrame, 1U);
}

// The first frame starts the count; until the display's period is known no interval is judged.
TEST(PresentedCounts, JudgesNoIntervalBeforeThereIsOneOrAPeriod)
{
    PresentedCounts counts;
    counts.add(10.0, 1.0 / 60.0);
    EXPECT_EQ(counts.totals().refreshesWithoutNewFrame, 0U);
    PresentedCounts unknown;
    unknown.add(10.0, 0.0);
    unknown.add(10.5, 0.0);
    EXPECT_EQ(unknown.totals().presented, 2U);
    EXPECT_EQ(unknown.totals().refreshesWithoutNewFrame, 0U);
}

// A period that changes (a window moved to a display of another rate) is used from the frame it
// comes with.
TEST(PresentedCounts, FollowsAChangingPeriod)
{
    PresentedCounts counts;
    double time = 10.0;
    for (int frame = 0; frame < 60; ++frame) {
        counts.add(time, 1.0 / 60.0);
        time += 1.0 / 60.0;
    }
    // The first frame at the new rate, one of its refreshes after the last at the old.
    time += 1.0 / 120.0 - 1.0 / 60.0;
    for (int frame = 0; frame < 120; ++frame) {
        counts.add(time, 1.0 / 120.0);
        time += 1.0 / 120.0;
    }
    EXPECT_EQ(counts.totals().refreshesWithoutNewFrame, 0U);
}

} // namespace
} // namespace orizon::editor
