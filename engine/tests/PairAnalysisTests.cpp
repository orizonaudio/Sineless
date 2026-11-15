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

// Each step stops once its budget is spent, but always makes progress: with the smallest budget
// each step does one block of a pass over the stretch or one transform, never the search at once,
// and the steps end with the match.
TEST(PairSearch, DoesOnePieceAStepOnTheSmallestBudget)
{
    const auto a = noise(40);
    PairSearch search;
    search.start(a, delayed(a, 100), kMaxOffset, kBlock);
    std::uint64_t most = 0;
    int steps = 0;
    bool ended = false;
    while (!ended) {
        const std::uint64_t before = search.work();
        ended = search.step(1);
        const std::uint64_t work = search.work() - before;
        if (!ended) {
            ASSERT_GT(work, 0U) << "a step made no progress";
        }
        most = std::max(most, work);
        ++steps;
    }
    EXPECT_GT(steps, static_cast<int>(kLength / kBlock)) << "fewer steps than blocks to pass over";
    EXPECT_LT(most, search.work() / 2) << "one step did most of the search";
    EXPECT_EQ(search.result().offset, 100);
}

// A long search takes several steps of a moderate budget, rather than one, and a stretch eight
// times as long takes several times as many: a step's work follows its budget, not the stretch.
TEST(PairSearch, SpreadsALongSearchOverSteps)
{
    const auto stepsFor = [](std::size_t length) {
        const auto a = noise(41, length);
        PairSearch search;
        search.start(a, delayed(a, 100), kMaxOffset, kBlock);
        int steps = 1;
        while (!search.step(kLength * 8)) {
            ++steps;
        }
        EXPECT_EQ(search.result().offset, 100) << length << " samples";
        return steps;
    };
    const int steps = stepsFor(kLength);
    EXPECT_GT(steps, 2);
    EXPECT_GT(stepsFor(8 * kLength), 4 * steps);
}

// Preparing a stretch (finding where each track sounds, and both) passes over all of it, so it is
// spread over steps like the search:
// starting does none of it, and even tracks that never sound together, which leave no offset to
// search, take several steps of a moderate budget, each counted as work.
TEST(PairSearch, PreparesALongStretchOverSteps)
{
    const std::vector<float> silence(kLength, 0.0f);
    PairSearch search;
    search.start(silence, noise(45), kMaxOffset, kBlock);
    EXPECT_EQ(search.work(), 0U) << "starting did work";
    int steps = 1;
    std::uint64_t most = 0;
    while (true) {
        const std::uint64_t before = search.work();
        const bool ended = search.step(kLength / 4);
        most = std::max(most, search.work() - before);
        if (ended) {
            break;
        }
        ++steps;
    }
    EXPECT_GT(steps, 2) << "the stretch was prepared in one step";
    EXPECT_GE(search.work(), 2 * kLength) << "preparing both tracks was not counted";
    EXPECT_LE(most, (kLength / 4) + (3 * kBlock)) << "a step went far beyond its budget";
    EXPECT_FALSE(search.result().measured);
}

