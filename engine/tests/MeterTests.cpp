#include "SampleRates.h"
#include "engine/Meter.h"
#include "protocol/Protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <gtest/gtest.h>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace orizon::engine {
namespace {

namespace protocol = orizon::protocol;

// A track's channels, positions [0, size) of a span; the readout channel is Left unless given.
struct Track
{
    std::vector<float> left{};
    std::vector<float> right{}; // Empty for a mono track.
    std::vector<float> view{};  // Empty: Left.
};

MeterBlocks::Reader readerOf(const Track& track, std::ptrdiff_t origin = 0)
{
    return [&track, origin](std::ptrdiff_t first, std::span<float> view, std::span<float> left,
                            std::span<float> right) {
        const auto read = [&](const std::vector<float>& from, std::span<float> into) {
            for (std::size_t index = 0; index < into.size(); ++index) {
                const std::ptrdiff_t position = first + origin + static_cast<std::ptrdiff_t>(index);
                if (position < 0 || std::cmp_greater_equal(position, from.size())) {
                    return false;
                }
                into[index] = from[static_cast<std::size_t>(position)];
            }
            return true;
        };
        return read(track.view.empty() ? track.left : track.view, view) && read(track.left, left) &&
               (right.empty() || read(track.right, right));
    };
}

std::size_t blockOf(double sampleRate)
{
    return static_cast<std::size_t>(std::lround(0.01 * sampleRate));
}

// What `blocks` measure over positions [first, end); no value (NaN) where they measure nothing.
Readouts measureOf(MeterBlocks& blocks, std::size_t first, std::size_t end,
                   std::span<const std::size_t> joins, const MeterBlocks::Reader& read)
{
    return blocks.measure(first, end, joins, read).value_or(Readouts{});
}

// The readouts of positions [first, end) of a whole track, its blocks measured from its start.
Readouts measured(const Track& track, double sampleRate, std::size_t first, std::size_t end,
                  std::span<const std::size_t> joins = {})
{
    MeterBlocks blocks;
    const std::size_t count = track.left.size();
    blocks.reset(count, blockOf(sampleRate), sampleRate, track.right.empty() ? 1 : 2, 0,
                 static_cast<std::ptrdiff_t>(count));
    const MeterBlocks::Reader read = readerOf(track);
    EXPECT_TRUE(blocks.update(0, count, joins, read));
    return measureOf(blocks, first, end, joins, read);
}

Readouts measuredWhole(const Track& track, double sampleRate)
{
    return measured(track, sampleRate, 0, track.left.size());
}

std::vector<float> tone(double frequency, double seconds, double amplitude, double sampleRate,
                        double phase = 0.0)
{
    std::vector<float> samples(static_cast<std::size_t>(std::llround(seconds * sampleRate)));
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] =
            static_cast<float>(amplitude * std::sin((2.0 * std::numbers::pi * frequency *
                                                     static_cast<double>(index) / sampleRate) +
                                                    phase));
    }
    return samples;
}

double dbOf(double linear)
{
    return 20.0 * std::log10(linear);
}

class MeterAtRate : public testing::TestWithParam<double>
{
};

std::string rateName(const testing::TestParamInfo<double>& info)
{
    return std::to_string(static_cast<int>(info.param)) + "Hz";
}

// The K-weighting the standard tabulates at 48 kHz (BS.1770-5, Tables 1 and 2): the analog
// prototype gives it, so other rates get the same response.
TEST(KWeighting, MatchesTheStandardsCoefficientsAt48kHz)
{
    const KWeighting weighting(48000.0);
    EXPECT_NEAR(weighting.shelf().b0, 1.53512485958697, 1e-10);
    EXPECT_NEAR(weighting.shelf().b1, -2.69169618940638, 1e-10);
    EXPECT_NEAR(weighting.shelf().b2, 1.19839281085285, 1e-10);
    EXPECT_NEAR(weighting.shelf().a1, -1.69065929318241, 1e-10);
    EXPECT_NEAR(weighting.shelf().a2, 0.73248077421585, 1e-10);
    EXPECT_NEAR(weighting.highPass().a1, -1.99004745483398, 1e-10);
    EXPECT_NEAR(weighting.highPass().a2, 0.99007225036621, 1e-10);
}

// A steady tone reads its amplitude, within what BS.1770 Annex 2 allows: never lower than the
// largest gap between the points read leaves a crest (half a gap off the nearest point, so
// cos(π f / (n fs)) with n points a sample) less the standard filter's gain error, and above it by
// no more than the filter's ripple. Its samples alone can read far lower at a tone locked to the
// rate, which is what the true peak is for.
TEST_P(MeterAtRate, TruePeakReadsASteadyTonesAmplitude)
{
    const double rate = GetParam();
    const double points = rate < protocol::kTruePeakHalfRateHz     ? 4.0
                          : rate < protocol::kTruePeakSampleRateHz ? 2.0
                                                                   : 1.0;
    for (const double frequency : {1000.0, 4000.0, 12000.0, 20000.0}) {
        for (int step = 0; step < 64; ++step) {
            const double phase = std::numbers::pi * step / 64.0;
            const Track track{.left = tone(frequency, 0.05, 0.5, rate, phase)};
            const Readouts readouts = measuredWhole(track, rate);
            // The standard's phases 1 and 2 have a gain of 0.973 (−0.24 dB).
            const double lowest =
                dbOf(std::cos(std::numbers::pi * frequency / (points * rate))) - 0.25;
            const double read = dbOf(readouts.truePeak / 0.5);
            EXPECT_GE(read, lowest) << frequency << " Hz, phase " << phase;
            EXPECT_LE(read, 0.25) << frequency << " Hz, phase " << phase;
            EXPECT_GE(readouts.truePeak, readouts.peak);
        }
    }
}

