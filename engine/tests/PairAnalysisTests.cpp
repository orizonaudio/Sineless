#include "engine/PairAnalysis.h"
#include "protocol/Protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <random>
#include <span>
#include <vector>

namespace orizon::engine {
namespace {

constexpr std::size_t kLength = 24000;
constexpr std::size_t kMaxOffset = 480; // 10 ms at 48 kHz.
constexpr std::size_t kBlock = 480;

std::vector<float> noise(unsigned seed, std::size_t length = kLength)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> uniform(-0.5f, 0.5f);
    std::vector<float> values(length);
    for (float& value : values) {
        value = uniform(random);
    }
    return values;
}

// `values` delayed by `delay` samples: the same signal, arriving later.
std::vector<float> delayed(const std::vector<float>& values, std::size_t delay)
{
    std::vector<float> out(values.size(), 0.0f);
    for (std::size_t i = delay; i < values.size(); ++i) {
        out[i] = values[i - delay];
    }
    return out;
}

PairAnalysis analyse(const std::vector<float>& a, const std::vector<float>& b)
{
    return analysePair(a, b, kMaxOffset, kBlock);
}

// --- A search spread over frames -------------------------------------------------------------

// A track and itself correlate fully, whatever their length: no part of either is left out of
// the energies the correlation is normalised by, including a last few samples that do not fill a
// vector's lanes. Lengths of 22 samples (0.5 ms at 44.1 kHz), 959 and 1001.
TEST(PairAnalysis, CorrelatesATrackFullyWithItselfWhateverItsLength)
{
    for (const std::size_t length : {std::size_t{22}, std::size_t{959}, std::size_t{1001}}) {
        const auto track = noise(37, length);
        const PairAnalysis analysis = analysePair(track, track, length / 4, kBlock);
        EXPECT_NEAR(analysis.correlation, 1.0, 1e-6) << length << " samples";
        EXPECT_EQ(analysis.offset, 0) << length << " samples";
    }
}

// A search stepped a little at a time finds what one done at once does, whatever the signals: a
// late copy, an inverted one, unrelated tracks, silence, a track not finite in places.
TEST(PairSearch, FindsWhatOneSearchAtOnceFinds)
{
    auto broken = delayed(noise(31), 50);
    broken[1000] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> inverted = delayed(noise(32), 20);
    for (float& value : inverted) {
        value = -value;
    }
    const std::vector<std::pair<std::vector<float>, std::vector<float>>> pairs{
        {noise(30), delayed(noise(30), 200)},
        {noise(32), inverted},
        {noise(33), noise(34)},
        {std::vector<float>(kLength, 0.0f), noise(35)},
        {noise(31), broken},
        {noise(36), delayed(noise(36), kMaxOffset)}, // Late by the reach: its last offset.
        {delayed(noise(38), 100), noise(38)},        // Leading this track.
    };
    for (const auto& [a, b] : pairs) {
        const PairAnalysis atOnce = analysePair(a, b, kMaxOffset, kBlock);
        for (const std::size_t budget : {std::size_t{1}, std::size_t{5000}, kLength * 4}) {
            PairSearch search;
            search.start(a, b, kMaxOffset, kBlock);
            int steps = 0;
            while (!search.step(budget) && steps < 10000) {
                ++steps;
            }
            const PairAnalysis spread = search.result();
            EXPECT_EQ(spread.measured, atOnce.measured) << "budget " << budget;
            EXPECT_EQ(spread.related, atOnce.related) << "budget " << budget;
            EXPECT_EQ(spread.offset, atOnce.offset) << "budget " << budget;
            EXPECT_EQ(spread.inverted, atOnce.inverted) << "budget " << budget;
            EXPECT_EQ(spread.correlation, atOnce.correlation) << "budget " << budget;
        }
    }
}

// Each step stops once its budget is spent, but always makes progress: with a budget below one
// offset's work, each step searches one offset, at most the whole stretch, and the steps end
// with the match.
TEST(PairSearch, DoesAtLeastOneOffsetAndNoMoreThanItsBudgetAllows)
{
    const auto a = noise(40);
    PairSearch search;
    search.start(a, delayed(a, 100), kMaxOffset, kBlock);
    bool ended = false;
    while (!ended) {
        const std::uint64_t before = search.work();
        ended = search.step(1);
        const std::uint64_t work = search.work() - before;
        if (!ended) {
            ASSERT_GT(work, 0U) << "a step made no progress";
        }
        ASSERT_LE(work, kLength) << "a step did more than one offset's work";
    }
    EXPECT_EQ(search.result().offset, 100);
}

// A long search takes several steps of a moderate budget, rather than one.
TEST(PairSearch, SpreadsALongSearchOverSteps)
{
    const auto a = noise(41);
    PairSearch search;
    search.start(a, delayed(a, 100), kMaxOffset, kBlock);
    int steps = 1;
    while (!search.step(kLength * 8)) {
        ++steps;
    }
    EXPECT_GT(steps, 2);
    EXPECT_EQ(search.result().offset, 100);
}

// Where a track is not finite (a broken signal upstream), the summing gain is measured around it,
// in every track alike: the gain of tracks with runs of NaN or infinity in each of them, all far
// above full scale, is the gain of the same tracks silenced there, as if those positions were not
// played.
TEST(PairAnalysis, MeasuresTheSummingGainAroundBrokenSamples)
{
    std::vector<std::vector<float>> broken{noise(71), noise(71), noise(72), noise(73)};
    std::vector<std::vector<float>> silenced = broken;
    const auto breakRun = [&](std::size_t track, std::size_t from, float value) {
        for (std::size_t i = from; i < from + 3000; ++i) {
            broken[track][i] = value;
            for (std::vector<float>& samples : silenced) {
                samples[i] = 0.0f;
            }
        }
    };
    breakRun(1, 2000, std::numeric_limits<float>::quiet_NaN());
    breakRun(2, 9000, std::numeric_limits<float>::infinity());
    breakRun(0, 15000, std::numeric_limits<float>::quiet_NaN()); // Every track broken somewhere.
    breakRun(3, 19000, std::numeric_limits<float>::quiet_NaN());
    // Far above full scale, as a float host allows: loud, not broken.
    for (auto* tracks : {&broken, &silenced}) {
        for (std::vector<float>& track : *tracks) {
            for (float& value : track) {
                value *= 100.0f;
            }
        }
    }
    const std::vector<std::span<const float>> brokenTracks(broken.begin(), broken.end());
    const std::vector<std::span<const float>> silencedTracks(silenced.begin(), silenced.end());
    const double expected = overlapGain(silencedTracks, kBlock);
    ASSERT_TRUE(std::isfinite(expected));
    EXPECT_NEAR(overlapGain(brokenTracks, kBlock), expected, 1e-9);
}

// A gain measured in steps, by any budget, is the one measured at once: copies, a broken sample
// in one track (left out of every track there), an inverted copy and an unrelated track.
TEST(GainMeasure, MeasuresWhatOneMeasurementAtOnceMeasures)
{
    auto broken = noise(51);
    broken[1000] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> inverted = noise(51);
    for (float& value : inverted) {
        value = -value;
    }
    const std::array<std::vector<std::vector<float>>, 3> sets{{
        {noise(51), noise(51)},
        {noise(51), broken, noise(52)},
        {noise(51), inverted, noise(53), noise(54)},
    }};
    for (const auto& set : sets) {
        const std::vector<std::span<const float>> tracks(set.begin(), set.end());
        const double atOnce = overlapGain(tracks, kBlock);
        for (const std::size_t budget : {std::size_t{1}, kLength, kLength * 3}) {
            GainMeasure measure;
            measure.start(tracks, kBlock);
            while (!measure.step(budget)) {
            }
            EXPECT_EQ(measure.result(), atOnce) << set.size() << " tracks, budget " << budget;
        }
    }
}

// Each step passes over at least one track, so that it ends, and no more than its budget allows
// beyond the pass that reaches it.
TEST(GainMeasure, PassesOverOneTrackAtLeastAndNoMoreThanItsBudgetAllows)
{
    const std::vector<std::vector<float>> set{noise(61), noise(62), noise(63)};
    const std::vector<std::span<const float>> tracks(set.begin(), set.end());
    for (const std::size_t budget : {std::size_t{1}, kLength * 2}) {
        GainMeasure measure;
        measure.start(tracks, kBlock);
        int steps = 0;
        bool ended = false;
        while (!ended) {
            const std::uint64_t before = measure.work();
            ended = measure.step(budget);
            const std::uint64_t passed = measure.work() - before;
            ASSERT_GT(passed, 0U) << "a step made no progress";
            ASSERT_LT(passed, budget + kLength) << "a step went on past its budget";
            ++steps;
        }
        EXPECT_GT(steps, 1) << "budget " << budget << ": measured all at once";
    }
}

// One search is used for track after track: where the next two tracks do not sound together, it
// leaves nothing of the samples the one before held there, on either side. A track that sounds in
// only the first half of the stretch, against another that sounds throughout, after a search of
// two tracks that both sounded throughout, reads as it does searched afresh.
TEST(PairSearch, LeavesNothingOfTheLastSearchWhereTracksDoNotMeet)
{
    const auto whole = noise(43);
    auto half = whole;
    std::fill(half.begin() + static_cast<std::ptrdiff_t>(kLength / 2), half.end(), 0.0f);
    const auto earlier = noise(44);
    for (const auto& [a, b] : {std::pair{whole, half}, std::pair{half, whole}}) {
        PairSearch search;
        search.start(earlier, earlier, kMaxOffset, kBlock);
        while (!search.step(kLength * 4)) {
        }
        search.start(a, b, kMaxOffset, kBlock);
        while (!search.step(kLength * 4)) {
        }
        const PairAnalysis afresh = analysePair(a, b, kMaxOffset, kBlock);
        EXPECT_EQ(search.result().correlation, afresh.correlation);
        EXPECT_EQ(search.result().related, afresh.related);
    }
}

// Starting again leaves nothing of the search before: one begun on a copy 300 samples late and
// started again on a copy 40 late finds 40.
TEST(PairSearch, StartsAfresh)
{
    const auto a = noise(42);
    PairSearch search;
    search.start(a, delayed(a, 300), kMaxOffset, kBlock);
    EXPECT_FALSE(search.step(1)) << "the first search ended at once: nothing left of it";
    search.start(a, delayed(a, 40), kMaxOffset, kBlock);
    while (!search.step(1)) {
    }
    EXPECT_EQ(search.result().offset, 40);
}

TEST(PairAnalysis, IdenticalTracksReinforce)
{
    const auto a = noise(1);
    const PairAnalysis result = analyse(a, a);
    const std::array<std::span<const float>, 2> tracks{a, a};
    EXPECT_NEAR(overlapGain(tracks, kBlock), 10.0 * std::log10(2.0), 0.01) << "about +3 dB";
    EXPECT_NEAR(result.correlation, 1.0, 1e-6);
    EXPECT_TRUE(result.related);
    EXPECT_EQ(result.offset, 0);
    EXPECT_FALSE(result.inverted);
}

TEST(PairAnalysis, AnInvertedCopyCancels)
{
    const auto a = noise(2);
    std::vector<float> b(a);
    for (float& value : b) {
        value = -value;
    }
    const PairAnalysis result = analyse(a, b);
    const std::array<std::span<const float>, 2> tracks{a, b};
    EXPECT_LT(overlapGain(tracks, kBlock), -60.0);
    EXPECT_NEAR(result.correlation, -1.0, 1e-6);
    EXPECT_EQ(result.offset, 0);
    EXPECT_TRUE(result.inverted) << "the fix flips it back";
}

TEST(PairAnalysis, FindsHowLateATrackArrives)
{
    const auto a = noise(3);
    const auto b = delayed(a, 37);
    const PairAnalysis result = analyse(a, b);
    EXPECT_EQ(result.offset, 37);
    EXPECT_FALSE(result.inverted);
    EXPECT_LT(std::abs(result.correlation), 0.1) << "unaligned, broadband noise barely correlates";

    const PairAnalysis early = analyse(b, a);
    EXPECT_EQ(early.offset, -37) << "a track that leads has a negative offset";
}

// The audio searched has whatever length the view leaves, not only whole blocks: one sample short
// of a whole number of blocks, exactly a whole number, or one over, a late copy is found where it
// is, and no block is read past the end of the audio.
TEST(PairAnalysis, FindsALateCopyWhateverLengthIsSearched)
{
    for (const std::size_t length :
         {(2 * kBlock) - 1, 2 * kBlock, (2 * kBlock) + 1, (20 * kBlock) - 1, 20 * kBlock}) {
        SCOPED_TRACE(testing::Message() << length << " samples");
        const auto a = noise(3, length);
        const auto b = delayed(a, 37);
        const PairAnalysis result = analyse(a, b);
        EXPECT_TRUE(result.measured);
        EXPECT_TRUE(result.related);
        EXPECT_EQ(result.offset, 37);
        EXPECT_FALSE(result.inverted);
    }
}

// `values` scaled to an RMS level `dbfs` (the noise is about -10.8 dBFS RMS).
std::vector<float> atLevel(std::vector<float> values, double dbfs)
{
    const double rms = std::sqrt(energyOf(values) / static_cast<double>(values.size()));
    const auto gain = static_cast<float>(std::pow(10.0, dbfs / 20.0) / rms);
    for (float& value : values) {
        value *= gain;
    }
    return values;
}

// Compare matches any tracks that sound above the analysis gate, however quiet: a late copy at
// levels from 3 dB to 60 dB above the gate is found, and its sum measured; 3 dB below the gate,
// nothing is measured.
TEST(PairAnalysis, MatchesQuietTracksAboveTheGate)
{
    for (const double above : {3.0, 10.0, 30.0, 60.0}) {
        SCOPED_TRACE(testing::Message() << above << " dB above the gate");
        const auto a = atLevel(noise(11), protocol::kAnalysisGateDbfs + above);
        const PairAnalysis result = analyse(a, delayed(a, 37));
        EXPECT_TRUE(result.measured);
        EXPECT_TRUE(result.related);
        EXPECT_EQ(result.offset, 37);
        const std::array<std::span<const float>, 2> pair{a, a};
        EXPECT_NEAR(overlapGain(pair, kBlock), 10.0 * std::log10(2.0), 0.01) << "copies";
    }
    const auto below = atLevel(noise(11), protocol::kAnalysisGateDbfs - 3.0);
    EXPECT_FALSE(analyse(below, delayed(below, 37)).measured) << "below the gate";
}

// In a short Seconds span the search covers the match reach either way of a stretch only twice
// its length: a copy late by most of the reach (8 ms in 20 ms at 48 kHz) is still found there.
TEST(PairAnalysis, FindsALateCopyInAShortSpan)
{
    constexpr std::size_t kShort = 960;
    const auto a = noise(12, kShort);
    const PairAnalysis result = analysePair(a, delayed(a, 384), kMaxOffset, kBlock);
    EXPECT_TRUE(result.related);
    EXPECT_EQ(result.offset, 384);
    EXPECT_FALSE(result.inverted);
}

// A shift is suggested only when it matches clearly better than none. Here the fast part of two
// tracks lines up as they are, while their slow part, the only one the coarse search sees (the
// fast part cancels in every 8 samples it averages), is 100 samples late in the second, with a
// hundredth more energy: shifted, they match a little better, but not clearly, so no shift is
// suggested, though the coarse search found one far from none.
TEST(PairAnalysis, SuggestsNoShiftThatMatchesNoBetterThanNone)
{
    const auto fastNoise = noise(14);
    const auto slowNoise = noise(15);
    std::vector<float> fast(kLength);
    std::vector<float> slow(kLength);
    for (std::size_t i = 0; i < kLength; ++i) {
        fast[i] = (i % 2 == 0 ? 1.0f : -1.0f) * fastNoise[i / 8]; // Cancels in every 8.
        float sum = 0.0f;
        for (std::size_t j = i < 32 ? 0 : i - 32; j <= i; ++j) {
            sum += slowNoise[j];
        }
        slow[i] = sum;
    }
    const auto energy = [](const std::vector<float>& values) {
        double total = 0.0;
        for (const float value : values) {
            total += static_cast<double>(value) * value;
        }
        return total;
    };
    const auto scale = static_cast<float>(std::sqrt(1.01 * energy(fast) / energy(slow)));
    for (float& value : slow) {
        value *= scale;
    }
    const auto lateSlow = delayed(slow, 100);
    std::vector<float> a(kLength);
    std::vector<float> b(kLength);
    for (std::size_t i = 0; i < kLength; ++i) {
        a[i] = fast[i] + slow[i];
        b[i] = fast[i] + lateSlow[i];
    }
    const PairAnalysis result = analysePair(a, b, kMaxOffset, kBlock);
    EXPECT_TRUE(result.related);
    EXPECT_EQ(result.offset, 0) << "a shift suggested that matches only slightly better than none";
}

// A steady tone matches itself every period, so a late copy could be lined up at several shifts;
// the nearest is the one suggested: a 200 Hz tone 40 samples late (its period is 240) reads as
// about 40 samples late, not as leading by a period less that.
TEST(PairAnalysis, LinesUpADelayedToneAtTheNearestShift)
{
    std::vector<float> a(kLength);
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 200.0 *
                                                  static_cast<double>(i) / 48000.0));
    }
    const PairAnalysis result = analyse(a, delayed(a, 40));
    EXPECT_TRUE(result.related);
    // Within a tenth of the period: shifts that match almost as well may be preferred if smaller.
    EXPECT_NEAR(static_cast<double>(result.offset), 40.0, 24.0);
    EXPECT_FALSE(result.inverted);
}

