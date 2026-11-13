// The wave drawn between samples at deep zoom: the band-limited signal the samples describe, as a
// converter plays it, not straight lines whose corners the audio cannot hold. Checked against the
// true tone at every sample rate a host uses, for tones up to the highest the help promises
// (0.4 of the rate); and where the neighbours run out, at a join or beside a broken sample, drawn
// straight instead of ringing.

#include "SampleRates.h"
#include "engine/Reconstruction.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <vector>

namespace orizon::engine {
namespace {

constexpr std::size_t kReach = kReconstructionReach;

// A tone of `frequency` at `rate`, `count` samples of it from sample 0, at a phase that puts no
// sample on a crest.
std::vector<float> tone(double frequency, double rate, std::size_t count)
{
    std::vector<float> samples(count);
    for (std::size_t n = 0; n < count; ++n) {
        samples[n] = static_cast<float>(
            0.8 *
            std::sin((2.0 * std::numbers::pi * frequency * static_cast<double>(n) / rate) + 0.3));
    }
    return samples;
}

double toneAt(double frequency, double rate, double position)
{
    return 0.8 * std::sin((2.0 * std::numbers::pi * frequency * position / rate) + 0.3);
}

struct ToneCase
{
    double rate = 48000.0;
    double share = 0.1; // Of the sample rate.
};

std::vector<ToneCase> toneCases()
{
    std::vector<ToneCase> cases;
    for (const double rate : kSampleRates) {
        for (const double share : {0.02, 0.1, 0.25, 0.4}) {
            cases.push_back({.rate = rate, .share = share});
        }
    }
    return cases;
}

class Tones : public ::testing::TestWithParam<ToneCase>
{
};

// Between the samples, the drawn wave is the tone itself, crests between samples included; the
// samples are drawn exactly where they are.
TEST_P(Tones, DrawTheWaveTheSamplesDescribe)
{
    const ToneCase c = GetParam();
    const double frequency = c.share * c.rate;
    constexpr std::size_t kShown = 40;
    const std::vector<float> samples = tone(frequency, c.rate, kShown + (2 * kReach));
    for (const std::size_t factor : {std::size_t{2}, std::size_t{7}, kMaxPointsPerSample}) {
        SCOPED_TRACE(factor);
        Reconstruction reconstruction;
        std::vector<float> points;
        reconstruction.upsample(samples, kReach, kReach, factor, points);
        ASSERT_EQ(points.size(), ((kShown - 1) * factor) + 1);
        double worst = 0.0;
        for (std::size_t k = 0; k < points.size(); ++k) {
            const double position = static_cast<double>(kReach) +
                                    (static_cast<double>(k) / static_cast<double>(factor));
            if (k % factor == 0) {
                EXPECT_EQ(points[k], samples[kReach + (k / factor)]) << "sample " << k / factor;
            }
            worst = std::max(worst, std::abs(points[k] - toneAt(frequency, c.rate, position)));
        }
        EXPECT_LT(worst, 1e-3) << "the wave between the samples is the tone's";
    }
}

std::string toneName(const ::testing::TestParamInfo<ToneCase>& info)
{
    return std::to_string(static_cast<long long>(std::llround(info.param.rate))) + "Hz_" +
           std::to_string(static_cast<int>(std::lround(info.param.share * 100.0))) +
           "PercentOfRate";
}

INSTANTIATE_TEST_SUITE_P(EveryRate, Tones, ::testing::ValuesIn(toneCases()), toneName);

// A constant stays the constant between its samples: no ripple on DC or a held level.
TEST(Reconstruction, KeepsAConstantFlat)
{
    const std::vector<float> samples(64, 0.37f);
    Reconstruction reconstruction;
    std::vector<float> points;
    reconstruction.upsample(samples, kReach, kReach, 8, points);
    for (const float point : points) {
        EXPECT_NEAR(point, 0.37f, 1e-6f);
    }
}

// Points per sample follow the columns per sample: the samples alone while lines and the wave
// look alike (fewer than kReconstructionColumnsPerSample columns a sample), then enough points for
// a point per column, never fewer as the view widens on screen, and never more than the cap.
TEST(Reconstruction, AddsAboutAPointPerColumn)
{
    const auto threshold = protocol::kReconstructionColumnsPerSample;
    for (const double samples : {8.0, 32.0, 100.0, 1000.0}) {
        SCOPED_TRACE(samples);
        std::size_t previous = 1;
        for (std::size_t columns = 1; columns < 40000; columns += 7) {
            const std::size_t points = Reconstruction::pointsPerSample(samples, columns);
            const double perSample = static_cast<double>(columns) / samples;
            if (perSample < threshold) {
                EXPECT_EQ(points, 1U) << columns << " columns";
            } else {
                EXPECT_GT(points, 1U) << columns << " columns";
                EXPECT_GE(static_cast<double>(points),
                          std::min(perSample, static_cast<double>(kMaxPointsPerSample)))
                    << "a point per column, " << columns << " columns";
            }
            EXPECT_LE(points, kMaxPointsPerSample);
            EXPECT_GE(points, previous) << "never fewer as columns grow";
            previous = points;
        }
    }
    EXPECT_EQ(Reconstruction::pointsPerSample(0.0, 2000), 1U) << "no samples known";
}

// Where the neighbours run out (the edge of what is held), the points between the last samples
// are drawn straight between them rather than from a filter missing half its taps.
TEST(Reconstruction, DrawsLinesWhereNeighboursRunOut)
{
    const std::vector<float> samples = tone(5000.0, 48000.0, 48);
    Reconstruction reconstruction;
    std::vector<float> points;
    constexpr std::size_t kFactor = 4;
    reconstruction.upsample(samples, 0, 0, kFactor, points);
    ASSERT_EQ(points.size(), ((samples.size() - 1) * kFactor) + 1);
    const auto straight = [&](std::size_t n) {
        for (std::size_t phase = 1; phase < kFactor; ++phase) {
            const float fraction = static_cast<float>(phase) / static_cast<float>(kFactor);
            const float line = samples[n] + ((samples[n + 1] - samples[n]) * fraction);
            if (std::abs(points[(n * kFactor) + phase] - line) > 1e-6f) {
                return false;
            }
        }
        return true;
    };
    EXPECT_TRUE(straight(0)) << "no neighbours before the first sample";
    EXPECT_TRUE(straight(samples.size() - 2)) << "none after the last";
    EXPECT_FALSE(straight(samples.size() / 2)) << "the middle has all its neighbours";
}

// The first gap with all its neighbours held (a reach in from the first sample, as at the start of
// what a window holds) is the wave, not a line: only the gaps before it fall back.
TEST(Reconstruction, DrawsTheWaveFromTheFirstGapWithAllItsNeighbours)
{
    constexpr double kRate = 48000.0;
    constexpr double kFrequency = 5000.0;
    const std::vector<float> samples = tone(kFrequency, kRate, 64);
    Reconstruction reconstruction;
    std::vector<float> points;
    constexpr std::size_t kFactor = 4;
    reconstruction.upsample(samples, 0, 0, kFactor, points);
    const std::size_t first = kReach - 1; // Reads samples 0 to 2 * kReach - 1.
    for (std::size_t phase = 1; phase < kFactor; ++phase) {
        const double position = static_cast<double>(first) + (static_cast<double>(phase) / kFactor);
        EXPECT_NEAR(points[(first * kFactor) + phase], toneAt(kFrequency, kRate, position), 1e-3)
            << "phase " << phase;
    }
    const float line = samples[first - 1] + ((samples[first] - samples[first - 1]) / 2.0f);
    EXPECT_NEAR(points[((first - 1) * kFactor) + 2], line, 1e-6f) << "one gap earlier: a line";
}

// A sample that is not finite (a broken signal upstream) leaves a gap beside it, and the points
// whose neighbours include it are drawn straight; the rest of the wave is not lost with it.
TEST(Reconstruction, DrawsAroundABrokenSample)
{
    std::vector<float> samples = tone(3000.0, 48000.0, 96);
    constexpr std::size_t kBroken = 48;
    samples[kBroken] = std::numeric_limits<float>::quiet_NaN();
    Reconstruction reconstruction;
    std::vector<float> points;
    constexpr std::size_t kFactor = 4;
    reconstruction.upsample(samples, kReach, kReach, kFactor, points);
    const auto at = [&](std::size_t n, std::size_t phase) {
        return points[((n - kReach) * kFactor) + phase];
    };
    EXPECT_TRUE(std::isnan(at(kBroken - 1, 2))) << "between the broken sample and its neighbour";
    EXPECT_TRUE(std::isnan(at(kBroken, 2)));
    const float line = samples[kBroken - 3] + ((samples[kBroken - 2] - samples[kBroken - 3]) / 2);
    EXPECT_NEAR(at(kBroken - 3, 2), line, 1e-6f) << "neighbours reach it: straight";
    EXPECT_TRUE(std::isfinite(at(kBroken - kReach - 1, 2))) << "out of its reach: the wave";
    std::size_t missing = 0;
    for (const float point : points) {
        missing += std::isnan(point) ? 1 : 0;
    }
    EXPECT_EQ(missing, (2 * (kFactor - 1)) + 1) << "only the points beside it are missing";
}

// A join (a sweep's playhead, between the newest pass and the previous one) is drawn straight on
// both sides, so a step there does not ring into the audio either side of it.
TEST(Reconstruction, DoesNotRingAcrossASeam)
{
    std::vector<float> samples(96, 0.0f);
    constexpr std::size_t kSeam = 48;
    std::fill(samples.begin() + static_cast<std::ptrdiff_t>(kSeam), samples.end(), 0.5f);
    Reconstruction reconstruction;
    std::vector<float> points;
    constexpr std::size_t kFactor = 8;
    reconstruction.upsample(samples, kReach, kReach, kFactor, points, kSeam);
    for (std::size_t k = 0; k < points.size(); ++k) {
        const std::size_t n = kReach + (k / kFactor);
        const float expected = n < kSeam - 1 ? 0.0f : n >= kSeam ? 0.5f : points[k];
        EXPECT_NEAR(points[k], expected, 1e-6f) << "point " << k;
        EXPECT_GE(points[k], -1e-6f) << "no undershoot before the step";
        EXPECT_LE(points[k], 0.5f + 1e-6f) << "no overshoot after it";
    }
    // Without the seam the same step rings: the test above is not passing by accident.
    reconstruction.upsample(samples, kReach, kReach, kFactor, points);
    EXPECT_TRUE(std::ranges::any_of(points, [](float point) { return point < -1e-3f; }));
}

} // namespace
} // namespace orizon::engine