// A tone at a quarter of the rate sampled 45° off its crest: its samples read 3 dB below the
// crest, the true peak within 0.2 dB of it, where the interpolator reads between the samples.
TEST_P(MeterAtRate, TruePeakReadsACrestBetweenTheSamples)
{
    const double rate = GetParam();
    const Track track{.left = tone(rate / 4.0, 0.05, 0.5, rate, std::numbers::pi / 4.0)};
    const Readouts readouts = measuredWhole(track, rate);
    EXPECT_NEAR(dbOf(readouts.peak / 0.5), dbOf(std::numbers::sqrt2 / 2.0), 0.01);
    if (rate < protocol::kTruePeakHalfRateHz) {
        EXPECT_NEAR(dbOf(readouts.truePeak / 0.5), 0.0, 0.2);
    } else if (rate < protocol::kTruePeakSampleRateHz) {
        // Two points a sample: within half a gap of the crest, less the filter's gain error.
        EXPECT_GE(dbOf(readouts.truePeak / 0.5), dbOf(std::cos(std::numbers::pi / 8.0)) - 0.25);
    }
}

// A 1 kHz sine at −23 dBFS in both channels reads −23 LUFS: BS.1770's calibration (a full-scale
// sine in one channel reads −3.01, each channel adding its power).
TEST_P(MeterAtRate, LoudnessOfASineInBothChannels)
{
    const double rate = GetParam();
    const double amplitude = std::pow(10.0, -23.0 / 20.0);
    const Track track{.left = tone(1000.0, 2.0, amplitude, rate),
                      .right = tone(1000.0, 2.0, amplitude, rate)};
    EXPECT_NEAR(measuredWhole(track, rate).loudness, -23.0, 0.1);
}

// A mono track is its one channel: a full-scale 1 kHz sine reads −3.01 LUFS.
TEST_P(MeterAtRate, LoudnessOfAMonoTrackIsItsChannel)
{
    const double rate = GetParam();
    const Track track{.left = tone(1000.0, 2.0, 1.0, rate)};
    EXPECT_NEAR(measuredWhole(track, rate).loudness, -3.01, 0.1);
}

// Gating: silence and passages more than 10 LU below the rest do not pull the loudness down,
// passages within 10 LU do.
TEST_P(MeterAtRate, LoudnessLeavesOutSilenceAndQuietPassages)
{
    const double rate = GetParam();
    const double loud = std::pow(10.0, -20.0 / 20.0);
    const auto passage = [&](double level, double seconds) {
        return tone(1000.0, seconds, level, rate);
    };
    const auto joined = [](std::initializer_list<std::vector<float>> parts) {
        std::vector<float> all;
        for (const auto& part : parts) {
            all.insert(all.end(), part.begin(), part.end());
        }
        return all;
    };
    const std::vector<float> withSilence = joined({passage(loud, 2.0), passage(0.0, 2.0)});
    const std::vector<float> withQuiet =
        joined({passage(loud, 2.0), passage(loud * std::pow(10.0, -30.0 / 20.0), 2.0)});
    const std::vector<float> withNear =
        joined({passage(loud, 2.0), passage(loud * std::pow(10.0, -6.0 / 20.0), 2.0)});
    const double alone = measuredWhole({.left = passage(loud, 2.0)}, rate).loudness;
    // The blocks straddling the change count, as they sound; those wholly in the silence or the
    // passage 30 dB down are left out alike, so the two read the same and near the loud part's.
    const float silenceAfter = measuredWhole({.left = withSilence}, rate).loudness;
    EXPECT_NEAR(measuredWhole({.left = withQuiet}, rate).loudness, silenceAfter, 0.02);
    EXPECT_NEAR(silenceAfter, alone, 0.5);
    EXPECT_LT(measuredWhole({.left = withNear}, rate).loudness, alone - 1.0);
}

// The absolute gate: audio under −70 LUFS has no loudness (a tone at −80 dBFS reads none, as
// digital silence does), and a tail under the gate after a passage it lies within 10 LU of does not
// pull the loudness down, where the relative gate alone would count it: it reads as silence after
// the passage does.
TEST_P(MeterAtRate, LoudnessLeavesOutWhatIsUnderTheAbsoluteGate)
{
    const double rate = GetParam();
    const auto level = [](double decibels) { return std::pow(10.0, decibels / 20.0); };
    EXPECT_TRUE(
        std::isnan(measuredWhole({.left = tone(1000.0, 2.0, level(-80.0), rate)}, rate).loudness))
        << "a tone under the gate";
    // A 1 kHz sine at −63 dBFS in one channel reads −66 LUFS; the tail, 7 LU under it, −73 LUFS.
    std::vector<float> withTail = tone(1000.0, 2.0, level(-63.0), rate);
    std::vector<float> withSilence = withTail;
    const std::vector<float> tail = tone(1000.0, 2.0, level(-70.0), rate);
    withTail.insert(withTail.end(), tail.begin(), tail.end());
    withSilence.resize(withTail.size(), 0.0f);
    const float silenceAfter = measuredWhole({.left = withSilence}, rate).loudness;
    ASSERT_NEAR(silenceAfter, -66.0, 0.5);
    EXPECT_NEAR(measuredWhole({.left = withTail}, rate).loudness, silenceAfter, 0.3)
        << "the tail under the gate counted";
}