// Tracks count as sounding together only where the quieter is within 20 dB of the louder: a track
// that cancels this one, then carries only bleed 30 dB down while this one plays on, reads as
// cancelling, not watered down to harmless by the stretch where it is all but silent.
TEST(PairAnalysis, JudgesTheSumWhereTheTracksSoundTogether)
{
    const auto a = noise(13);
    const auto bleed = atLevel(noise(14), -10.8 - 30.0);
    std::vector<float> b(a.size());
    constexpr std::size_t kCancelling = 12 * kBlock;
    for (std::size_t i = 0; i < b.size(); ++i) {
        b[i] = i < kCancelling ? -a[i] : bleed[i];
    }
    const std::array<std::span<const float>, 2> pair{a, b};
    EXPECT_LT(overlapGain(pair, kBlock), -60.0);
}

// A quieter double, 12 dB down and inverted, still sounds together with the track: its partial
// cancellation is measured, (1 - 1/4)^2 of the energy against 1 + 1/16.
TEST(PairAnalysis, JudgesAQuieterDoubleAsSoundingTogether)
{
    const auto a = noise(15);
    std::vector<float> b(a.size());
    for (std::size_t i = 0; i < b.size(); ++i) {
        b[i] = -0.25f * a[i];
    }
    const std::array<std::span<const float>, 2> pair{a, b};
    EXPECT_NEAR(overlapGain(pair, kBlock), 10.0 * std::log10(0.5625 / 1.0625), 0.01);
}

