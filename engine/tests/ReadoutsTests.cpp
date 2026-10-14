#include "SampleRates.h"
#include "engine/Readouts.h"
#include "protocol/Protocol.h"

#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <optional>
#include <random>
#include <string>
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

// Samples that are not finite (from a broken signal upstream) are measured around and counted:
// the readouts are the tone's.
TEST(Readouts, MeasureAroundSamplesThatAreNotFinite)
{
    std::vector<float> samples(9600);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 *
                                                        static_cast<double>(i) / kSampleRate));
    }
    const Readouts clean = computeReadouts(samples, kSampleRate);
    for (const std::size_t i : {100U, 2000U, 4321U}) {
        samples[i] = std::numeric_limits<float>::quiet_NaN();
    }
    samples[5000] = std::numeric_limits<float>::infinity();
    samples[7000] = -std::numeric_limits<float>::infinity();
    const Readouts readouts = computeReadouts(samples, kSampleRate);
    EXPECT_EQ(readouts.invalid, 5U);
    EXPECT_NEAR(readouts.peak, clean.peak, 1e-6f);
    EXPECT_NEAR(readouts.rms, clean.rms, 1e-3f);
    ASSERT_TRUE(readouts.frequencyHz.has_value());
    EXPECT_NEAR(*readouts.frequencyHz, 1000.0, 1.0);
}

// Nothing to measure: no value, not the level of silence.
TEST(Readouts, HandleAnEmptySpan)
{
    const Readouts readouts = computeReadouts({}, kSampleRate);
    EXPECT_TRUE(std::isnan(readouts.peak));
    EXPECT_TRUE(std::isnan(readouts.rms));
    EXPECT_FALSE(readouts.frequencyHz);
}

// No sample finite: nothing to measure either, and every one counted.
TEST(Readouts, HandleASpanWithNothingFinite)
{
    std::vector<float> samples(960, std::numeric_limits<float>::quiet_NaN());
    samples[10] = std::numeric_limits<float>::infinity();
    samples[20] = -std::numeric_limits<float>::infinity();
    const Readouts readouts = computeReadouts(samples, kSampleRate);
    EXPECT_TRUE(std::isnan(readouts.peak));
    EXPECT_TRUE(std::isnan(readouts.rms));
    EXPECT_FALSE(readouts.frequencyHz);
    EXPECT_EQ(readouts.invalid, 960U);
}

// The shortest span the time span setting allows.
double shortestSpan()
{
    return protocol::kSettings.at(static_cast<std::size_t>(protocol::SettingId::kTimeSpan)).minimum;
}

// Tones as instruments and mixes give them, at every sample rate: not perfectly periodic, offset
// from zero, or high in a short span.
class RealTones : public ::testing::TestWithParam<double>
{
protected:
    // `seconds` of a tone at `frequency`, whose pitch swings by `vibrato` (a fraction) five times
    // a second, offset by `dc`, with uniform noise of up to `noise`.
    [[nodiscard]] std::vector<float> make(double frequency, double seconds, double amplitude,
                                          double vibrato = 0.0, double dc = 0.0,
                                          float noise = 0.0f) const
    {
        const double rate = GetParam();
        std::mt19937 random(7); // NOLINT(bugprone-random-generator-seed): repeatable on purpose.
        std::uniform_real_distribution<float> uniform(-noise, noise);
        std::vector<float> samples(static_cast<std::size_t>(std::llround(seconds * rate)));
        double phase = 0.0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const double time = static_cast<double>(i) / rate;
            const double now =
                frequency * (1.0 + vibrato * std::sin(2.0 * std::numbers::pi * 5.0 * time));
            samples[i] = static_cast<float>(dc + amplitude * std::sin(phase)) +
                         (noise > 0.0f ? uniform(random) : 0.0f);
            phase += 2.0 * std::numbers::pi * now / rate;
        }
        return samples;
    }

    [[nodiscard]] std::optional<double> frequencyOf(const std::vector<float>& samples) const
    {
        return computeReadouts(samples, GetParam()).frequencyHz;
    }
};

TEST_P(RealTones, FindTheFrequencyOfANoisyTone)
{
    const auto found = frequencyOf(make(440.0, 0.2, 0.5, 0.0, 0.0, 0.02f));
    ASSERT_TRUE(found.has_value()) << "a little noise on a tone leaves its pitch";
    EXPECT_NEAR(*found, 440.0, 4.4);
}

// A singer's or a violinist's vibrato swings the pitch by about half a semitone either way.
TEST_P(RealTones, FindTheFrequencyOfAToneWithVibrato)
{
    const double halfSemitone = std::pow(2.0, 1.0 / 24.0) - 1.0;
    const auto found = frequencyOf(make(440.0, 0.5, 0.5, halfSemitone));
    ASSERT_TRUE(found.has_value()) << "a note with vibrato still has its pitch";
    EXPECT_NEAR(*found, 440.0, 4.4);
}

// The shortest span of a tone riding on a DC offset: its cycles cross the tone's own
// mean level, not zero.
TEST_P(RealTones, FindTheFrequencyOfAnOffsetToneInTheShortestSpan)
{
    const auto found = frequencyOf(make(6000.0, shortestSpan(), 0.2, 0.0, 0.5));
    ASSERT_TRUE(found.has_value());
    EXPECT_NEAR(*found, 6000.0, 60.0);
}

TEST_P(RealTones, FindTheFrequencyOfAHighToneInTheShortestSpan)
{
    const auto found = frequencyOf(make(5000.0, shortestSpan(), 0.5));
    ASSERT_TRUE(found.has_value()) << "a few cycles of 5 kHz";
    EXPECT_NEAR(*found, 5000.0, 50.0);
}

INSTANTIATE_TEST_SUITE_P(EveryRate, RealTones, ::testing::ValuesIn(kSampleRates),
                         [](const ::testing::TestParamInfo<double>& info) {
                             return std::to_string(static_cast<int>(info.param)) + "Hz";
                         });

} // namespace
} // namespace orizon::engine