// A view shorter than one 400 ms block has no loudness; exactly one block has; a 499 ms view holds
// one block and a 500 ms view two, the second taking in the last 100 ms.
TEST_P(MeterAtRate, LoudnessCountsWholeBlocksOnly)
{
    const double rate = GetParam();
    const auto samples = [&](double seconds) {
        return static_cast<std::size_t>(std::llround(seconds * rate));
    };
    const std::size_t block = samples(protocol::kLoudnessBlockSeconds);
    // Quiet for 400 ms, loud for the next 100 ms: only a second block reads the loud part.
    std::vector<float> left = tone(1000.0, 0.4, 0.01, rate);
    const std::vector<float> loud = tone(1000.0, 0.1, 1.0, rate);
    left.insert(left.end(), loud.begin(), loud.end());
    const Track track{.left = left};
    EXPECT_TRUE(std::isnan(measured(track, rate, 0, block - 1).loudness));
    const float one = measured(track, rate, 0, block).loudness;
    EXPECT_FALSE(std::isnan(one));
    EXPECT_FLOAT_EQ(measured(track, rate, 0, samples(0.499)).loudness, one);
    EXPECT_GT(measured(track, rate, 0, samples(0.5)).loudness, one + 3.0f);
}

TEST_P(MeterAtRate, SilenceHasNoLoudnessCorrelationOrTruePeakAbovePeak)
{
    const double rate = GetParam();
    const Track track{.left = std::vector<float>(static_cast<std::size_t>(rate), 0.0f),
                      .right = std::vector<float>(static_cast<std::size_t>(rate), 0.0f)};
    const Readouts readouts = measuredWhole(track, rate);
    EXPECT_TRUE(std::isnan(readouts.loudness));
    EXPECT_TRUE(std::isnan(readouts.correlation));
    EXPECT_EQ(readouts.peak, 0.0f);
    EXPECT_EQ(readouts.truePeak, 0.0f);
}

// The DC offset is the mean, once the view holds four periods of the lowest audible note.
TEST_P(MeterAtRate, DcOffsetOverTheMinimumLength)
{
    const double rate = GetParam();
    const auto minimum =
        static_cast<std::size_t>(std::llround(protocol::kDcOffsetMinimumSeconds * rate));
    const Track constant{.left = std::vector<float>(minimum * 2, 0.25f)};
    EXPECT_FLOAT_EQ(measured(constant, rate, 0, minimum).dcOffset, 0.25f);
    EXPECT_TRUE(std::isnan(measured(constant, rate, 0, minimum - 1).dcOffset));
    // A tone of four periods over exactly the minimum (20 Hz), from any phase: within A / (4π).
    for (int step = 0; step < 8; ++step) {
        const Track low{.left = tone(4.0 / protocol::kDcOffsetMinimumSeconds, 0.3, 1.0, rate,
                                     std::numbers::pi * step / 4.0)};
        EXPECT_LE(std::abs(measured(low, rate, 0, minimum).dcOffset),
                  1.0 / (4.0 * std::numbers::pi) + 1e-3);
    }
}

// Correlation: +1 for the same signal in both channels, −1 for one flipped, near 0 for unrelated
// noise; none for a mono track or with one channel silent.
TEST_P(MeterAtRate, CorrelationOfLeftAndRight)
{
    const double rate = GetParam();
    const std::vector<float> signal = tone(440.0, 0.5, 0.5, rate);
    std::vector<float> flipped(signal.size());
    std::ranges::transform(signal, flipped.begin(), [](float sample) { return -sample; });
    std::mt19937 random(7); // NOLINT(bugprone-random-generator-seed): reproducible
    std::normal_distribution<float> noise(0.0f, 0.1f);
    std::vector<float> first(signal.size());
    std::vector<float> second(signal.size());
    std::ranges::generate(first, [&] { return noise(random); });
    std::ranges::generate(second, [&] { return noise(random); });
    EXPECT_NEAR(measuredWhole({.left = signal, .right = signal}, rate).correlation, 1.0, 1e-6);
    EXPECT_NEAR(measuredWhole({.left = signal, .right = flipped}, rate).correlation, -1.0, 1e-6);
    EXPECT_NEAR(measuredWhole({.left = first, .right = second}, rate).correlation, 0.0, 0.05);
    EXPECT_TRUE(std::isnan(measuredWhole({.left = signal}, rate).correlation));
    const std::vector<float> silent(signal.size(), 0.0f);
    EXPECT_TRUE(std::isnan(measuredWhole({.left = signal, .right = silent}, rate).correlation));
}