TEST(PairAnalysis, UnrelatedTracksSumHarmlessly)
{
    const auto a = noise(4);
    const auto b = noise(5);
    const PairAnalysis result = analyse(a, b);
    const std::array<std::span<const float>, 2> tracks{a, b};
    EXPECT_NEAR(overlapGain(tracks, kBlock), 0.0, 0.2);
    EXPECT_NEAR(result.correlation, 0.0, 0.05);
    EXPECT_FALSE(result.related);
    EXPECT_EQ(result.offset, 0);
    EXPECT_FALSE(result.inverted) << "nothing to fix";
}

TEST(PairAnalysis, SuggestsNoFixForAHiHatOverAKickTail)
{
    // A decaying 50 Hz kick every 12000 samples, and noise bursts starting in its tail.
    std::vector<float> kick(kLength);
    std::vector<float> hiHat(kLength);
    const auto bursts = noise(7);
    for (std::size_t i = 0; i < kLength; ++i) {
        const auto since = static_cast<double>(i % 12000) / 48000.0;
        kick[i] = static_cast<float>(0.9 * std::exp(-since * 9.0) *
                                     std::sin(2.0 * std::numbers::pi * 50.0 * since));
        const std::size_t burst = (i + 6000) % 12000;
        hiHat[i] =
            burst < 2400
                ? bursts[i] * static_cast<float>(std::exp(-static_cast<double>(burst) / 400.0))
                : 0.0f;
    }
    const PairAnalysis result = analyse(kick, hiHat);
    EXPECT_FALSE(result.related);
    EXPECT_EQ(result.offset, 0);
    EXPECT_FALSE(result.inverted);
}

