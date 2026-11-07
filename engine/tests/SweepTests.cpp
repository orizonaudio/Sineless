#include "SampleRates.h"
#include "engine/Sweep.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <utility>

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
    // Every span is in bars of the host time signature.
    EXPECT_DOUBLE_EQ(beatSpanQuarterNotes(BeatSpan::kEighthBar, 4.0), 0.5);
    EXPECT_DOUBLE_EQ(beatSpanQuarterNotes(BeatSpan::kThirdBar, 4.0), 4.0 / 3);
    EXPECT_DOUBLE_EQ(beatSpanQuarterNotes(BeatSpan::kThirdBar, 3.0), 1.0) << "3/4";
    EXPECT_DOUBLE_EQ(beatSpanQuarterNotes(BeatSpan::kHalfBar, 4.0), 2.0);
    EXPECT_DOUBLE_EQ(beatSpanQuarterNotes(BeatSpan::kSixBars, 4.0), 24.0);
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

// Even the shortest span, 1/8 bar, can outgrow the capture in a long bar at a slow tempo (16/1
// below about 22 BPM at 384 kHz): the window then holds exactly what the capture can, its musical
// span the same length, so the sweep never asks for audio it does not keep. Just above that tempo
// it is the 1/8 bar it was asked for. At every rate, in bars of 4/4, 16/4 and 16/1.
TEST(Sweep, ShortensTheShortestSpanToTheCapture)
{
    constexpr std::uint64_t kCapture = std::uint64_t{1} << 23;
    for (const double rate : kSampleRates) {
        for (const auto& [numerator, denominator] :
             {std::pair{4, 4}, std::pair{16, 4}, std::pair{16, 1}}) {
            const double bar = 4.0 * numerator / denominator;
            const double eighth = beatSpanQuarterNotes(BeatSpan::kEighthBar, bar);
            // The tempo at which 1/8 bar takes the whole capture.
            const double fits = eighth * 60.0 * rate / static_cast<double>(kCapture);
            for (const double tempo : {fits * 0.99, fits * 1.01}) {
                const MusicalStamp stamp{.sampleIndex = 0,
                                         .quarterNotes = 0.0,
                                         .tempo = tempo,
                                         .numerator = numerator,
                                         .denominator = denominator,
                                         .playing = true};
                const auto window =
                    sweepWindow(stamp, 3 * kCapture, rate, BeatSpan::kEighthBar, kCapture);
                ASSERT_TRUE(window);
                const double samplesPerQuarterNote = 60.0 * rate / tempo;
                const bool over = tempo < fits;
                SCOPED_TRACE(::testing::Message() << rate << " Hz, " << numerator << "/"
                                                  << denominator << ", " << tempo << " BPM");
                EXPECT_EQ(window->limited, over);
                if (over) {
                    EXPECT_EQ(window->sampleCount, kCapture);
                    EXPECT_NEAR(window->spanQuarterNotes * samplesPerQuarterNote,
                                static_cast<double>(kCapture), 1.0);
                } else {
                    EXPECT_LT(window->sampleCount, kCapture);
                    EXPECT_DOUBLE_EQ(window->spanQuarterNotes, eighth);
                }
                EXPECT_GT(window->cursor, 0.0);
                EXPECT_LE(window->cursor, 1.0);
            }
        }
    }
}

TEST(Sweep, TellsSimilarLengthsApartFromChangedOnes)
{
    EXPECT_TRUE(sweepLengthsSimilar(2000, 2000));
    EXPECT_TRUE(sweepLengthsSimilar(2000, 2001));
    EXPECT_TRUE(sweepLengthsSimilar(1024000, 1025000));
    EXPECT_FALSE(sweepLengthsSimilar(2000, 2010));
    EXPECT_FALSE(sweepLengthsSimilar(500, 501)) << "short sweeps allow no drift";
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
        frozenSweep(clockAt120(), 120000, kRate, BeatSpan::kHalfBar, barImage(), 96000);
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
    // Two bars are shorter than three but do not divide them: rebuilt too.
    const auto threeBars =
        sweepWindow(clockAt120(), 120000, kRate, BeatSpan::kThreeBars, kUnlimited);
    ASSERT_TRUE(threeBars);
    const auto odd = frozenSweep(clockAt120(), 120000, kRate, BeatSpan::kTwoBars, threeBars,
                                 threeBars->sampleCount);
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