// Correlation counts only where both channels sound: Left alone for a while, then both alike,
// still reads +1.
TEST_P(MeterAtRate, CorrelationWhereBothSound)
{
    const double rate = GetParam();
    std::vector<float> left = tone(440.0, 0.5, 0.5, rate);
    std::vector<float> right(left.size(), 0.0f);
    const std::vector<float> both = tone(440.0, 0.5, 0.5, rate);
    left.insert(left.end(), both.begin(), both.end());
    right.insert(right.end(), both.begin(), both.end());
    EXPECT_NEAR(measuredWhole({.left = left, .right = right}, rate).correlation, 1.0, 1e-6);
    // The channel that rests need not be digitally silent: another tone under the analysis gate
    // (−100 dBFS), or audible but more than 60 dB under that channel's loudest part (−75 dBFS
    // against −6), does not sound with Left either, and its part still does not count (it would
    // read 0.71).
    for (const double resting : {-100.0, -75.0}) {
        SCOPED_TRACE(resting);
        const std::vector<float> quiet = tone(660.0, 0.5, std::pow(10.0, resting / 20.0), rate);
        std::ranges::copy(quiet, right.begin());
        EXPECT_NEAR(measuredWhole({.left = left, .right = right}, rate).correlation, 1.0, 1e-3);
    }
    // A channel that never rises above the analysis gate in the view (dither alone) does not
    // sound with the other anywhere: no correlation, where its level relative to its own loudest
    // part would count all of it.
    const std::vector<float> dither = tone(440.0, 1.0, std::pow(10.0, -100.0 / 20.0), rate);
    EXPECT_TRUE(std::isnan(measuredWhole({.left = left, .right = dither}, rate).correlation));
}

// The readouts of positions [first, end), measured directly from the samples, one at a time: the
// filters run from the track's start, the true peak reads its neighbours either side.
Readouts direct(const Track& track, double rate, std::size_t first, std::size_t end)
{
    const std::vector<float>& view = track.view.empty() ? track.left : track.view;
    const bool stereo = !track.right.empty();
    const KWeighting weighting(rate);
    const TruePeak interpolator(rate);
    KWeighting::State leftState{};
    KWeighting::State rightState{};
    std::vector<double> weighted(end + 1, 0.0); // Energy before each position.
    for (std::size_t position = 0; position < end; ++position) {
        const auto finite = [](float sample) { return std::isfinite(sample) ? sample : 0.0f; };
        const double left = weighting.process(finite(track.left[position]), leftState);
        double energy = left * left;
        if (stereo) {
            const double right = weighting.process(finite(track.right[position]), rightState);
            energy += right * right;
        }
        weighted[position + 1] = weighted[position] + energy;
    }
    Readouts readouts;
    double sumSquares = 0.0;
    double sum = 0.0;
    float peak = 0.0f;
    double truePeak = 0.0;
    std::size_t finite = 0;
    for (std::size_t position = first; position < end; ++position) {
        const float sample = view[position];
        if (std::isfinite(sample)) {
            peak = std::max(peak, std::abs(sample));
            sumSquares += static_cast<double>(sample) * sample;
            sum += sample;
            ++finite;
        }
        if (position + 1 < end && position >= TruePeak::kBefore &&
            position + TruePeak::kAfter < view.size()) {
            const std::span<const float, TruePeak::kTaps> around(
                view.data() + position - TruePeak::kBefore, TruePeak::kTaps);
            truePeak = std::max(truePeak, interpolator.between(around));
        }
    }
    if (finite > 0) {
        readouts.peak = peak;
        readouts.truePeak = static_cast<float>(std::max(static_cast<double>(peak), truePeak));
        readouts.rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(finite)));
        if (static_cast<double>(end - first) >=
            std::round(protocol::kDcOffsetMinimumSeconds * rate)) {
            readouts.dcOffset = static_cast<float>(sum / static_cast<double>(finite));
        }
    }
    const auto block =
        static_cast<std::size_t>(std::llround(protocol::kLoudnessBlockSeconds * rate));
    std::vector<double> powers;
    for (std::size_t index = 0;; ++index) {
        const std::size_t start =
            first + static_cast<std::size_t>(std::llround(static_cast<double>(index) *
                                                          protocol::kLoudnessStepSeconds * rate));
        if (start + block > end) {
            break;
        }
        powers.push_back((weighted[start + block] - weighted[start]) / static_cast<double>(block));
    }
    const auto lufs = [](double power) { return -0.691 + (10.0 * std::log10(power)); };
    const auto mean = [&](double gate) {
        double total = 0.0;
        std::size_t counted = 0;
        for (const double power : powers) {
            if (power > 0.0 && lufs(power) > gate) {
                total += power;
                ++counted;
            }
        }
        return counted > 0 ? total / static_cast<double>(counted) : 0.0;
    };
    const double absolute = mean(-70.0);
    const double relative = absolute > 0.0 ? std::max(lufs(absolute) - 10.0, -70.0) : 0.0;
    if (absolute > 0.0 && mean(relative) > 0.0) {
        readouts.loudness = static_cast<float>(lufs(mean(relative)));
    }
    return readouts;
}

void expectSame(const Readouts& measured, const Readouts& expected)
{
    const auto same = [](float a, float b, const char* name) {
        if (std::isnan(b)) {
            EXPECT_TRUE(std::isnan(a)) << name;
        } else {
            EXPECT_NEAR(a, b, 1e-5 * std::max(1.0f, std::abs(b))) << name;
        }
    };
    same(measured.peak, expected.peak, "peak");
    same(measured.rms, expected.rms, "rms");
    same(measured.truePeak, expected.truePeak, "true peak");
    same(measured.dcOffset, expected.dcOffset, "dc offset");
    same(measured.loudness, expected.loudness, "loudness");
}