TEST(PairAnalysis, FindsALateCopyUnderOtherSound)
{
    // The same source in a second microphone, 37 samples later, under as much unrelated sound.
    const auto a = noise(8);
    auto b = delayed(a, 37);
    const auto other = noise(9);
    for (std::size_t i = 0; i < kLength; ++i) {
        b[i] += other[i];
    }
    const PairAnalysis result = analyse(a, b);
    EXPECT_TRUE(result.related);
    EXPECT_EQ(result.offset, 37);
}

TEST(PairAnalysis, CallsAnInvertedToneInvertedRatherThanLate)
{
    // At 60 Hz a half period (400 samples) lies within the search: both explain the cancellation.
    std::vector<float> a(kLength);
    std::vector<float> b(kLength);
    for (std::size_t i = 0; i < kLength; ++i) {
        const double phase = 2.0 * std::numbers::pi * 60.0 * static_cast<double>(i) / 48000.0;
        a[i] = static_cast<float>(0.5 * std::sin(phase));
        b[i] = -a[i];
    }
    const PairAnalysis result = analyse(a, b);
    EXPECT_TRUE(result.inverted);
    EXPECT_EQ(result.offset, 0);
}

TEST(PairAnalysis, ReportsNothingForSilence)
{
    const std::vector<float> silence(kLength, 0.0f);
    const PairAnalysis result = analyse(noise(6), silence);
    EXPECT_FALSE(result.measured);
    EXPECT_EQ(result.correlation, 0.0);
    EXPECT_EQ(result.offset, 0);
}

