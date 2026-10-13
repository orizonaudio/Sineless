#include "engine/Readouts.h"

#include <cmath>
#include <gtest/gtest.h>
#include <numbers>
#include <random>
#include <vector>

namespace orizon::engine {
namespace {

constexpr double kSampleRate = 48000.0;

std::vector<float> tone(double frequency, double seconds, double amplitude,
                        double secondHarmonic = 0.0)
{
    std::vector<float> samples(static_cast<std::size_t>(seconds * kSampleRate));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double phase =
            2.0 * std::numbers::pi * frequency * static_cast<double>(i) / kSampleRate;
        samples[i] = static_cast<float>(amplitude *
                                        (std::sin(phase) + secondHarmonic * std::sin(2 * phase)));
    }
    return samples;
}

TEST(Readouts, MeasureASine)
{
    const Readouts readouts = computeReadouts(tone(440.0, 0.1, 0.5), kSampleRate);
    EXPECT_NEAR(readouts.peak, 0.5f, 1e-3f);
    EXPECT_NEAR(readouts.rms, 0.5f / std::numbers::sqrt2_v<float>, 1e-3f);
    ASSERT_TRUE(readouts.frequencyHz.has_value());
    EXPECT_NEAR(readouts.frequencyHz.value(), 440.0, 0.1);
}

TEST(Readouts, MeasureASquareWave)
{
    std::vector<float> samples(4800);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = (i / 240) % 2 == 0 ? 0.8f : -0.8f; // 100 Hz.
    }
    const Readouts readouts = computeReadouts(samples, kSampleRate);
    EXPECT_FLOAT_EQ(readouts.peak, 0.8f);
    EXPECT_FLOAT_EQ(readouts.rms, 0.8f);
    ASSERT_TRUE(readouts.frequencyHz.has_value());
    EXPECT_NEAR(readouts.frequencyHz.value(), 100.0, 0.01);
}

TEST(Readouts, FindTheFundamentalWithAStrongHarmonic)
{
    const Readouts readouts = computeReadouts(tone(220.0, 0.1, 0.4, 0.5), kSampleRate);
    ASSERT_TRUE(readouts.frequencyHz.has_value());
    EXPECT_NEAR(readouts.frequencyHz.value(), 220.0, 0.1);
}

TEST(Readouts, ReportNoFrequencyForSilenceDcOrTooShortSpans)
{
    EXPECT_FALSE(computeReadouts(std::vector<float>(1000, 0.0f), kSampleRate).frequencyHz);
    const Readouts dc = computeReadouts(std::vector<float>(1000, -0.25f), kSampleRate);
    EXPECT_FALSE(dc.frequencyHz);
    EXPECT_FLOAT_EQ(dc.peak, 0.25f);
    EXPECT_FLOAT_EQ(dc.rms, 0.25f);
    // 0.5 ms of a 100 Hz tone holds no full cycle.
    EXPECT_FALSE(computeReadouts(tone(100.0, 0.0005, 0.5), kSampleRate).frequencyHz);
}

TEST(Readouts, ReportNoFrequencyForNoise)
{
    std::mt19937 random(3); // NOLINT(bugprone-random-generator-seed): repeatable on purpose.
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
    std::vector<float> noise(48000);
    for (float& value : noise) {
        value = uniform(random);
    }
    EXPECT_FALSE(computeReadouts(noise, kSampleRate).frequencyHz);
    // 16-bit dither: two least significant bits of noise.
    for (float& value : noise) {
        value *= 2.0f / 32768.0f;
    }
    EXPECT_FALSE(computeReadouts(noise, kSampleRate).frequencyHz);
}

TEST(Readouts, FindTheFrequencyOfNotesWithGapsBetweenThem)
{
    // 100 Hz in bursts of 50 ms with 70 ms of silence between, as a bass line plays.
    std::vector<float> notes = tone(100.0, 1.0, 0.5);
    for (std::size_t i = 0; i < notes.size(); ++i) {
        if (i % 5760 >= 2400) {
            notes[i] = 0.0f;
        }
    }
    const Readouts readouts = computeReadouts(notes, kSampleRate);
    ASSERT_TRUE(readouts.frequencyHz.has_value());
    EXPECT_NEAR(readouts.frequencyHz.value(), 100.0, 0.5);
}

// Nothing to measure: no value, not the level of silence.
TEST(Readouts, HandleAnEmptySpan)
{
    const Readouts readouts = computeReadouts({}, kSampleRate);
    EXPECT_TRUE(std::isnan(readouts.peak));
    EXPECT_TRUE(std::isnan(readouts.rms));
    EXPECT_FALSE(readouts.frequencyHz);
}

} // namespace
} // namespace orizon::engine