// Noise shaped like music: loud and quiet stretches, Left and Right partly alike.
Track musicLike(double rate, double seconds, unsigned seed)
{
    std::mt19937 random(seed);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    const auto count = static_cast<std::size_t>(seconds * rate);
    Track track{.left = std::vector<float>(count), .right = std::vector<float>(count)};
    for (std::size_t index = 0; index < count; ++index) {
        const double envelope =
            0.05 + (0.3 * (0.5 + (0.5 * std::sin(static_cast<double>(index) / rate * 7.0))));
        const float common = noise(random);
        track.left[index] = static_cast<float>(envelope * (common + (0.5f * noise(random))));
        track.right[index] = static_cast<float>(envelope * (common + (0.5f * noise(random))));
    }
    return track;
}

// Any visible part, its edges inside blocks, across the loudness steps, within one block: the
// blocks and the samples of the edge blocks read what a direct measurement reads.
TEST_P(MeterAtRate, VisiblePartFromBlocksMatchesADirectMeasurement)
{
    const double rate = GetParam();
    const Track track = musicLike(rate, 1.5, 3);
    const std::size_t count = track.left.size();
    const std::size_t block = blockOf(rate);
    std::mt19937 random(11); // NOLINT(bugprone-random-generator-seed): reproducible
    std::vector<std::pair<std::size_t, std::size_t>> parts{
        {0, count}, {block / 2, block / 2 + 7}, {block * 3 + 1, count - block - 3}};
    std::uniform_int_distribution<std::size_t> anywhere(0, count - 1);
    for (int index = 0; index < 6; ++index) {
        std::size_t a = anywhere(random);
        std::size_t b = anywhere(random);
        parts.emplace_back(std::min(a, b), std::max(a, b) + 1);
    }
    MeterBlocks blocks;
    blocks.reset(count, block, rate, 2, 0, static_cast<std::ptrdiff_t>(count));
    const MeterBlocks::Reader read = readerOf(track);
    ASSERT_TRUE(blocks.update(0, count, {}, read));
    for (const auto& [first, end] : parts) {
        SCOPED_TRACE(testing::Message() << first << " to " << end);
        expectSame(measureOf(blocks, first, end, {}, read), direct(track, rate, first, end));
    }
}

// After a change in the middle, blocks kept up to date read as blocks measured afresh: the
// K-weighting state passed on, and a change just past a block's start, whose previous block's last
// points read it (a quiet previous block and an impulse there, so a stale true peak would show).
TEST_P(MeterAtRate, UpdatedBlocksMatchFreshOnes)
{
    const double rate = GetParam();
    const std::size_t block = blockOf(rate);
    for (std::size_t offset = 0; offset <= 7; ++offset) {
        SCOPED_TRACE(testing::Message() << "offset " << offset);
        Track track = musicLike(rate, 0.5, 5);
        const std::size_t count = track.left.size();
        const std::size_t changed = (block * 20) + offset;
        for (std::size_t index = changed - block; index < changed; ++index) {
            track.left[index] = 0.0f;
            track.right[index] = 0.0f;
        }
        MeterBlocks kept;
        kept.reset(count, block, rate, 2, 0, static_cast<std::ptrdiff_t>(count));
        const MeterBlocks::Reader read = readerOf(track);
        ASSERT_TRUE(kept.update(0, count, {}, read));
        // The change: an impulse, then different audio for a while.
        track.left[changed] = 0.9f;
        track.right[changed] = -0.9f;
        for (std::size_t index = changed + 1; index < changed + (block * 3); ++index) {
            track.left[index] *= -0.5f;
        }
        ASSERT_TRUE(kept.update(changed, changed + (block * 3), {}, read));
        MeterBlocks fresh;
        fresh.reset(count, block, rate, 2, 0, static_cast<std::ptrdiff_t>(count));
        ASSERT_TRUE(fresh.update(0, count, {}, read));
        for (std::size_t index = 0; index < kept.blocks().size(); ++index) {
            const MeterBlocks::Block& a = kept.blocks()[index];
            const MeterBlocks::Block& b = fresh.blocks()[index];
            EXPECT_FLOAT_EQ(a.truePeak, b.truePeak) << "block " << index;
            EXPECT_FLOAT_EQ(a.truePeakAfter, b.truePeakAfter) << "block " << index;
            EXPECT_NEAR(a.weighted, b.weighted, 1e-9 * std::max(1.0, b.weighted))
                << "block " << index;
        }
    }
}

