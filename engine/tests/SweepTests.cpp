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
    EXPECT_EQ(window->start, 48000);
    EXPECT_EQ(window->rotation, 48000u) << "sample 96000 moves to the left edge";
    EXPECT_DOUBLE_EQ(window->cursor, 0.5);
    EXPECT_FALSE(window->limited);
    EXPECT_DOUBLE_EQ(window->cycleStart, 4.0) << "bar 2 starts at quarter note 4";
    EXPECT_DOUBLE_EQ(window->spanQuarterNotes, 4.0);
    EXPECT_DOUBLE_EQ(window->barQuarterNotes, 4.0);
}

TEST(Sweep, AlignsTwoBarCyclesToOddBars)
{
    // 3.25 bars: a 2-bar cycle started at bar 3 (sample 192000), 1.25 bars ago.
    const auto window = sweepWindow(clockAt120(), 312000, kRate, BeatSpan::kTwoBars, kUnlimited);
    ASSERT_TRUE(window);
    EXPECT_EQ(window->start + static_cast<std::int64_t>(window->rotation), 192000);
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

TEST(Sweep, StartsBeforeTheCaptureCoversTheSpan)
{
    // Half a bar captured: the window starts before the capture, with the playhead halfway.
    const auto window = sweepWindow(clockAt120(), 48000, kRate, BeatSpan::kOneBar, kUnlimited);
    ASSERT_TRUE(window);
    EXPECT_EQ(window->start, -48000);
    EXPECT_EQ(window->rotation, 48000u) << "the cycle start, sample 0, moves to the left edge";
    EXPECT_DOUBLE_EQ(window->cursor, 0.5);
}

// A bar sweep held at 1.25 bars: its cycle is bar 2, quarter notes 4 to 8.
std::optional<SweepWindow> barImage()
{
    return sweepWindow(clockAt120(), 120000, kRate, BeatSpan::kOneBar, kUnlimited);
}

TEST(FrozenSweep, KeepsTheImageForTheSameSpan)
{
    const auto frozen =
        frozenSweep(clockAt120(), 120000, kRate, BeatSpan::kOneBar, barImage(), 96000);
    ASSERT_TRUE(frozen);
    EXPECT_FALSE(frozen->rebuild);
    EXPECT_DOUBLE_EQ(frozen->start, 0.0);
    EXPECT_DOUBLE_EQ(frozen->end, 1.0);
    EXPECT_EQ(frozen->window.sampleCount, 96000u);
}

TEST(FrozenSweep, CropsAShorterSpanThatDividesTheImage)
{
    // Two beats at 1.25 bars: the cycle of quarter notes 4 to 6, the first half of the bar.
    const auto frozen =
        frozenSweep(clockAt120(), 120000, kRate, BeatSpan::kTwoBeats, barImage(), 96000);
    ASSERT_TRUE(frozen);
    EXPECT_FALSE(frozen->rebuild);
    EXPECT_DOUBLE_EQ(frozen->start, 0.0);
    EXPECT_DOUBLE_EQ(frozen->end, 0.5);
    EXPECT_DOUBLE_EQ(frozen->window.cycleStart, 4.0);
    EXPECT_DOUBLE_EQ(frozen->window.cursor, 0.5);
    EXPECT_EQ(frozen->window.sampleCount, 48000u);
}

TEST(FrozenSweep, RebuildsOtherSpansFromTheHeldAudio)
{
    // Two bars do not fit the image: the whole span, filled as far as the held audio goes.
    const auto longer =
        frozenSweep(clockAt120(), 120000, kRate, BeatSpan::kTwoBars, barImage(), 96000);
    ASSERT_TRUE(longer);
    EXPECT_TRUE(longer->rebuild);
    EXPECT_EQ(longer->window.sampleCount, 192000u);
    EXPECT_FALSE(longer->window.limited);
    EXPECT_EQ(longer->window.start, 120000 - 192000);
    // Two beats do not divide a 3/4 bar: rebuilt too.
    MusicalStamp waltz = clockAt120();
    waltz.numerator = 3;
    const auto waltzBar = sweepWindow(waltz, 120000, kRate, BeatSpan::kOneBar, kUnlimited);
    ASSERT_TRUE(waltzBar);
    const auto odd =
        frozenSweep(waltz, 120000, kRate, BeatSpan::kTwoBeats, waltzBar, waltzBar->sampleCount);
    ASSERT_TRUE(odd);
    EXPECT_TRUE(odd->rebuild);
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