TEST(PairAnalysis, LeavesOutTracksBelowTheSilenceFloor)
{
    // A late copy at about -126 dBFS, as dither or a tail would be: too quiet to analyse.
    std::vector<float> quiet = noise(8);
    for (float& value : quiet) {
        value *= 1e-6f;
    }
    const PairAnalysis result = analyse(quiet, delayed(quiet, 20));
    EXPECT_FALSE(result.measured);
    EXPECT_FALSE(result.related);
    EXPECT_TRUE(analyse(noise(8), delayed(noise(8), 20)).measured);
}

// Tracks that meet only far below the loudest moment do not sound together: this track plays
// loud, then fades 70 dB, and the other is an inverted copy of that faint tail alone. Both are
// above the analysis gate there, yet a verdict from near-silence is no verdict: nothing measured.
TEST(PairAnalysis, LeavesOutWhereTracksMeetOnlyFarBelowTheLoudest)
{
    constexpr float kTail = 3.16e-4f; // -70 dB.
    auto a = noise(12);
    std::vector<float> b(kLength, 0.0f);
    for (std::size_t i = kLength / 2; i < kLength; ++i) {
        a[i] *= kTail; // About -80 dBFS: above the analysis gate (kAnalysisGateDbfs).
        b[i] = -a[i];
    }
    const PairAnalysis result = analyse(a, b);
    EXPECT_FALSE(result.measured);
    EXPECT_FALSE(result.related);
    EXPECT_FALSE(result.inverted) << "nothing to fix";

    // The same tail 50 dB down, nearer than 60 dB to the loudest and still above the gate, is
    // compared and found inverted: only how far below the loudest it was left it out.
    constexpr float kNearerTail = 3.16e-3f; // -50 dB.
    auto nearer = noise(12);
    std::vector<float> nearerCopy(kLength, 0.0f);
    for (std::size_t i = kLength / 2; i < kLength; ++i) {
        nearer[i] *= kNearerTail;
        nearerCopy[i] = -nearer[i];
    }
    const PairAnalysis compared = analyse(nearer, nearerCopy);
    EXPECT_TRUE(compared.measured);
    EXPECT_TRUE(compared.inverted);
}