// Blocks that follow audio going on, as a capture's in Trigger and Free: grown in steps of every
// kind (a few samples, under a block, several blocks, ending on a block's edge and just past one)
// and dropping blocks from their front as they go, they read over any part what blocks built
// afresh over the audio they now cover read there, so a readout costs the new audio alone. The
// reader is asked for nothing before the block the old end lies in (less the true peak's reach)
// and nothing past what is readable.
TEST_P(MeterAtRate, BlocksThatFollowTheAudioReadAsBlocksBuiltAfresh)
{
    const double rate = GetParam();
    const Track track = musicLike(rate, 1.6, 21);
    const std::size_t total = track.left.size();
    const std::size_t block = blockOf(rate);
    const std::size_t keep = block * 60; // 0.6 s kept, so a 400 ms loudness block fits.
    std::size_t origin = 0;              // The track's index of position 0.
    std::size_t end = (block * 30) + 17;
    std::ptrdiff_t lowestRead = 0;
    std::ptrdiff_t highestRead = 0;
    const auto readerAt = [&](std::size_t from) {
        return MeterBlocks::Reader([&, from](std::ptrdiff_t first, std::span<float> view,
                                             std::span<float> left, std::span<float> right) {
            const std::size_t size = std::max({view.size(), left.size(), right.size()});
            lowestRead = std::min(lowestRead, first);
            highestRead = std::max(highestRead, first + static_cast<std::ptrdiff_t>(size));
            return readerOf(track, static_cast<std::ptrdiff_t>(from))(first, view, left, right);
        });
    };
    MeterBlocks followed;
    followed.reset(end, block, rate, 2, 0, static_cast<std::ptrdiff_t>(end));
    ASSERT_TRUE(followed.update(0, end, {}, readerAt(origin)));

    std::mt19937 random(31); // NOLINT(bugprone-random-generator-seed): reproducible
    const std::vector<std::size_t> steps{
        3,         block - 20, block * 4 + 9, block - 26,      block * 2,
        block / 2, 5,          block * 7 + 1, block,           block * 12 + 3,
        block - 1, 2,          block * 9,     block * 5 + 250, block * 3};
    for (const std::size_t step : steps) {
        const std::size_t before = end - origin;
        end = std::min(end + step, total);
        // Whole blocks before what is kept go.
        const std::size_t dropped = end - origin > keep ? (end - origin - keep) / block : 0;
        origin += dropped * block;
        const std::size_t count = end - origin;
        const auto readableFirst = -static_cast<std::ptrdiff_t>(std::min<std::size_t>(origin, 8));
        lowestRead = static_cast<std::ptrdiff_t>(count);
        highestRead = 0;
        ASSERT_TRUE(followed.follow(dropped, count, readableFirst,
                                    static_cast<std::ptrdiff_t>(count), readerAt(origin)));
        ASSERT_EQ(followed.sampleCount(), count);
        SCOPED_TRACE(testing::Message() << "to " << end << ", from " << origin);
        // Only the new audio is read: from the block the old end lay in, or the one before it
        // where the old end lay within the true peak's reach of that block's start.
        const std::size_t oldEnd = before - (dropped * block);
        const std::size_t oldBlock = (oldEnd / block) * block;
        const std::size_t firstMeasured =
            oldEnd - oldBlock < TruePeak::kAfter && oldBlock >= block ? oldBlock - block : oldBlock;
        EXPECT_GE(lowestRead, static_cast<std::ptrdiff_t>(firstMeasured) -
                                  static_cast<std::ptrdiff_t>(TruePeak::kBefore));
        EXPECT_LE(highestRead, static_cast<std::ptrdiff_t>(count));

        // Built afresh over the same audio, the filters settled on all of it before, as the
        // followed blocks' are.
        MeterBlocks fresh;
        fresh.reset(count, block, rate, 2, -static_cast<std::ptrdiff_t>(origin),
                    static_cast<std::ptrdiff_t>(count));
        const MeterBlocks::Reader read = readerOf(track, static_cast<std::ptrdiff_t>(origin));
        ASSERT_TRUE(fresh.prime(origin, read));
        ASSERT_TRUE(fresh.update(0, count, {}, read));
        std::uniform_int_distribution<std::size_t> anywhere(0, count - 1);
        std::vector<std::pair<std::size_t, std::size_t>> parts{
            {0, count}, {count - std::min(count, block + 3), count}};
        for (int index = 0; index < 4; ++index) {
            const std::size_t a = anywhere(random);
            const std::size_t b = anywhere(random);
            parts.emplace_back(std::min(a, b), std::max(a, b) + 1);
        }
        for (const auto& [first, last] : parts) {
            SCOPED_TRACE(testing::Message() << first << " to " << last);
            const Readouts kept = measureOf(followed, first, last, {}, read);
            const Readouts expected = measureOf(fresh, first, last, {}, read);
            expectSame(kept, expected);
            if (std::isnan(expected.correlation)) {
                EXPECT_TRUE(std::isnan(kept.correlation));
            } else {
                EXPECT_NEAR(kept.correlation, expected.correlation, 1e-6);
            }
            EXPECT_EQ(kept.invalid, expected.invalid);
        }
    }
    EXPECT_GT(origin, 0U) << "the blocks never slid";
}

// Following needs blocks to follow: none measured yet, or all of them dropped, is refused, and the
// statistics then fit nothing, so that their owner builds them afresh.
TEST(MeterBlocksTest, FollowNothingOnceEveryBlockIsDropped)
{
    const double rate = 48000.0;
    const Track track = musicLike(rate, 0.5, 4);
    const std::size_t count = track.left.size();
    const std::size_t block = blockOf(rate);
    const MeterBlocks::Reader read = readerOf(track);
    MeterBlocks never;
    EXPECT_FALSE(never.follow(0, count, 0, static_cast<std::ptrdiff_t>(count), read));
    MeterBlocks blocks;
    blocks.reset(block * 10, block, rate, 2, 0, static_cast<std::ptrdiff_t>(block * 10));
    ASSERT_TRUE(blocks.update(0, block * 10, {}, read));
    EXPECT_FALSE(blocks.follow(10, block * 4, 0, static_cast<std::ptrdiff_t>(block * 4), read));
    EXPECT_FALSE(blocks.measuring());
    EXPECT_FALSE(blocks.fits(block * 4, block, rate, 2));
}