// The blocks only say where tracks sound: tracks that sound throughout are searched alike in
// blocks of 441 samples (10 ms at 44.1 kHz) and in one block, nothing lost at a block's edge.
TEST(PairSearch, FindsTheSameMatchWhateverTheBlockLength)
{
    constexpr std::size_t kRateLength = 44100;
    constexpr std::size_t kEdgeBlock = 441;
    // Tones from 40 Hz to 2 kHz and delays of up to 200 samples either way, spread over trials.
    for (int trial = 0; trial < 100; ++trial) {
        const double low = 40.0 + ((trial * 97) % 1960);
        const double high = 40.0 + (((trial * 53) + 700) % 1960);
        auto a = noise(static_cast<unsigned>(100 + trial), kRateLength);
        for (std::size_t i = 0; i < kRateLength; ++i) {
            const double t = static_cast<double>(i) / 44100.0;
            a[i] = (0.2f * a[i]) +
                   static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * low * t)) +
                   static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * high * t));
        }
        // Late by `delay` (early when negative), wrapping round, so both sound in every block.
        const int delay = ((trial * 37) % 401) - 200;
        std::vector<float> b(kRateLength);
        for (std::size_t i = 0; i < kRateLength; ++i) {
            const auto from = (static_cast<std::ptrdiff_t>(i) - delay +
                               static_cast<std::ptrdiff_t>(kRateLength)) %
                              static_cast<std::ptrdiff_t>(kRateLength);
            b[i] = a[static_cast<std::size_t>(from)];
        }
        const std::vector<float>& first = a;
        const std::vector<float>& second = b;
        const PairAnalysis inBlocks = analysePair(first, second, kEdgeBlock, kEdgeBlock);
        const PairAnalysis whole = analysePair(first, second, kEdgeBlock, kRateLength);
        EXPECT_EQ(inBlocks.offset, whole.offset) << "trial " << trial << ", delay " << delay;
        EXPECT_EQ(inBlocks.inverted, whole.inverted) << "trial " << trial;
    }
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

    // Up to the whole reach either way: the last offsets searched.
    const auto farthest = delayed(a, kMaxOffset);
    EXPECT_EQ(analyse(a, farthest).offset, static_cast<std::ptrdiff_t>(kMaxOffset));
    EXPECT_EQ(analyse(farthest, a).offset, -static_cast<std::ptrdiff_t>(kMaxOffset));
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
// tracks lines up as they are, while their slow part is 100 samples late in the second, with a
// hundredth more energy: shifted, they match a little better, but not clearly, so no shift is
// suggested.
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
// 40 samples late, not as leading by a period less that, nor as the first shift that matches
// almost as well. And an 800 Hz tone 12 samples late (its period is 60) reads as 12 samples
// late, not as flipped and half a period later still, which matches as well.
TEST(PairAnalysis, LinesUpADelayedToneAtTheNearestShift)
{
    struct Tone
    {
        double frequency;
        std::size_t late;
    };
    for (const Tone& tone :
         {Tone{.frequency = 200.0, .late = 40}, Tone{.frequency = 800.0, .late = 12}}) {
        SCOPED_TRACE(testing::Message() << tone.frequency << " Hz");
        std::vector<float> a(kLength);
        for (std::size_t i = 0; i < a.size(); ++i) {
            a[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * tone.frequency *
                                                      static_cast<double>(i) / 48000.0));
        }
        const PairAnalysis result = analyse(a, delayed(a, tone.late));
        EXPECT_TRUE(result.related);
        EXPECT_EQ(result.offset, static_cast<std::ptrdiff_t>(tone.late));
        EXPECT_FALSE(result.inverted);
    }
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

// --- How deep a difference nulls ---------------------------------------------------------------

// The level of this track minus another against this track's, from each one's energy per block
// and the difference's, as the engine keeps them.
double nullDepthOf(const std::vector<float>& own, const std::vector<float>& other)
{
    const std::size_t blocks = own.size() / kBlock;
    std::vector<double> ownEnergy(blocks);
    std::vector<double> otherEnergy(blocks);
    std::vector<double> differenceEnergy(blocks);
    for (std::size_t index = 0; index < blocks; ++index) {
        std::vector<float> difference(kBlock);
        for (std::size_t i = 0; i < kBlock; ++i) {
            difference[i] = own[index * kBlock + i] - other[index * kBlock + i];
        }
        ownEnergy[index] = energyOf(std::span(own).subspan(index * kBlock, kBlock));
        otherEnergy[index] = energyOf(std::span(other).subspan(index * kBlock, kBlock));
        differenceEnergy[index] = energyOf(difference);
    }
    const std::array<std::span<const double>, 2> tracks{ownEnergy, otherEnergy};
    return nullDepthOfBlocks(tracks, differenceEnergy, kBlock, 0);
}

// An effect that changes nothing nulls completely: minus infinity, a measurement, not "no value".
TEST(NullDepth, IsMinusInfinityForAnIdenticalTrack)
{
    const auto own = noise(60);
    EXPECT_EQ(nullDepthOf(own, own), -std::numeric_limits<double>::infinity());
}

// What a level difference leaves: a copy 1 dB louder nulls 18.3 dB below this track,
// 20·log10|1 - 10^(1/20)|, and one at half the level 6 dB below it.
TEST(NullDepth, IsTheDifferencesLevelAgainstThisTrack)
{
    const auto own = noise(61);
    const auto scaled = [&](double gain) {
        std::vector<float> copy(own);
        for (float& value : copy) {
            value = static_cast<float>(value * gain);
        }
        return copy;
    };
    const double louder = std::pow(10.0, 1.0 / 20.0);
    EXPECT_NEAR(nullDepthOf(own, scaled(louder)), 20.0 * std::log10(louder - 1.0), 1e-3);
    EXPECT_NEAR(nullDepthOf(own, scaled(0.5)), 20.0 * std::log10(0.5), 1e-3);
}

