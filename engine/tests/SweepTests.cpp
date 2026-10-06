#include "engine/Sweep.h"

#include <gtest/gtest.h>

namespace orizon::engine {
namespace {

using protocol::BeatSpan;

constexpr double kRate = 48000.0;
constexpr std::uint64_t kUnlimited = 1U << 30U;

// 120 BPM in 4/4 from the project start: 24000 samples per quarter note, 96000 per bar.
MusicalStamp clockAt120()
{
    return {.sampleIndex = 0, .quarterNotes = 0.0, .tempo = 120.0, .playing = true};
}

TEST(Sweep, MeasuresSpansInQuarterNotesAndBars)
{
    EXPECT_DOUBLE_EQ(beatSpanQuarterNotes(BeatSpan::kSixteenthBeat, 4.0), 1.0 / 16);
    EXPECT_DOUBLE_EQ(beatSpanQuarterNotes(BeatSpan::kTwoBeats, 4.0), 2.0);
    EXPECT_DOUBLE_EQ(beatSpanQuarterNotes(BeatSpan::kTwoBars, 4.0), 8.0);
    EXPECT_DOUBLE_EQ(beatSpanQuarterNotes(BeatSpan::kOneBar, 3.5), 3.5) << "7/8";
}

TEST(Sweep, PutsTheCycleStartAtTheLeftEdge)
{
    // Playhead at 1.5 bars into a 1-bar cycle: the cycle started at bar 2 (sample 96000).
    const auto window = sweepWindow(clockAt120(), 144000, kRate, BeatSpan::kOneBar, kUnlimited);
    ASSERT_TRUE(window);
    EXPECT_EQ(window->sampleCount, 96000u);
    EXPECT_EQ(window->start, 48000u);
    EXPECT_EQ(window->rotation, 48000u) << "sample 96000 moves to the left edge";
    EXPECT_DOUBLE_EQ(window->cursor, 0.5);
    EXPECT_FALSE(window->limited);
}

TEST(Sweep, AlignsTwoBarCyclesToOddBars)
{
    // 3.25 bars: a 2-bar cycle started at bar 3 (sample 192000), 1.25 bars ago.
    const auto window = sweepWindow(clockAt120(), 312000, kRate, BeatSpan::kTwoBars, kUnlimited);
    ASSERT_TRUE(window);
    EXPECT_EQ(window->start + window->rotation, 192000u);
    EXPECT_DOUBLE_EQ(window->cursor, 0.625);
}

TEST(Sweep, ShortensSpansThatDoNotFit)
{
    // Four bars at 120 BPM need 384000 samples; only 200000 fit, so two bars (192000) are used.
    const auto window = sweepWindow(clockAt120(), 500000, kRate, BeatSpan::kFourBars, 200000);
    ASSERT_TRUE(window);
    EXPECT_TRUE(window->limited);
    EXPECT_EQ(window->sampleCount, 192000u);
}

TEST(Sweep, WaitsUntilTheSpanIsCaptured)
{
    EXPECT_FALSE(sweepWindow(clockAt120(), 50000, kRate, BeatSpan::kOneBar, kUnlimited));
}

TEST(Sweep, FollowsTheClockFromAnyStamp)
{
    // The same musical position expressed from a later stamp gives the same window.
    const MusicalStamp later{.sampleIndex = 120000, .quarterNotes = 5.0, .tempo = 120.0};
    const auto fromStart = sweepWindow(clockAt120(), 144000, kRate, BeatSpan::kOneBar, kUnlimited);
    const auto fromLater = sweepWindow(later, 144000, kRate, BeatSpan::kOneBar, kUnlimited);
    ASSERT_TRUE(fromStart && fromLater);
    EXPECT_EQ(fromStart->rotation, fromLater->rotation);
}

} // namespace
} // namespace orizon::engine