TEST(PairAnalysis, JudgesABriefTrackWhereItPlays)
{
    // Four seconds of one track; the other, a late copy, plays only in the first 0.2 s.
    constexpr std::size_t kLong = 192000;
    const auto a = noise(10, kLong);
    auto b = delayed(a, 37);
    std::fill(b.begin() + 9600, b.end(), 0.0f);
    const PairAnalysis result = analyse(a, b);
    EXPECT_TRUE(result.related) << "not watered down by the 3.8 s where it is silent";
    EXPECT_EQ(result.offset, 37);
}

TEST(PairAnalysis, CountsTheSummingGainWhereTracksOverlap)
{
    // The second track cancels the first, but only in its first 12 blocks (about a quarter).
    const auto a = noise(11);
    std::vector<float> b(kLength, 0.0f);
    for (std::size_t i = 0; i < 12 * kBlock; ++i) {
        b[i] = -a[i];
    }
    const std::array<std::span<const float>, 2> tracks{a, b};
    EXPECT_LT(overlapGain(tracks, kBlock), -60.0) << "where they meet, they cancel";
    const std::vector<float> silence(kLength, 0.0f);
    const std::array<std::span<const float>, 2> apart{a, silence};
    EXPECT_TRUE(std::isnan(overlapGain(apart, kBlock))) << "never together: nothing measured";
    const std::array<std::span<const float>, 1> alone{a};
    EXPECT_EQ(overlapGain(alone, kBlock), 0.0);
}