// Tracks that never sound together have nothing to null: no value, as with one track alone.
TEST(NullDepth, IsNoValueWhereTheTracksNeverSoundTogether)
{
    const auto own = noise(62);
    const std::vector<float> silent(kLength, 0.0f);
    EXPECT_TRUE(std::isnan(nullDepthOf(own, silent)));
    EXPECT_TRUE(std::isnan(nullDepthOf(silent, own)));
    const std::vector<double> energy(kLength / kBlock, 1.0);
    const std::array<std::span<const double>, 1> alone{energy};
    EXPECT_TRUE(std::isnan(nullDepthOfBlocks(alone, energy, kBlock, 0)));
}

// It is measured where this track and another sound together: a stretch where this track plays
// alone, and its difference is itself, does not water a deep null down.
TEST(NullDepth, CountsOnlyWhereTheTracksSoundTogether)
{
    const auto own = noise(63);
    auto other = own;
    for (std::size_t i = 0; i < kLength; ++i) {
        other[i] = i < kLength / 2 ? own[i] * 0.5f : 0.0f; // Then silent: this track alone.
    }
    EXPECT_NEAR(nullDepthOf(own, other), 20.0 * std::log10(0.5), 1e-3);
}

// --- Bright material ---------------------------------------------------------------------------

// Short hits of noise, ten a second: white, or differentiated (`brightness` times), which leaves
// little below a few kilohertz, as a hi-hat or a cymbal has. Each decays by 60 dB in
// `decaySeconds`.
std::vector<float> hits(double rate, int brightness, double decaySeconds, double seconds)
{
    std::mt19937 random(91); // NOLINT(bugprone-random-generator-seed): reproducible
    std::uniform_real_distribution<float> uniform(-0.5f, 0.5f);
    const auto length = static_cast<std::size_t>(std::lround(seconds * rate));
    const auto spacing = static_cast<std::size_t>(std::lround(0.1 * rate));
    std::vector<float> values(length);
    for (float& value : values) {
        value = uniform(random);
    }
    for (int pass = 0; pass < brightness; ++pass) {
        for (std::size_t i = length - 1; i > 0; --i) {
            values[i] = 0.5f * (values[i] - values[i - 1]);
        }
    }
    const double perSample = std::pow(10.0, -3.0 / (decaySeconds * rate));
    double envelope = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
        envelope = i % spacing == 0 ? 1.0 : envelope * perSample;
        values[i] = static_cast<float>(values[i] * envelope);
    }
    return values;
}

struct BrightCase
{
    double rate;
    double delayMs;
    int brightness;
    double decaySeconds;
};

class BrightMatch : public ::testing::TestWithParam<BrightCase>
{
};

// Two microphones on a hi-hat, or a bright synth and a delayed copy: the copy's offset is found
// exactly, at every rate hosts run at, wherever in the reach it lies.
TEST_P(BrightMatch, FindsALateCopy)
{
    const BrightCase& bright = GetParam();
    const auto track = hits(bright.rate, bright.brightness, bright.decaySeconds, 2.0);
    // An odd number of samples, as most distances are: no whole number of any coarser step.
    const auto delay =
        2 * static_cast<std::size_t>(std::lround(bright.delayMs * 0.0005 * bright.rate)) + 1;
    const auto reach =
        static_cast<std::size_t>(std::lround(protocol::kMatchReachSeconds * bright.rate));
    const auto block = static_cast<std::size_t>(std::lround(0.01 * bright.rate));
    const PairAnalysis analysis = analysePair(track, delayed(track, delay), reach, block);
    EXPECT_TRUE(analysis.related);
    EXPECT_EQ(analysis.offset, static_cast<std::ptrdiff_t>(delay));
    EXPECT_FALSE(analysis.inverted);
}

std::vector<BrightCase> brightCases()
{
    std::vector<BrightCase> cases;
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0, 384000.0}) {
        for (const double delayMs : {0.5, 1.5, 3.0, 4.5, 6.0, 7.5, 9.0}) {
            for (const int brightness : {0, 1, 2}) {
                for (const double decaySeconds : {0.004, 0.05}) {
                    cases.push_back({.rate = rate,
                                     .delayMs = delayMs,
                                     .brightness = brightness,
                                     .decaySeconds = decaySeconds});
                }
            }
        }
    }
    return cases;
}