// A read that fails while the blocks follow (the capture overwritten under it, or resized) leaves
// nothing half-measured to read from: the statistics fit nothing and measure nothing, so that
// their owner builds them afresh, as it does when a read fails in a sweep.
TEST(MeterBlocksTest, FollowLeavesNothingToReadWhenAReadFails)
{
    const double rate = 48000.0;
    const Track track = musicLike(rate, 0.6, 8);
    const std::size_t block = blockOf(rate);
    const std::size_t count = block * 20;
    const MeterBlocks::Reader read = readerOf(track);
    MeterBlocks blocks;
    blocks.reset(count, block, rate, 2, 0, static_cast<std::ptrdiff_t>(count));
    ASSERT_TRUE(blocks.update(0, count, {}, read));
    const MeterBlocks::Reader failing = [](std::ptrdiff_t, std::span<float>, std::span<float>,
                                           std::span<float>) { return false; };
    const std::size_t grown = count + (block * 3) + 11;
    EXPECT_FALSE(blocks.follow(4, grown - (block * 4), -5,
                               static_cast<std::ptrdiff_t>(grown - (block * 4)), failing));
    EXPECT_FALSE(blocks.measuring());
    EXPECT_FALSE(blocks.fits(grown - (block * 4), block, rate, 2));
    EXPECT_FALSE(blocks.measure(0, block * 8, {}, read)) << "a reading from blocks half measured";
}

// A view whose edge falls inside a block reads that block's samples. If the read fails there is no
// reading: not one of no level, which the Meter would show as a dash until the next readout is due
// instead of measuring again in the next frame.
TEST(MeterBlocksTest, MeasuresNothingWhenAnEdgeCannotBeRead)
{
    const double rate = 48000.0;
    const Track track = musicLike(rate, 0.6, 8);
    const std::size_t block = blockOf(rate);
    const std::size_t count = block * 20;
    const MeterBlocks::Reader read = readerOf(track);
    MeterBlocks blocks;
    blocks.reset(count, block, rate, 2, 0, static_cast<std::ptrdiff_t>(count));
    ASSERT_TRUE(blocks.update(0, count, {}, read));
    const MeterBlocks::Reader failing = [](std::ptrdiff_t, std::span<float>, std::span<float>,
                                           std::span<float>) { return false; };
    EXPECT_FALSE(blocks.measure(block / 2, count - (block / 2), {}, failing));
    const auto again = blocks.measure(block / 2, count - (block / 2), {}, read);
    ASSERT_TRUE(again) << "the blocks stay as measured for the next try";
    EXPECT_FALSE(std::isnan(again->peak));
    const auto empty = blocks.measure(block, block, {}, failing);
    ASSERT_TRUE(empty) << "an empty part is measured: it has no value";
    EXPECT_TRUE(std::isnan(empty->peak));
}

// The loudness reads again the samples before each gating block's edge that falls inside a 10 ms
// block (the K-weighted energy alone, so the reader is asked for no readout channel). If that read
// fails there is no reading, as for a view's edge: never a loudness from blocks half read.
TEST(MeterBlocksTest, MeasuresNothingWhenALoudnessEdgeCannotBeRead)
{
    const double rate = 48000.0;
    const Track track = musicLike(rate, 1.0, 9);
    const std::size_t block = blockOf(rate);
    const std::size_t count = block * 90;
    const MeterBlocks::Reader read = readerOf(track);
    MeterBlocks blocks;
    blocks.reset(count, block, rate, 2, 0, static_cast<std::ptrdiff_t>(count));
    ASSERT_TRUE(blocks.update(0, count, {}, read));
    const MeterBlocks::Reader weightingFails = [&](std::ptrdiff_t first, std::span<float> view,
                                                   std::span<float> left, std::span<float> right) {
        return !view.empty() && read(first, view, left, right);
    };
    // Gating blocks from whole 10 ms blocks need no samples: the blocks' own sums.
    const auto whole = blocks.measure(block * 10, block * 80, {}, weightingFails);
    ASSERT_TRUE(whole);
    EXPECT_FALSE(std::isnan(whole->loudness));
    // From the middle of a block, each gating block's edges fall inside one.
    EXPECT_FALSE(blocks.measure((block * 10) + (block / 2), block * 80, {}, weightingFails));
    const auto again = blocks.measure((block * 10) + (block / 2), block * 80, {}, read);
    ASSERT_TRUE(again);
    EXPECT_NEAR(again->loudness, whole->loudness, 1.0);
}

// Cleared, as while the Meter is off, the statistics fit nothing and measure nothing, so that a
// Meter switched on again measures afresh, not from the blocks it had before.
TEST(MeterBlocksTest, FitNothingOnceCleared)
{
    const double rate = 48000.0;
    const Track track = musicLike(rate, 0.5, 4);
    const std::size_t count = track.left.size();
    const std::size_t block = blockOf(rate);
    const MeterBlocks::Reader read = readerOf(track);
    MeterBlocks blocks;
    blocks.reset(count, block, rate, 2, 0, static_cast<std::ptrdiff_t>(count));
    ASSERT_TRUE(blocks.update(0, count, {}, read));
    ASSERT_TRUE(blocks.fits(count, block, rate, 2));
    ASSERT_FALSE(std::isnan(measureOf(blocks, 0, count, {}, read).peak));
    blocks.clear();
    EXPECT_FALSE(blocks.fits(count, block, rate, 2));
    EXPECT_FALSE(blocks.measure(0, count, {}, read));
    EXPECT_TRUE(blocks.blocks().empty());
}

