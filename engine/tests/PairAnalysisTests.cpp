#include "engine/PairAnalysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <gtest/gtest.h>
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
    std::vector<float> scratch;
    return analysePair(a, b, kMaxOffset, kBlock, scratch);
}

TEST(PairAnalysis, IdenticalTracksReinforce)
{
    const auto a = noise(1);
    const PairAnalysis result = analyse(a, a);
    EXPECT_NEAR(result.sumGain, 10.0 * std::log10(2.0), 0.01) << "about +3 dB";
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
    EXPECT_LT(result.sumGain, -60.0);
    EXPECT_NEAR(result.correlation, -1.0, 1e-6);
    EXPECT_EQ(result.offset, 0);
    EXPECT_TRUE(result.inverted);
    EXPECT_NEAR(result.fixedGain, 10.0 * std::log10(2.0), 0.01) << "flipped back, it reinforces";
}

TEST(PairAnalysis, FindsHowLateATrackArrives)
{
    const auto a = noise(3);
    const auto b = delayed(a, 37);
    const PairAnalysis result = analyse(a, b);
    EXPECT_EQ(result.offset, 37);
    EXPECT_FALSE(result.inverted);
    EXPECT_LT(std::abs(result.correlation), 0.1) << "unaligned, broadband noise barely correlates";
    EXPECT_NEAR(result.fixedGain, 10.0 * std::log10(2.0), 0.05);

    const PairAnalysis early = analyse(b, a);
    EXPECT_EQ(early.offset, -37) << "a track that leads has a negative offset";
}

TEST(PairAnalysis, UnrelatedTracksSumHarmlessly)
{
    const PairAnalysis result = analyse(noise(4), noise(5));
    EXPECT_NEAR(result.sumGain, 0.0, 0.2);
    EXPECT_NEAR(result.correlation, 0.0, 0.05);
    EXPECT_FALSE(result.related);
    EXPECT_EQ(result.offset, 0);
    EXPECT_FALSE(result.inverted);
    EXPECT_EQ(result.fixedGain, result.sumGain) << "nothing to fix";
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
    EXPECT_EQ(result.sumGain, 0.0);
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
    EXPECT_NEAR(result.fixedGain, 10.0 * std::log10(2.0), 0.1);
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

} // namespace
} // namespace orizon::engine