std::string brightName(const ::testing::TestParamInfo<BrightCase>& info)
{
    const BrightCase& bright = info.param;
    const std::array<const char*, 3> kinds{"White", "Bright", "Brighter"};
    return std::string(kinds.at(static_cast<std::size_t>(bright.brightness))) + "Decaying" +
           std::to_string(std::lround(bright.decaySeconds * 1000.0)) + "ms_Late" +
           std::to_string(std::lround(bright.delayMs * 10.0)) + "TenthsMs_" +
           std::to_string(std::lround(bright.rate)) + "Hz";
}

INSTANTIATE_TEST_SUITE_P(Hits, BrightMatch, ::testing::ValuesIn(brightCases()), brightName);

// A hit shorter than a block, arriving a block and a half late in the other track, never sounds
// together with its copy as recorded: nothing is measured at no offset, and the copy is still
// found where it is.
TEST(PairAnalysis, FindsShortHitsABlockApart)
{
    const auto track = hits(48000.0, 0, 0.004, 2.0);
    const PairAnalysis analysis = analysePair(track, delayed(track, 721), 960, kBlock);
    EXPECT_TRUE(analysis.related);
    EXPECT_EQ(analysis.offset, 721);
    EXPECT_FALSE(analysis.inverted);
}

// --- Bass material -----------------------------------------------------------------------------

// Two seconds of material with little above a few hundred hertz, whose match with a late copy is
// a broad peak: `kind` 0 a 50 Hz note, 1 a bass line of notes from 41 to 62 Hz, 2 noise below
// 150 Hz, 3 noise below 1 kHz, 4 noise below 20 Hz, 5 noise below 50 Hz (smoothed twice over the
// span that leaves that much).
std::vector<float> bass(double rate, int kind)
{
    const auto length = static_cast<std::size_t>(std::lround(2.0 * rate));
    std::vector<float> values(length);
    if (kind < 2) {
        const std::array<double, 4> notes{41.2, 55.0, 49.0, 61.7};
        double phase = 0.0;
        for (std::size_t i = 0; i < length; ++i) {
            const double seconds = static_cast<double>(i) / rate;
            const double note =
                kind == 0 ? 50.0 : notes.at(static_cast<std::size_t>(seconds * 4.0) % 4);
            phase += 2.0 * std::numbers::pi * note / rate;
            values[i] = static_cast<float>(0.5 * std::sin(phase));
        }
        return values;
    }
    std::mt19937 random(92); // NOLINT(bugprone-random-generator-seed): reproducible
    std::uniform_real_distribution<float> uniform(-0.5f, 0.5f);
    for (float& value : values) {
        value = uniform(random);
    }
    // The span of the moving average, run twice, that leaves little above the kind's limit.
    const std::array<double, 6> limits{0.0, 0.0, 150.0, 1000.0, 20.0, 50.0};
    const auto span = static_cast<std::size_t>(
        std::lround(rate / (2.0 * limits.at(static_cast<std::size_t>(kind)))));
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> smoothed(length, 0.0f);
        double sum = 0.0;
        for (std::size_t i = 0; i < length; ++i) {
            sum += values[i];
            if (i >= span) {
                sum -= values[i - span];
            }
            smoothed[i] = static_cast<float>(sum / static_cast<double>(span));
        }
        values = smoothed;
    }
    return values;
}

struct BassCase
{
    double rate;
    int kind;
    double delayMs;
};

class BroadPeak : public ::testing::TestWithParam<BassCase>
{
};