TEST(PairAnalysis, ComputesTheSummingGainFromBlockEnergies)
{
    const auto a = noise(11);
    std::vector<float> b(kLength, 0.0f);
    for (std::size_t i = 0; i < 12 * kBlock; ++i) {
        b[i] = -0.5f * a[i];
    }
    const std::array<std::span<const float>, 2> tracks{a, b};
    const std::size_t blocks = kLength / kBlock;
    std::vector<double> energyA(blocks);
    std::vector<double> energyB(blocks);
    std::vector<double> sums(blocks);
    for (std::size_t index = 0; index < blocks; ++index) {
        for (std::size_t i = index * kBlock; i < (index + 1) * kBlock; ++i) {
            energyA[index] += static_cast<double>(a[i]) * a[i];
            energyB[index] += static_cast<double>(b[i]) * b[i];
            sums[index] += static_cast<double>(a[i] + b[i]) * (a[i] + b[i]);
        }
    }
    const std::array<std::span<const double>, 2> energies{energyA, energyB};
    EXPECT_NEAR(overlapGainOfBlocks(energies, sums, kBlock), overlapGain(tracks, kBlock), 1e-9);
    EXPECT_NEAR(overlapGainOfBlocks(energies, sums, kBlock), 10.0 * std::log10(0.25 / 1.25), 0.01)
        << "half the level, inverted: where they meet the sum is a quarter of the energy";
}

// An exact cancellation is a measurement, -infinity dB, which the UI shows below its gain floor;
// silence is none, NaN. The engine floors neither, so the two stay apart.
TEST(PairAnalysis, SendsAnExactCancellationAsMinusInfinity)
{
    constexpr double kMinusInfinity = -std::numeric_limits<double>::infinity();
    EXPECT_EQ(summingGain(0.0, 1.0), kMinusInfinity);
    EXPECT_EQ(summingGain(0.0, 1e-300), kMinusInfinity);
    EXPECT_NEAR(summingGain(1e-40, 1.0), -400.0, 1e-9) << "a deep null keeps its depth";
    EXPECT_TRUE(std::isnan(summingGain(0.0, 0.0))) << "silent: nothing to measure";

    const auto a = noise(12);
    std::vector<float> b(a);
    for (float& value : b) {
        value = -value;
    }
    const std::array<std::span<const float>, 2> tracks{a, b};
    EXPECT_EQ(overlapGain(tracks, kBlock), kMinusInfinity);
    const std::vector<double> energy(kLength / kBlock, 1.0);
    const std::vector<double> nothing(kLength / kBlock, 0.0);
    const std::array<std::span<const double>, 2> energies{energy, energy};
    EXPECT_EQ(overlapGainOfBlocks(energies, nothing, kBlock), kMinusInfinity);
    const std::array<std::span<const double>, 2> silent{nothing, nothing};
    EXPECT_TRUE(std::isnan(overlapGainOfBlocks(silent, nothing, kBlock)))
        << "never together: nothing measured, not a cancellation";
}

} // namespace
} // namespace orizon::engine
