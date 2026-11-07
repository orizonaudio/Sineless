#include "engine/Readouts.h"

#include <cmath>
#include <cstddef>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <vector>

namespace orizon::engine {
namespace {

constexpr double kSampleRate = 48000.0;

std::vector<float> tone(double frequency, double seconds, double amplitude)
{
    std::vector<float> samples(static_cast<std::size_t>(seconds * kSampleRate));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double phase =
            2.0 * std::numbers::pi * frequency * static_cast<double>(i) / kSampleRate;
        samples[i] = static_cast<float>(amplitude * std::sin(phase));
    }
    return samples;
}

TEST(Readouts, MeasureASine)
{
    const Readouts readouts = computeReadouts(tone(440.0, 0.1, 0.5));
    EXPECT_NEAR(readouts.peak, 0.5f, 1e-3f);
    EXPECT_NEAR(readouts.rms, 0.5f / std::numbers::sqrt2_v<float>, 1e-3f);
}

TEST(Readouts, MeasureASquareWave)
{
    std::vector<float> samples(4800);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = (i / 240) % 2 == 0 ? 0.8f : -0.8f; // 100 Hz.
    }
    const Readouts readouts = computeReadouts(samples);
    EXPECT_FLOAT_EQ(readouts.peak, 0.8f);
    EXPECT_FLOAT_EQ(readouts.rms, 0.8f);
}

// A DC offset's peak and RMS are its level, whatever its sign.
TEST(Readouts, MeasureDc)
{
    const Readouts dc = computeReadouts(std::vector<float>(1000, -0.25f));
    EXPECT_FLOAT_EQ(dc.peak, 0.25f);
    EXPECT_FLOAT_EQ(dc.rms, 0.25f);
}

// Samples that are not finite (from a broken signal upstream) are measured around and counted:
// the readouts are the tone's.
TEST(Readouts, MeasureAroundSamplesThatAreNotFinite)
{
    std::vector<float> samples = tone(1000.0, 0.2, 0.5);
    const Readouts clean = computeReadouts(samples);
    for (const std::size_t i : {100U, 2000U, 4321U}) {
        samples[i] = std::numeric_limits<float>::quiet_NaN();
    }
    samples[5000] = std::numeric_limits<float>::infinity();
    samples[7000] = -std::numeric_limits<float>::infinity();
    const Readouts readouts = computeReadouts(samples);
    EXPECT_EQ(readouts.invalid, 5U);
    EXPECT_NEAR(readouts.peak, clean.peak, 1e-6f);
    EXPECT_NEAR(readouts.rms, clean.rms, 1e-3f);
}

// Nothing to measure: no value, not the level of silence.
TEST(Readouts, HandleAnEmptySpan)
{
    const Readouts readouts = computeReadouts({});
    EXPECT_TRUE(std::isnan(readouts.peak));
    EXPECT_TRUE(std::isnan(readouts.rms));
}

// No sample finite: nothing to measure either, and every one counted.
TEST(Readouts, HandleASpanWithNothingFinite)
{
    std::vector<float> samples(960, std::numeric_limits<float>::quiet_NaN());
    samples[10] = std::numeric_limits<float>::infinity();
    samples[20] = -std::numeric_limits<float>::infinity();
    const Readouts readouts = computeReadouts(samples);
    EXPECT_TRUE(std::isnan(readouts.peak));
    EXPECT_TRUE(std::isnan(readouts.rms));
    EXPECT_EQ(readouts.invalid, 960U);
}

} // namespace
} // namespace orizon::engine