// Two microphones on a bass, a few milliseconds apart: the offset suggested is the one that lines
// them up, not the first that matches almost as well, which lies up to half a millisecond short
// and leaves the fix weaker than it reads.
TEST_P(BroadPeak, FindsTheTopOfTheMatch)
{
    const BassCase& bassCase = GetParam();
    const auto track = bass(bassCase.rate, bassCase.kind);
    const auto delay =
        static_cast<std::size_t>(std::lround(bassCase.delayMs * 0.001 * bassCase.rate));
    const auto reach =
        static_cast<std::size_t>(std::lround(protocol::kMatchReachSeconds * bassCase.rate));
    const auto block = static_cast<std::size_t>(std::lround(0.01 * bassCase.rate));
    const PairAnalysis analysis = analysePair(track, delayed(track, delay), reach, block);
    EXPECT_TRUE(analysis.related);
    // Below 150 Hz the match changes by under a hundred-thousandth from one sample to the next at
    // the highest rates, less than the search's float sums tell apart (and far less than a sum of
    // the tracks would show): up to 8 microseconds off there.
    const std::ptrdiff_t indistinct =
        bassCase.kind < 3 ? static_cast<std::ptrdiff_t>(bassCase.rate / 128000.0) : 0;
    EXPECT_LE(std::abs(analysis.offset - static_cast<std::ptrdiff_t>(delay)), indistinct)
        << analysis.offset << " for " << delay;
    EXPECT_FALSE(analysis.inverted);
    // Seen from the later microphone, the other leads by as much.
    const PairAnalysis leading = analysePair(delayed(track, delay), track, reach, block);
    EXPECT_TRUE(leading.related);
    EXPECT_LE(std::abs(leading.offset + static_cast<std::ptrdiff_t>(delay)), indistinct)
        << leading.offset << " for -" << delay;
    EXPECT_FALSE(leading.inverted);
}

std::vector<BassCase> bassCases()
{
    std::vector<BassCase> cases;
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0, 384000.0}) {
        for (const int kind : {0, 1, 2, 3}) {
            // Far enough for the match to be clearly better than none (a 50 Hz note up to 0.6 ms
            // late matches within 2% as it is, and no shift is suggested), and under a quarter of
            // the lowest note's period (beyond it, flipped and less far matches a tone as well).
            for (const double delayMs : {2.0, 3.5}) {
                cases.push_back({.rate = rate, .kind = kind, .delayMs = delayMs});
            }
        }
    }
    return cases;
}

std::string bassName(const ::testing::TestParamInfo<BassCase>& info)
{
    const BassCase& bassCase = info.param;
    const std::array<const char*, 4> kinds{"Note50Hz", "BassLine", "NoiseBelow150Hz",
                                           "NoiseBelow1kHz"};
    return std::string(kinds.at(static_cast<std::size_t>(bassCase.kind))) + "_Late" +
           std::to_string(std::lround(bassCase.delayMs * 10.0)) + "TenthsMs_" +
           std::to_string(std::lround(bassCase.rate)) + "Hz";
}

INSTANTIATE_TEST_SUITE_P(Bass, BroadPeak, ::testing::ValuesIn(bassCases()), bassName);

// --- Aligning two tracks ------------------------------------------------------------------------

// `values` delayed by `delay` samples, a whole number or not: the wave between the samples, read
// with a long windowed sinc in double (silent before it starts).
std::vector<float> delayedBy(const std::vector<float>& values, double delay)
{
    constexpr int kReach = 192;
    constexpr double kBeta = 12.0;
    const auto bessel = [](double x) {
        double sum = 1.0;
        double term = 1.0;
        for (int k = 1; k < 60; ++k) {
            term *= (x * x / 4.0) / (static_cast<double>(k) * k);
            sum += term;
        }
        return sum;
    };
    const double whole = std::floor(delay);
    const double fraction = delay - whole;
    std::vector<double> taps(2 * std::size_t{kReach});
    for (int k = 0; k < 2 * kReach; ++k) {
        // The output at n reads the input at n - whole - fraction: inputs n - whole - kReach + 1 +
        // k.
        const double t = static_cast<double>(k - kReach + 1) - (1.0 - fraction);
        const double sinc =
            t == 0.0 ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
        const double ratio = t / kReach;
        taps[static_cast<std::size_t>(k)] =
            std::abs(ratio) >= 1.0
                ? 0.0
                : sinc * bessel(kBeta * std::sqrt(1.0 - ratio * ratio)) / bessel(kBeta);
    }
    const auto size = static_cast<std::ptrdiff_t>(values.size());
    std::vector<float> out(values.size(), 0.0f);
    if (fraction == 0.0) {
        return delayed(values, static_cast<std::size_t>(whole));
    }
    for (std::ptrdiff_t n = 0; n < size; ++n) {
        double sum = 0.0;
        // Input index n - whole - 1 + (k - kReach + 1): the samples either side of the point read.
        const std::ptrdiff_t first = n - static_cast<std::ptrdiff_t>(whole) - kReach;
        for (int k = 0; k < 2 * kReach; ++k) {
            const std::ptrdiff_t index = first + k;
            if (index >= 0 && index < size) {
                sum += taps[static_cast<std::size_t>(k)] * values[static_cast<std::size_t>(index)];
            }
        }
        out[static_cast<std::size_t>(n)] = static_cast<float>(sum);
    }
    return out;
}