// A view of digital silence just after a loud tone reads the filters' tail, settled on the audio
// before the view, as a meter running through the passage would; with silence before it, none.
TEST_P(MeterAtRate, LoudnessSettlesOnTheAudioBeforeTheView)
{
    const double rate = GetParam();
    std::vector<float> left = tone(1000.0, 0.5, 0.7, rate);
    const std::size_t toneEnd = left.size();
    left.resize(toneEnd * 3, 0.0f);
    const Track track{.left = left};
    EXPECT_FALSE(std::isnan(measured(track, rate, toneEnd, left.size()).loudness));
    const Track silent{.left = std::vector<float>(left.size(), 0.0f)};
    EXPECT_TRUE(std::isnan(measured(silent, rate, toneEnd, left.size()).loudness));
    // Primed from audio before the span (the Trigger and Free windows), likewise, also when less
    // than the preroll is there to read.
    for (const std::size_t held : {toneEnd, toneEnd / 50}) {
        MeterBlocks blocks;
        const std::size_t count = left.size() - toneEnd;
        blocks.reset(count, blockOf(rate), rate, 1, -static_cast<std::ptrdiff_t>(held),
                     static_cast<std::ptrdiff_t>(count));
        const MeterBlocks::Reader read = readerOf(track, static_cast<std::ptrdiff_t>(toneEnd));
        ASSERT_TRUE(blocks.prime(static_cast<std::size_t>(0.1 * rate), read));
        ASSERT_TRUE(blocks.update(0, count, {}, read));
        EXPECT_FALSE(std::isnan(measureOf(blocks, 0, count, {}, read).loudness)) << held;
    }
}

// Samples that are not finite are counted where they are visible and left out of every value; one
// before the view leaves its loudness as the clean audio gives it.
TEST_P(MeterAtRate, SamplesThatAreNotFiniteAreLeftOut)
{
    const double rate = GetParam();
    Track track = musicLike(rate, 1.5, 9);
    const Track clean = track;
    const std::size_t count = track.left.size();
    track.left[10] = std::numeric_limits<float>::quiet_NaN();
    track.right[count / 2] = std::numeric_limits<float>::infinity();
    const Readouts readouts = measuredWhole(track, rate);
    EXPECT_EQ(readouts.invalid, 2U) << "Left's and Right's";
    EXPECT_FALSE(std::isnan(readouts.peak));
    EXPECT_FALSE(std::isnan(readouts.loudness));
    EXPECT_FALSE(std::isnan(readouts.correlation));
    const std::size_t later = count / 2 + static_cast<std::size_t>(0.2 * rate);
    const Readouts after = measured(track, rate, later, count);
    EXPECT_EQ(after.invalid, 0U);
    EXPECT_NEAR(after.loudness, measured(clean, rate, later, count).loudness, 1e-3);
}

// A sweep's join, full scale one side and minus full scale the other: the wave is not read across
// it, so the true peak is the samples' level, not an overshoot.
TEST_P(MeterAtRate, TruePeakIsNotReadAcrossAJoin)
{
    const double rate = GetParam();
    const std::size_t count = blockOf(rate) * 4;
    const std::size_t join = count / 2 + 3;
    std::vector<float> left(count);
    for (std::size_t index = 0; index < count; ++index) {
        left[index] = index < join ? 0.9f : -0.9f;
    }
    const std::array joins{join};
    const Readouts readouts = measured({.left = left}, rate, 0, count, joins);
    EXPECT_LE(readouts.truePeak, 0.9f * 1.002f);
}

// Kept up to date as a live sweep is, its playhead's join moving on with each change: the blocks
// match blocks measured afresh, though those after the join are not measured again.
TEST_P(MeterAtRate, UpdatedBlocksMatchFreshOnesAcrossAMovingJoin)
{
    const double rate = GetParam();
    const std::size_t block = blockOf(rate);
    Track track = musicLike(rate, 0.5, 13);
    const std::size_t count = track.left.size();
    MeterBlocks kept;
    kept.reset(count, block, rate, 2, 0, static_cast<std::ptrdiff_t>(count));
    const MeterBlocks::Reader read = readerOf(track);
    std::size_t playhead = block * 5 + 3;
    std::array joins{playhead};
    ASSERT_TRUE(kept.update(0, count, joins, read));
    for (int step = 0; step < 6; ++step) {
        const std::size_t next = playhead + (block * 3 / 2);
        for (std::size_t index = playhead; index < next; ++index) {
            track.left[index] = -track.left[index];
        }
        joins = {next};
        ASSERT_TRUE(kept.update(playhead, next, joins, read));
        playhead = next;
    }
    MeterBlocks fresh;
    fresh.reset(count, block, rate, 2, 0, static_cast<std::ptrdiff_t>(count));
    ASSERT_TRUE(fresh.update(0, count, joins, read));
    for (std::size_t index = 0; index < kept.blocks().size(); ++index) {
        EXPECT_NEAR(kept.blocks()[index].weighted, fresh.blocks()[index].weighted,
                    1e-9 * std::max(1.0, fresh.blocks()[index].weighted))
            << "block " << index;
        EXPECT_FLOAT_EQ(kept.blocks()[index].truePeak, fresh.blocks()[index].truePeak)
            << "block " << index;
    }
    expectSame(measureOf(kept, 0, count, joins, read), measureOf(fresh, 0, count, joins, read));
}

INSTANTIATE_TEST_SUITE_P(Rates, MeterAtRate, testing::ValuesIn(kSampleRates), rateName);

} // namespace
} // namespace orizon::engine
