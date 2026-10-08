#include "engine/PairAnalysis.h"

#include <cmath>
#include <gtest/gtest.h>
#include <numbers>
#include <random>
#include <vector>

namespace orizon::engine {
namespace {

constexpr std::size_t kLength = 24000;
constexpr std::size_t kMaxOffset = 480; // 10 ms at 48 kHz.

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
    return analysePair(a, b, kMaxOffset, scratch);
}

TEST(PairAnalysis, IdenticalTracksReinforce)
{
    const auto a = noise(1);
    const PairAnalysis result = analyse(a, a);
    EXPECT_NEAR(result.sumGain, 10.0 * std::log10(2.0), 0.01) << "about +3 dB";
    EXPECT_NEAR(result.correlation, 1.0, 1e-6);
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
    EXPECT_EQ(result.sumGain, 0.0);
    EXPECT_EQ(result.correlation, 0.0);
    EXPECT_EQ(result.offset, 0);
}

TEST(PairAnalysis, AppliesTheFix)
{
    const std::vector<float> b{1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<float> fixed(4);
    applyFix(b, 1, true, fixed);
    EXPECT_EQ(fixed, (std::vector<float>{-2.0f, -3.0f, -4.0f, 0.0f})) << "earlier by one, flipped";
    applyFix(b, -2, false, fixed);
    EXPECT_EQ(fixed, (std::vector<float>{0.0f, 0.0f, 1.0f, 2.0f}));
}

} // namespace
} // namespace orizon::engine