// Aligned over Align's reach, a quarter of a second either way, in blocks of 10 ms.
PairAnalysis align(const std::vector<float>& a, const std::vector<float>& b, double rate)
{
    return alignPair(a, b,
                     static_cast<std::size_t>(std::lround(protocol::kAlignReachSeconds * rate)),
                     static_cast<std::size_t>(std::lround(0.01 * rate)));
}

struct BetweenCase
{
    double rate;
    double fraction;
    int brightness; // Of the hits, as BrightMatch; -1: steady white noise.
};

class BetweenSamples : public ::testing::TestWithParam<BetweenCase>
{
};

// An effect whose delay is not a whole number of samples (or a second microphone at any
// distance): the delay is found between the samples, to a hundredth of one, and the track is not
// taken for flipped. On bright material no whole number of samples shows it: a copy seven and a
// half samples late matches six and nine samples away better than seven or eight, with the sign
// turned.
TEST_P(BetweenSamples, FindsTheDelay)
{
    const BetweenCase& c = GetParam();
    const auto track = c.brightness < 0 ? noise(71, static_cast<std::size_t>(std::lround(c.rate)))
                                        : hits(c.rate, c.brightness, 0.05, 1.0);
    const double delay = 301.0 + c.fraction;
    const PairAnalysis analysis = align(track, delayedBy(track, delay), c.rate);
    EXPECT_TRUE(analysis.related);
    EXPECT_FALSE(analysis.inverted);
    EXPECT_NEAR(analysis.delay, delay, 0.01);
    // And as whole samples, which a fix of the sum shifts by: the nearest.
    EXPECT_EQ(analysis.offset, std::lround(analysis.delay));
}

std::vector<BetweenCase> betweenCases()
{
    std::vector<BetweenCase> cases;
    for (const double rate : {44100.0, 48000.0}) {
        // At the quarters of a sample the correlation is read at, and between them.
        for (const double fraction : {0.2, 0.25, 0.37, 0.45, 0.5, 0.75}) {
            for (const int brightness : {-1, 1, 2}) {
                cases.push_back({.rate = rate, .fraction = fraction, .brightness = brightness});
            }
        }
    }
    return cases;
}

std::string betweenName(const ::testing::TestParamInfo<BetweenCase>& info)
{
    const BetweenCase& c = info.param;
    const std::array<const char*, 4> kinds{"WhiteNoise", "WhiteHits", "BrightHits", "BrighterHits"};
    const int kind = c.brightness + 1;
    return std::string(kinds.at(static_cast<std::size_t>(kind))) + "_Plus" +
           std::to_string(std::lround(c.fraction * 100.0)) + "Hundredths_" +
           std::to_string(std::lround(c.rate)) + "Hz";
}

INSTANTIATE_TEST_SUITE_P(Fractions, BetweenSamples, ::testing::ValuesIn(betweenCases()),
                         betweenName);

struct WholeCase
{
    double rate;
    int kind; // Of bass(); -1: white noise.
};

class WholeDelay : public ::testing::TestWithParam<WholeCase>
{
};

// A plain latency, as most effects have, is a whole number of samples: it is found as exactly
// that, so the two tracks null completely, on any material and at any rate. On bass the search
// alone lands a few samples off (the correlation is all but flat there), which would leave a null
// of some 45 dB.
TEST_P(WholeDelay, IsFoundExactlySoTheTracksNull)
{
    const WholeCase& c = GetParam();
    const auto track = c.kind < 0 ? noise(72, static_cast<std::size_t>(std::lround(2.0 * c.rate)))
                                  : bass(c.rate, c.kind);
    const auto delay = static_cast<std::size_t>(std::lround(0.0035 * c.rate));
    const PairAnalysis analysis = align(track, delayed(track, delay), c.rate);
    EXPECT_TRUE(analysis.related);
    EXPECT_FALSE(analysis.inverted);
    EXPECT_EQ(analysis.delay, static_cast<double>(delay));
}

std::vector<WholeCase> wholeCases()
{
    std::vector<WholeCase> cases;
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0, 384000.0}) {
        for (const int kind : {-1, 1, 2, 3, 4, 5}) {
            cases.push_back({.rate = rate, .kind = kind});
        }
    }
    return cases;
}

std::string wholeName(const ::testing::TestParamInfo<WholeCase>& info)
{
    const WholeCase& c = info.param;
    const std::array<const char*, 7> kinds{"WhiteNoise",      "Note50Hz",       "BassLine",
                                           "NoiseBelow150Hz", "NoiseBelow1kHz", "NoiseBelow20Hz",
                                           "NoiseBelow50Hz"};
    const int kind = c.kind + 1;
    return std::string(kinds.at(static_cast<std::size_t>(kind))) + "_" +
           std::to_string(std::lround(c.rate)) + "Hz";
}

INSTANTIATE_TEST_SUITE_P(Material, WholeDelay, ::testing::ValuesIn(wholeCases()), wholeName);

// The latencies real effects report (2047 and 9727 samples at 44.1 kHz), far beyond what a
// microphone's distance needs.
TEST(Alignment, FindsAnEffectsLatency)
{
    const auto track = noise(73, 88200);
    for (const std::size_t delay : {std::size_t{2047}, std::size_t{9727}}) {
        const PairAnalysis analysis = align(track, delayed(track, delay), 44100.0);
        EXPECT_TRUE(analysis.related) << delay;
        EXPECT_EQ(analysis.delay, static_cast<double>(delay));
    }
}

// Up to its reach and no further: a delay just inside it, between two samples, is found as
// surely as any (the correlation is taken a little beyond the reach for that); one just outside
// is not a match.
TEST(Alignment, FindsADelayUpToItsReachOnly)
{
    constexpr double kRate = 48000.0;
    const auto track = noise(74, 96000);
    const double reach = protocol::kAlignReachSeconds * kRate;
    const PairAnalysis inside = align(track, delayedBy(track, reach - 0.63), kRate);
    EXPECT_TRUE(inside.related);
    EXPECT_NEAR(inside.delay, reach - 0.63, 0.01);
    const PairAnalysis outside =
        align(track, delayed(track, static_cast<std::size_t>(reach) + 40), kRate);
    EXPECT_FALSE(outside.related);
    // The same for a track that leads.
    const PairAnalysis leading = align(delayedBy(track, reach - 0.63), track, kRate);
    EXPECT_TRUE(leading.related);
    EXPECT_NEAR(leading.delay, 0.63 - reach, 0.01);
    // And a latency of the reach itself is found exactly, on the lowest bass too (noise below
    // 20 Hz), where the correlation alone points short of it.
    const auto low = bass(kRate, 4);
    const PairAnalysis atReach = align(low, delayed(low, static_cast<std::size_t>(reach)), kRate);
    EXPECT_TRUE(atReach.related);
    EXPECT_EQ(atReach.delay, reach);
}

// Align lines up any tracks that sound above the analysis gate, however quiet, as the sum's
// match does: the delay of a copy 3 dB to 60 dB above the gate is found.
TEST(Alignment, FindsTheDelayOfQuietTracksAboveTheGate)
{
    for (const double above : {3.0, 10.0, 30.0, 60.0}) {
        SCOPED_TRACE(testing::Message() << above << " dB above the gate");
        const auto track = atLevel(noise(77, 48000), protocol::kAnalysisGateDbfs + above);
        const PairAnalysis analysis = align(track, delayed(track, 2208), 48000.0);
        EXPECT_TRUE(analysis.related);
        EXPECT_EQ(analysis.delay, 2208.0);
    }
}

// A track that leads is found as surely as one that lags, with a negative delay.
TEST(Alignment, FindsATrackThatLeads)
{
    const auto track = noise(75, 96000);
    const PairAnalysis analysis = align(delayed(track, 2208), track, 48000.0);
    EXPECT_TRUE(analysis.related);
    EXPECT_EQ(analysis.delay, -2208.0);
}

// A copy at another level and flipped is still this track: its delay and its flip are found
// whatever its level, which is left to the difference.
TEST(Alignment, FindsALouderFlippedCopy)
{
    const auto track = noise(76, 96000);
    const double louder = std::pow(10.0, 2.3 / 20.0);
    auto copy = delayed(track, 100);
    for (float& value : copy) {
        value = static_cast<float>(-louder * value);
    }
    const PairAnalysis analysis = align(track, copy, 48000.0);
    EXPECT_TRUE(analysis.related);
    EXPECT_TRUE(analysis.inverted);
    EXPECT_EQ(analysis.delay, 100.0);
}

} // namespace
} // namespace orizon::engine
