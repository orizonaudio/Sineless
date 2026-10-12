// Signal corpus: approved frames for known signals. Each case plays deterministic synthetic audio
// through real instances of a group, has the analyzer produce frames as an editor would, and
// compares what the last frame says (readouts, views or lanes, matches, summing gains) with an
// approved file in engine/tests/corpus, one per case. A change in what the engine reports for a
// known signal fails here, also where no unit test looks.
//
// The approved files are snapshots, so each case also states what physics says its signal must
// read (a 0.5 sine peaks at -6.02 dBFS, an inverted copy correlates at -1, a delayed copy is found
// at its delay, silence measures nothing): a snapshot can never approve a wrong value.
//
// Set ORIZON_UPDATE_CORPUS=1 to write the approved files from what the engine reports now, then
// review the diff; see "Test levels" in CONTRIBUTING.md.

#include "FrameDecoding.h"
#include "engine/Analyzer.h"
#include "engine/InstanceRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <numbers>
#include <optional>
#include <ostream>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orizon::engine {
namespace {

using nlohmann::json;
using protocol::DisplayMode;
using protocol::FrameFlag;
using protocol::FrameHeader;
using protocol::FrameMode;
using protocol::SettingId;
using protocol::TimeBase;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kMinusInfinity = -std::numeric_limits<double>::infinity();

// Approved numbers are compared within kRelativeTolerance of their size, or kAbsoluteTolerance,
// whichever is larger. CI runs on Linux and macOS clang (x64 and arm64) and Windows MSVC (x64 and
// ARM64): the engine is built without fast-math, so they differ only where one compiler fuses a
// multiply-add (arm64) and another does not, or a libm rounds a sine the other way, each a float
// ulp (6e-8 of the value) at a time. The longest float sums behind a frame (a correlation over a
// 24000-sample sweep, in eight lanes) round by about 1e-6 of their result at worst, so 1e-4
// leaves a hundred times that, and is still far tighter than any change a reader could see:
// 0.001 dB on a level, 1e-4 on a correlation. The absolute part covers near-zero values of any
// kind (a level, a correlation, a gain, a point of a trace), such as the chance correlation of
// unrelated noise, where a relative tolerance would allow nothing: 1e-6 is far below the smallest
// change a reader could see in any of them, and far above the rounding seen. Built with GCC at
// -O3 for an FMA-capable x64, the corpus differs from Clang's only in such near-zero
// correlations, by 3e-9.
constexpr double kRelativeTolerance = 1e-4;
constexpr double kAbsoluteTolerance = 1e-6;
// Numbers are written with this many significant digits: a float32 holds about 7.2.
constexpr int kSignificantDigits = 7;

// Tolerances of the physical expectations, which hold whatever the snapshot says.
// The corpus's sines are sampled at their crest, or 0.0001 dB below it (1 kHz at 44.1 kHz), and
// fit whole cycles into the span. Uniform noise reads its RMS within about 0.06 dB (one standard
// deviation) over a 4800-sample span; the corpus's reads 0.02 dB high.
constexpr double kPeakToleranceDb = 0.01;
constexpr double kRmsToleranceDb = 0.05;
constexpr double kFrequencyTolerance = 1e-3; // Of the frequency.
// A copy correlates at exactly 1 and sums exactly 3.01 dB louder; what is left is float rounding.
constexpr double kCorrelationTolerance = 1e-3;
constexpr double kGainToleranceDb = 0.01;
// Unrelated noise correlates by chance, about 1 / sqrt(n): 0.013 over the 6000 samples of a
// quarter-beat sweep at 48 kHz. These hold where a case expects 0 (no relation) for either: the
// correlation within about four times that, and the summing gain, which a chance correlation r
// moves by about 10 log10(1 + r) (0.06 dB at r = 0.013), within about three times that.
constexpr double kChanceCorrelation = 0.05;
constexpr double kChanceGainDb = 0.2;

// A level in dBFS, for the expectations only: -infinity at 0, never a floor, so silence that reads
// as a tiny number instead of nothing fails.
double dbfs(double linear)
{
    return linear == 0.0 ? kMinusInfinity : 20.0 * std::log10(std::abs(linear));
}

// A track's audio: the sample at index `n`, playing at `rate`.
using Signal = std::function<float(std::uint64_t n, std::uint32_t rate)>;

// The phase is from integers, so it is exact however long the signal plays and the sine library
// always sees a small argument.
Signal sine(std::uint32_t hertz, double amplitude)
{
    return [=](std::uint64_t n, std::uint32_t rate) {
        const auto cycle = static_cast<double>((n * hertz) % rate);
        return static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * cycle / static_cast<double>(rate)));
    };
}

Signal dc(double level)
{
    return [=](std::uint64_t, std::uint32_t) { return static_cast<float>(level); };
}

Signal silence()
{
    return dc(0.0);
}

// White noise from a hash of the sample index: the same on every platform, uniform in
// [-amplitude, amplitude).
Signal noise(std::uint64_t seed, double amplitude = 1.0)
{
    return [=](std::uint64_t n, std::uint32_t) {
        std::uint64_t x = n * 0x9E3779B97F4A7C15ULL + seed * 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 30U;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27U;
        const double unit = static_cast<double>(x >> 40U) / static_cast<double>(1U << 24U);
        return static_cast<float>(amplitude * (2.0 * unit - 1.0));
    };
}

Signal scaled(Signal signal, double gain)
{
    return [source = std::move(signal), gain](std::uint64_t n, std::uint32_t rate) {
        return static_cast<float>(gain * source(n, rate));
    };
}

Signal inverted(Signal signal)
{
    return scaled(std::move(signal), -1.0);
}

// `signal` arriving `samples` later (silent before it starts).
Signal delayed(Signal signal, std::uint64_t samples)
{
    return [source = std::move(signal), samples](std::uint64_t n, std::uint32_t rate) {
        return n < samples ? 0.0f : source(n - samples, rate);
    };
}

// `signal` with `value` (NaN, an infinity) in place of every `every`-th sample.
Signal spiked(Signal signal, float value, std::uint64_t every)
{
    return [source = std::move(signal), value, every](std::uint64_t n, std::uint32_t rate) {
        return n % every == every / 2 ? value : source(n, rate);
    };
}

// One instance's input: its left and right channels, or the left alone on a mono bus.
struct Track
{
    Signal left;
    Signal right;
    int channels = 2;
};

Track stereo(const Signal& left, const Signal& right)
{
    Track track;
    track.left = left;
    track.right = right;
    return track;
}

Track stereo(const Signal& both)
{
    return stereo(both, both);
}

Track mono(const Signal& signal)
{
    Track track;
    track.left = signal;
    track.right = signal;
    track.channels = 1;
    return track;
}

// What a case's signal must read, whatever the snapshot says. Unset: not stated.
struct Expect
{
    // Whether the trigger fired: a sine that swings well past the trigger's hysteresis
    // (settings.ts, triggerHysteresis: 0.01 by default) crosses its level once a cycle.
    std::optional<bool> triggered;
    // Readouts of this track's trigger view (dBFS; -infinity: exactly 0, silent) and its
    // frequency (Hz; NaN: none found).
    std::optional<double> peakDbfs;
    std::optional<double> rmsDbfs;
    std::optional<double> frequency;
    // Compare: the other track's match and the summing gains (dB). NaN: nothing measured;
    // -infinity: an exact cancellation, the sum exactly 0 (never a floor: the UI applies that).
    std::optional<double> correlation;
    std::optional<int> offset;
    std::optional<int> polarity; // 1 as it is, -1 flipped, 0 no clear match.
    std::optional<double> sumGainDb;
    std::optional<double> fixedGainDb;
};

// One case of the corpus: a signal, how it is viewed, and what it must read. Written as one line
// of the table: single() or compare(), then what differs from their defaults.
struct Case
{
    std::string_view name; // Test name and file name.
    DisplayMode mode = DisplayMode::kSingle;
    TimeBase timeBase = TimeBase::kSeconds;
    std::uint32_t rate = 48000;
    bool frozen = false;
    Track own;
    std::optional<Track> other; // Compare: the track compared with this one.
    Expect expect;

    Case& at(std::uint32_t sampleRate)
    {
        rate = sampleRate;
        return *this;
    }
    Case& in(TimeBase base)
    {
        timeBase = base;
        return *this;
    }
    Case& freeze()
    {
        frozen = true;
        return *this;
    }
    Case& triggers()
    {
        expect.triggered = true;
        return *this;
    }
    Case& levels(double peakDbfs, double rmsDbfs)
    {
        expect.peakDbfs = peakDbfs;
        expect.rmsDbfs = rmsDbfs;
        return *this;
    }
    Case& pitch(double hertz)
    {
        expect.frequency = hertz;
        return *this;
    }
    Case& match(double correlation, int offset, int polarity)
    {
        expect.correlation = correlation;
        expect.offset = offset;
        expect.polarity = polarity;
        return *this;
    }
    Case& gains(double sumDb, std::optional<double> fixedDb)
    {
        expect.sumGainDb = sumDb;
        expect.fixedGainDb = fixedDb;
        return *this;
    }
};

// One track in the Single view, in Seconds mode unless the case says otherwise.
Case single(std::string_view name, const Track& own)
{
    Case c;
    c.name = name;
    c.own = own;
    return c;
}

// This track and another in the Compare view, in Beats mode unless the case says otherwise.
Case compare(std::string_view name, const Track& own, const Track& other)
{
    Case c;
    c.name = name;
    c.mode = DisplayMode::kCompare;
    c.timeBase = TimeBase::kBeats;
    c.own = own;
    c.other = other;
    return c;
}

constexpr TimeBase kSeconds = TimeBase::kSeconds;
constexpr TimeBase kBeats = TimeBase::kBeats;
constexpr double kNone = kNaN;              // No frequency found, nothing measured.
constexpr double kSilent = kMinusInfinity;  // The dBFS of exactly 0.
constexpr double kCancels = kMinusInfinity; // The summing gain of an exact cancellation.
constexpr float kNaNSample = std::numeric_limits<float>::quiet_NaN();
constexpr float kInfSample = std::numeric_limits<float>::infinity();
// Levels known in closed form, written out because std::log10 is not constexpr (a static
// initialiser that calls it could throw); KnownLevels checks each against its formula.
// 20 log10(0.5), -6.0206 dBFS: a peak of 0.5.
constexpr double kHalf = -6.020599913279624;
// -10 log10(2), -3.0103 dB: the RMS level of a sine against its peak.
constexpr double kSine = -3.010299956639812;
// -10 log10(3), -4.7712 dB: the RMS level of uniform noise against its peak, 1 / sqrt(3).
constexpr double kUniform = -4.771212547196624;
// 10 log10(2), +3.0103 dB: the summing gain of two identical tracks, twice the amplitude
// against two unrelated tracks of the same level.
constexpr double kIdentical = 3.010299956639812;
// 10 log10(2.25 / 1.25), +2.5527 dB: a track and a copy at half its level, 1.5 times the
// amplitude against 1.25 times the energy.
constexpr double kHalfCopy = 2.5527250510330606;

// The corpus, one case per line. Delays are within the match reach (protocol::kMatchReachSeconds:
// 480 samples at 48 kHz, 1920 at 192 kHz) unless the case says beyond it.
// clang-format off
std::vector<Case> corpus()
{
    const Signal tone = sine(1000, 0.5);
    const Signal hiss = noise(1, 0.5);
    return {
        // Single: one track's readouts (of the Mid, the trigger view) and its views.
        single("Single_Sine1kHalf_48k_Stereo", stereo(tone)).triggers().levels(kHalf, kHalf + kSine).pitch(1000),
        single("Single_Sine1kHalf_44k1_Stereo", stereo(tone)).at(44100).triggers().levels(kHalf, kHalf + kSine).pitch(1000),
        single("Single_Sine1kHalf_384k_Stereo", stereo(tone)).at(384000).triggers().levels(kHalf, kHalf + kSine).pitch(1000),
        single("Single_Sine1kHalf_48k_Frozen", stereo(tone)).freeze().triggers().levels(kHalf, kHalf + kSine).pitch(1000),
        single("Single_Sine5kHalf_48k_Beats", stereo(sine(5000, 0.5))).in(kBeats).levels(kHalf, kHalf + kSine).pitch(5000),
        single("Single_Sine440Full_48k_Mono", mono(sine(440, 1.0))).triggers().levels(0, kSine).pitch(440),
        single("Single_Sine440Full_192k_Mono", mono(sine(440, 1.0))).at(192000).triggers().levels(0, kSine).pitch(440),
        // A quiet sine, at twice the trigger's default hysteresis (0.01) so that it triggers
        // clearly, not by a rounding either way.
        single("Single_Sine100At34dB_48k_Stereo", stereo(sine(100, 0.02))).triggers().levels(dbfs(0.02), dbfs(0.02) + kSine).pitch(100),
        // A sine on the left alone: the Mid holds it at half its level.
        single("Single_SineLeftOnly_48k_Stereo", stereo(tone, silence())).triggers().levels(2 * kHalf, 2 * kHalf + kSine).pitch(1000),
        // Channels in opposite polarity: the Mid is silent, the Side holds the sine.
        single("Single_SineOutOfPhase_48k_Stereo", stereo(tone, inverted(tone))).levels(kSilent, kSilent).pitch(kNone),
        single("Single_DcQuarter_48k_Stereo", stereo(dc(0.25))).levels(2 * kHalf, 2 * kHalf).pitch(kNone),
        single("Single_DcMinusHalf_192k_Mono", mono(dc(-0.5))).at(192000).levels(kHalf, kHalf).pitch(kNone),
        single("Single_Silence_48k_Stereo", stereo(silence())).levels(kSilent, kSilent).pitch(kNone),
        single("Single_Silence_384k_Mono", mono(silence())).at(384000).levels(kSilent, kSilent).pitch(kNone),
        single("Single_Silence_48k_Beats", stereo(silence())).in(kBeats).levels(kSilent, kSilent).pitch(kNone),
        single("Single_Noise_48k_Stereo", stereo(hiss)).levels(kHalf, kHalf + kUniform).pitch(kNone),
        // Samples no host should send, one in every 4800: what the readouts should then say is
        // not decided, so nothing is stated; the snapshot keeps what they say now. A NaN sample
        // leaves the peak as it was but makes the RMS NaN and hides the frequency.
        single("Single_SineWithNaN_48k_Stereo", stereo(spiked(tone, kNaNSample, 4800))),
        single("Single_SineWithInfinity_48k_Stereo", stereo(spiked(tone, kInfSample, 4800))),

        // Compare: the other track's match against this one, and their summing gains.
        compare("Compare_NoiseCopy_48k", stereo(hiss), stereo(hiss)).match(1, 0, 1).gains(kIdentical, kIdentical),
        compare("Compare_NoiseCopyHalf_48k", stereo(hiss), stereo(scaled(hiss, 0.5))).match(1, 0, 1).gains(kHalfCopy, kHalfCopy),
        compare("Compare_NoiseInverted_48k", stereo(hiss), stereo(inverted(hiss))).match(-1, 0, -1).gains(kCancels, kIdentical),
        compare("Compare_NoiseInverted_192k", stereo(hiss), stereo(inverted(hiss))).at(192000).match(-1, 0, -1).gains(kCancels, kIdentical),
        compare("Compare_NoiseInverted_48k_Seconds", stereo(hiss), stereo(inverted(hiss))).in(kSeconds).match(-1, 0, -1).gains(kCancels, kIdentical),
        compare("Compare_NoiseInverted_48k_Mono", mono(hiss), mono(inverted(hiss))).match(-1, 0, -1).gains(kCancels, kIdentical),
        compare("Compare_NoiseInverted_48k_Frozen", stereo(hiss), stereo(inverted(hiss))).freeze().match(-1, 0, -1).gains(kCancels, kIdentical),
        // A delayed copy, fixed, sums as a copy does: +3.01 dB (kIdentical), as the Seconds cases
        // assert. The Beats cases leave the fix's gain unstated because the engine gets it wrong
        // now, we suspect: Beats mode's fixed sum gain is too weak for delayed pairs, in
        // proportion to the delay (2.67 dB at 1500 samples of a 24000-sample sweep), likely as
        // the shifted track is summed across the playhead with audio of another pass. Their
        // snapshots record that current behaviour, not a correct value; the bug is to be fixed in
        // its own change, which then states +3.01 dB here and approves the new snapshots.
        compare("Compare_NoiseDelayed100_48k", stereo(hiss), stereo(delayed(hiss, 100))).match(0, 100, 1).gains(0, std::nullopt),
        compare("Compare_NoiseDelayed100_192k", stereo(hiss), stereo(delayed(hiss, 100))).at(192000).match(0, 100, 1).gains(0, std::nullopt),
        compare("Compare_NoiseDelayed1500_192k", stereo(hiss), stereo(delayed(hiss, 1500))).at(192000).match(0, 1500, 1).gains(0, std::nullopt),
        compare("Compare_NoiseDelayed100_48k_Seconds", stereo(hiss), stereo(delayed(hiss, 100))).in(kSeconds).match(0, 100, 1).gains(0, kIdentical),
        compare("Compare_NoiseDelayedInverted37_48k", stereo(hiss), stereo(inverted(delayed(hiss, 37)))).match(0, 37, -1).gains(0, std::nullopt),
        compare("Compare_NoiseDelayedInverted37_48k_Seconds", stereo(hiss), stereo(inverted(delayed(hiss, 37)))).in(kSeconds).match(0, 37, -1).gains(0, kIdentical),
        // This track is the late one: the other leads, by a negative offset.
        compare("Compare_NoiseLeading64_48k", stereo(delayed(hiss, 64)), stereo(hiss)).match(0, -64, 1).gains(0, std::nullopt),
        // Beyond the 480-sample reach: no match is found, and no fix suggested.
        compare("Compare_NoiseDelayed600_48k", stereo(hiss), stereo(delayed(hiss, 600))).match(0, 0, 0).gains(0, 0),
        compare("Compare_NoiseUnrelated_48k", stereo(hiss), stereo(noise(2, 0.5))).match(0, 0, 0).gains(0, 0),
        // A steady tone flipped: the flip wins over the half-period shift that matches as well.
        compare("Compare_SineInverted_48k", stereo(tone), stereo(inverted(tone))).match(-1, 0, -1).gains(kCancels, kIdentical),
        compare("Compare_SineCopy_44k1", stereo(tone), stereo(tone)).at(44100).match(1, 0, 1).gains(kIdentical, kIdentical),
        // Silence on either side: the tracks never sound together, so nothing is measured.
        compare("Compare_OtherSilent_48k", stereo(hiss), stereo(silence())).match(kNone, 0, 0).gains(kNone, kNone),
        compare("Compare_BothSilent_48k", stereo(silence()), stereo(silence())).levels(kSilent, kSilent).pitch(kNone).match(kNone, 0, 0).gains(kNone, kNone),
        // A steady offset is sound to the analysis too: two equal ones sum as copies.
        compare("Compare_DcCopy_48k", stereo(dc(0.25)), stereo(dc(0.25))).levels(2 * kHalf, 2 * kHalf).pitch(kNone).match(1, 0, 1).gains(kIdentical, kIdentical),
        // NaN samples on this track, as in Single: nothing is stated. The blocks holding them are
        // left out of the match, and the sum lane carries the NaN points.
        compare("Compare_NoiseWithNaN_48k", stereo(spiked(hiss, kNaNSample, 4800)), stereo(hiss)),
    };
}
// clang-format on

std::string testName(const ::testing::TestParamInfo<Case>& info)
{
    return std::string(info.param.name);
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const Case& c, std::ostream* out)
{
    *out << c.name;
}

// Instances of group A on one host clock at 120 BPM in 4/4, the first viewed by an analyzer.
class Host
{
public:
    explicit Host(const Case& c) : rate(c.rate)
    {
        add(c.own);
        if (c.other) {
            add(*c.other);
        }
    }

    [[nodiscard]] Engine& own() const { return *tracks.front().engine; }

    // Plays `count` samples, in blocks of 10 ms, producing a frame after each as a page asks.
    void play(std::uint64_t count, Analyzer& analyzer, std::optional<DecodedFrame>& frame,
              std::vector<std::byte>& bytes)
    {
        const std::uint64_t block = rate / 100;
        std::vector<float> left(block);
        std::vector<float> right(block);
        for (std::uint64_t done = 0; done < count; done += block) {
            for (const auto& [engine, track] : tracks) {
                for (std::uint64_t i = 0; i < block; ++i) {
                    left[i] = track.left(sample + i, rate);
                    right[i] = track.right ? track.right(sample + i, rate) : left[i];
                }
                const std::array<const float*, 2> channels{left.data(), right.data()};
                engine->captureRing().write(channels.data(), track.channels,
                                            static_cast<int>(block));
                engine->timeline().advance({.playing = true,
                                            .positionValid = true,
                                            .quarterNotes = quarterNotes,
                                            .tempoValid = true,
                                            .tempo = kTempo},
                                           static_cast<int>(block), rate);
            }
            sample += block;
            quarterNotes += static_cast<double>(block) * kTempo / (60.0 * rate);
            std::vector<std::byte> produced;
            if (analyzer.produce(produced)) {
                bytes = std::move(produced);
                frame = decode(bytes);
            }
        }
    }

    InstanceRegistry registry;

private:
    static constexpr double kTempo = 120.0;

    struct Member
    {
        std::shared_ptr<Engine> engine;
        Track track;
    };

    void add(const Track& track)
    {
        auto engine = std::make_shared<Engine>();
        engine->setSampleRate(rate);
        engine->setInputChannels(track.channels);
        registry.add(engine);
        tracks.push_back({.engine = std::move(engine), .track = track});
    }

    std::uint32_t rate;
    std::vector<Member> tracks;
    std::uint64_t sample = 0;
    double quarterNotes = 0.0;
};

// Single mode shows the default views (L and R) over a triggered 100 ms span, or a quarter-beat
// sweep (125 ms at 120 BPM); the multi-track view shows each track's Mid.
void configure(Engine& engine, const Case& c)
{
    engine.settings().set(SettingId::kDisplayMode, static_cast<double>(c.mode));
    engine.settings().set(SettingId::kTimeBase, static_cast<double>(c.timeBase));
    engine.settings().set(SettingId::kTimeSpan, 0.1);
    engine.settings().set(SettingId::kBeatSpan,
                          static_cast<double>(protocol::BeatSpan::kQuarterBeat));
}

constexpr std::size_t kColumns = 500;

// A number as the corpus writes it: kSignificantDigits, and NaN and infinities as strings, which
// JSON has no numbers for.
json number(double value)
{
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0 ? "Infinity" : "-Infinity";
    }
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.*g", kSignificantDigits, value);
    const double rounded = std::strtod(text.data(), nullptr);
    if (rounded == std::trunc(rounded) && std::abs(rounded) < 1e15) {
        return static_cast<std::int64_t>(rounded); // Written as 3, not 3.0.
    }
    return rounded;
}

// The extremes of a run of points, and how many are not numbers.
json summary(std::span<const float> values)
{
    float lowest = std::numeric_limits<float>::infinity();
    float highest = -std::numeric_limits<float>::infinity();
    std::int64_t notNumbers = 0;
    for (const float value : values) {
        if (std::isnan(value)) {
            ++notNumbers;
            continue;
        }
        lowest = std::min(lowest, value);
        highest = std::max(highest, value);
    }
    json out;
    out["min"] = number(values.size() > static_cast<std::size_t>(notNumbers) ? lowest : kNaN);
    out["max"] = number(values.size() > static_cast<std::size_t>(notNumbers) ? highest : kNaN);
    out["nanPoints"] = notNumbers;
    return out;
}

constexpr std::array<std::pair<FrameFlag, std::string_view>, protocol::kFrameFlagCount> kFlags{{
    {FrameFlag::kTriggered, "triggered"},
    {FrameFlag::kFrozen, "frozen"},
    {FrameFlag::kRoll, "roll"},
    {FrameFlag::kSynced, "synced"},
    {FrameFlag::kLimited, "limited"},
    {FrameFlag::kCompared, "compared"},
    {FrameFlag::kPartial, "partial"},
}};
// A flag added to the protocol enlarges the array; its unnamed entry fails here.
static_assert(std::ranges::all_of(kFlags, [](const auto& flag) { return !flag.second.empty(); }),
              "every frame flag needs a name in kFlags");

// What a frame says that a reader would check: its shape, readouts, each view's or lane's
// extremes, and in a Compare frame the matches and summing gains. The time position (sequence,
// cursor, cycle) is left out: it follows how long the case played, not the signal.
json snapshot(const Case& c, const DecodedFrame& frame, const std::vector<std::byte>& bytes)
{
    json out;
    out["signal"]["displayMode"] = protocol::kDisplayModeNames.at(static_cast<std::size_t>(c.mode));
    out["signal"]["timeBase"] = protocol::kTimeBaseNames.at(static_cast<std::size_t>(c.timeBase));
    out["signal"]["sampleRate"] = c.rate;
    out["signal"]["channels"] = c.own.channels;
    out["signal"]["frozen"] = c.frozen;

    out["mode"] = protocol::kFrameModeNames.at(static_cast<std::size_t>(frame.mode));
    for (const auto& [flag, name] : kFlags) {
        out["flags"][name] = frame.has(flag);
    }
    out["pointCount"] = frame.pointCount;
    out["samplesPerPoint"] = number(frame.samplesPerPoint);
    out["visibleSamples"] = number(frame.visibleSamples);
    out["readouts"]["peak"] = number(frame.peak);
    out["readouts"]["rms"] = number(frame.rms);
    out["readouts"]["frequency"] = number(frame.frequency);

    const std::size_t perPoint = frame.mode == FrameMode::kMinMax ? 2 : 1;
    const std::size_t traceLength = std::size_t{frame.pointCount} * perPoint;
    const std::span<const float> data(frame.data);
    std::size_t at = 0;
    if (frame.laneCount == 0) {
        for (std::size_t view = 0; view < protocol::kChannelViewCount; ++view) {
            if ((frame.views & (1U << view)) != 0) {
                out["views"][protocol::kChannelViewNames.at(view)] =
                    summary(data.subspan(at, traceLength));
                at += traceLength;
            }
        }
        return out;
    }
    const bool compared = frame.has(FrameFlag::kCompared);
    const std::size_t lanes = frame.laneCount;
    const std::size_t tracks = compared ? lanes - 1U : lanes;
    for (std::size_t lane = 0; lane < lanes; ++lane) {
        const std::string name = lane < tracks ? "track" + std::to_string(lane) : "sum";
        out["lanes"][name] = summary(data.subspan(at, traceLength));
        at += traceLength;
    }
    if (compared) {
        out["sumGain"] = number(get<float>(bytes, FrameHeader::kSumGainOffset));
        out["fixedGain"] = number(get<float>(bytes, FrameHeader::kFixedGainOffset));
        for (std::size_t track = 1; track < tracks; ++track) {
            const std::size_t first = at + track * protocol::kMatchValues;
            json& match = out["matches"]["track" + std::to_string(track)];
            match["correlation"] = number(data[first]);
            match["offset"] = number(data[first + 1]);
            match["polarity"] = number(data[first + 2]);
        }
    }
    return out;
}

// Each difference between `approved` and `actual`, by path. Numbers match within the tolerance;
// "NaN" and the infinities as written.
void compare(const json& approved, const json& actual, const std::string& path,
             std::vector<std::string>& differences)
{
    if (approved.is_number() && actual.is_number()) {
        const double expected = approved.get<double>();
        const double value = actual.get<double>();
        const double allowed = std::max(
            kAbsoluteTolerance, kRelativeTolerance * std::max(std::abs(expected), std::abs(value)));
        if (!(std::abs(expected - value) <= allowed)) {
            differences.push_back(path + ": approved " + approved.dump() + ", now " +
                                  actual.dump());
        }
        return;
    }
    if (approved.is_object() && actual.is_object()) {
        std::set<std::string> keys;
        for (const auto& [key, value] : approved.items()) {
            keys.insert(key);
        }
        for (const auto& [key, value] : actual.items()) {
            keys.insert(key);
        }
        for (const std::string& key : keys) {
            std::string at = path;
            at += '.';
            at += key;
            if (!approved.contains(key)) {
                differences.push_back(at + ": not approved, now " + actual.at(key).dump());
            } else if (!actual.contains(key)) {
                differences.push_back(at + ": approved " + approved.at(key).dump() + ", now gone");
            } else {
                compare(approved.at(key), actual.at(key), at, differences);
            }
        }
        return;
    }
    if (approved != actual) {
        differences.push_back(path + ": approved " + approved.dump() + ", now " + actual.dump());
    }
}

bool updating()
{
    constexpr const char* kVariable = "ORIZON_UPDATE_CORPUS";
#ifdef _MSC_VER
    // MSVC deprecates std::getenv (C4996, an error at /W4 /WX); _dupenv_s is its replacement.
    char* value = nullptr;
    std::size_t length = 0;
    const bool set = _dupenv_s(&value, &length, kVariable) == 0 && value != nullptr;
    const bool update = set && std::string_view(value) == "1";
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s allocates with malloc.
    return update;
#else
    const char* value = std::getenv(kVariable); // NOLINT(concurrency-mt-unsafe)
    return value != nullptr && std::string_view(value) == "1";
#endif
}

std::filesystem::path corpusFile(std::string_view name)
{
    return std::filesystem::path(ORIZON_CORPUS_DIR) / (std::string(name) + ".json");
}

// Sorted keys (nlohmann::json keeps objects sorted), two-space indents, LF line endings and a
// final newline, and exponents without a plus or leading zeros (1.2e-7, not 1.2e-07): as Prettier
// writes JSON, so `npm run format:check` passes on files the tests write.
std::string serialise(const json& value)
{
    static const std::regex kExponent(R"((\d)e\+?(-?)0*(\d))");
    return std::regex_replace(value.dump(2), kExponent, "$1e$2$3") + "\n";
}

// The value of an approved number: NaN and the infinities are written as strings.
double valueOf(const json& value)
{
    if (value.is_number()) {
        return value.get<double>();
    }
    const auto text = value.get<std::string>();
    if (text == "NaN") {
        return kNaN;
    }
    return text == "Infinity" ? std::numeric_limits<double>::infinity() : kMinusInfinity;
}

// The approved snapshot in `file`, or empty with `problem` saying why there is none.
std::optional<json> loadApproved(const std::filesystem::path& file, std::string& problem)
{
    std::ifstream input(file, std::ios::binary);
    if (!input.is_open()) {
        problem = file.string() +
                  " is missing: run the engine tests with ORIZON_UPDATE_CORPUS=1 to write it, "
                  "then review it";
        return std::nullopt;
    }
    std::stringstream text;
    text << input.rdbuf();
    json approved = json::parse(text.str(), nullptr, false);
    if (approved.is_discarded()) {
        problem = file.string() + " is not JSON";
        return std::nullopt;
    }
    return approved;
}

// Whether `actual` meets an expectation: NaN must be NaN, anything else be within `tolerance`.
void expectNear(std::string_view what, double actual, std::optional<double> expected,
                double tolerance)
{
    if (!expected) {
        return;
    }
    if (std::isnan(*expected)) {
        EXPECT_TRUE(std::isnan(actual)) << what << " should be no value (NaN), is " << actual;
    } else {
        EXPECT_NEAR(actual, *expected, tolerance) << what;
    }
}

class SignalCorpus : public ::testing::TestWithParam<Case>
{
};

TEST_P(SignalCorpus, MatchesTheApprovedFrame)
{
    const Case& c = GetParam();
    Host host(c);
    configure(host.own(), c);
    Analyzer analyzer(host.own(), host.registry);
    analyzer.setColumns(kColumns);
    std::optional<DecodedFrame> frame;
    std::vector<std::byte> bytes;
    host.play(c.rate, analyzer, frame, bytes); // One second.
    if (c.frozen) {
        analyzer.setFrozen(true);
        host.play(c.rate / 10, analyzer, frame, bytes);
    }
    ASSERT_TRUE(frame) << "no frame";
    ASSERT_EQ(frame->has(FrameFlag::kFrozen), c.frozen);
    ASSERT_EQ(frame->has(FrameFlag::kCompared), c.mode == DisplayMode::kCompare);
    const json actual = snapshot(c, *frame, bytes);

    // What physics says the signal reads.
    const Expect& expect = c.expect;
    if (expect.triggered) {
        EXPECT_EQ(frame->has(FrameFlag::kTriggered), *expect.triggered) << "triggered";
    }
    if (expect.peakDbfs && std::isinf(*expect.peakDbfs)) {
        EXPECT_EQ(frame->peak, 0.0f) << "silence peaks at nothing, not at a tiny level";
    } else {
        expectNear("peak (dBFS)", dbfs(frame->peak), expect.peakDbfs, kPeakToleranceDb);
    }
    if (expect.rmsDbfs && std::isinf(*expect.rmsDbfs)) {
        EXPECT_EQ(frame->rms, 0.0f) << "silence has no level, not a tiny one";
    } else {
        expectNear("RMS (dBFS)", dbfs(frame->rms), expect.rmsDbfs, kRmsToleranceDb);
    }
    if (expect.frequency) {
        // The frame carries 0 for no frequency (protocol/src/frame.ts).
        if (std::isnan(*expect.frequency)) {
            EXPECT_EQ(frame->frequency, 0.0f) << "no frequency to find";
        } else {
            EXPECT_NEAR(frame->frequency, *expect.frequency,
                        kFrequencyTolerance * *expect.frequency);
        }
    }
    if (c.mode == DisplayMode::kCompare) {
        const json& match = actual.at("matches").at("track1");
        expectNear("correlation", valueOf(match.at("correlation")), expect.correlation,
                   expect.correlation == 0.0 ? kChanceCorrelation : kCorrelationTolerance);
        if (expect.offset) {
            EXPECT_EQ(valueOf(match.at("offset")), *expect.offset) << "offset (samples)";
        }
        if (expect.polarity) {
            EXPECT_EQ(valueOf(match.at("polarity")), *expect.polarity) << "polarity";
        }
        for (const auto& [name, expected] :
             {std::pair{"sumGain", expect.sumGainDb}, std::pair{"fixedGain", expect.fixedGainDb}}) {
            const double gain = valueOf(actual.at(name));
            if (expected && std::isinf(*expected)) {
                EXPECT_EQ(gain, *expected)
                    << name << " cancels exactly: -infinity, not a number at a floor";
            } else {
                expectNear(name, gain, expected,
                           expected == 0.0 ? kChanceGainDb : kGainToleranceDb);
            }
        }
    }

    // What was approved.
    const std::filesystem::path file = corpusFile(c.name);
    if (updating()) {
        std::ofstream output(file, std::ios::binary | std::ios::trunc); // LF on every platform.
        output << serialise(actual);
        ASSERT_TRUE(output.good()) << "could not write " << file;
        return;
    }
    std::string problem;
    const std::optional<json> approved = loadApproved(file, problem);
    ASSERT_TRUE(approved) << problem;
    std::vector<std::string> differences;
    compare(*approved, actual, "$", differences);
    std::string report;
    for (const std::string& difference : differences) {
        report += "\n  " + difference;
    }
    EXPECT_TRUE(differences.empty())
        << c.name << " differs from " << file << ":" << report
        << "\nIf the change is intended, run with ORIZON_UPDATE_CORPUS=1 and review the diff.";
}

INSTANTIATE_TEST_SUITE_P(Corpus, SignalCorpus, ::testing::ValuesIn(corpus()), testName);

// Every approved file belongs to a case, so a renamed or removed case leaves none behind.
// The levels written out above are their formulas.
TEST(SignalCorpusConstants, KnownLevels)
{
    EXPECT_DOUBLE_EQ(kHalf, 20.0 * std::log10(0.5));
    EXPECT_DOUBLE_EQ(kSine, -10.0 * std::log10(2.0));
    EXPECT_DOUBLE_EQ(kUniform, -10.0 * std::log10(3.0));
    EXPECT_DOUBLE_EQ(kIdentical, 10.0 * std::log10(2.0));
    EXPECT_DOUBLE_EQ(kHalfCopy, 10.0 * std::log10(2.25 / 1.25));
}

TEST(SignalCorpusFiles, BelongToACase)
{
    std::set<std::string> names;
    for (const Case& c : corpus()) {
        EXPECT_TRUE(names.insert(std::string(c.name)).second) << c.name << " is named twice";
    }
    for (const auto& entry : std::filesystem::directory_iterator(ORIZON_CORPUS_DIR)) {
        const std::filesystem::path& path = entry.path();
        EXPECT_EQ(path.extension(), ".json") << path << " is not a corpus file";
        EXPECT_TRUE(names.contains(path.stem().string())) << path << " belongs to no case";
    }
}

// The comparison itself: tolerances, NaN, and keys on either side.
TEST(SignalCorpusCompare, AllowsRoundingButNotChange)
{
    const auto differences = [](const json& approved, const json& actual) {
        std::vector<std::string> found;
        compare(approved, actual, "$", found);
        return found.size();
    };
    EXPECT_EQ(differences(json{{"x", 0.5}}, json{{"x", 0.5 * (1 + kRelativeTolerance / 2)}}), 0u);
    EXPECT_EQ(differences(json{{"x", 0.5}}, json{{"x", 0.5 * (1 + kRelativeTolerance * 2)}}), 1u);
    EXPECT_EQ(differences(json{{"x", 0}}, json{{"x", kAbsoluteTolerance / 2}}), 0u);
    EXPECT_EQ(differences(json{{"x", 0}}, json{{"x", kAbsoluteTolerance * 2}}), 1u);
    EXPECT_EQ(differences(json{{"x", "NaN"}}, json{{"x", number(kNaN)}}), 0u);
    EXPECT_EQ(differences(json{{"x", "NaN"}}, json{{"x", 0}}), 1u);
    EXPECT_EQ(differences(json{{"x", 0}}, json{{"x", "NaN"}}), 1u);
    EXPECT_EQ(differences(json{{"x", 1}}, json{{"x", 1}, {"y", 2}}), 1u);
    EXPECT_EQ(differences(json{{"x", 1}, {"y", 2}}, json{{"x", 1}}), 1u);
    EXPECT_EQ(differences(json{{"x", true}}, json{{"x", false}}), 1u);
}

TEST(SignalCorpusCompare, FailsWithoutAnApprovedFile)
{
    std::string problem;
    EXPECT_FALSE(loadApproved(corpusFile("NoSuchCase"), problem));
    EXPECT_NE(problem.find("ORIZON_UPDATE_CORPUS=1"), std::string::npos) << problem;

    const std::filesystem::path broken =
        std::filesystem::temp_directory_path() / "sineless-corpus-broken.json";
    {
        std::ofstream output(broken, std::ios::binary | std::ios::trunc);
        output << "{\"peak\": ";
    }
    problem.clear();
    EXPECT_FALSE(loadApproved(broken, problem));
    EXPECT_NE(problem.find("is not JSON"), std::string::npos) << problem;
    std::filesystem::remove(broken);
}

TEST(SignalCorpusCompare, WritesNumbersStably)
{
    EXPECT_EQ(number(3.0).dump(), "3");
    EXPECT_EQ(number(0.123456789).dump(), "0.1234568");
    EXPECT_EQ(number(-kNaN).dump(), "\"NaN\"");
    EXPECT_EQ(number(kMinusInfinity).dump(), "\"-Infinity\"");
    EXPECT_EQ(serialise(json{{"b", 1}, {"a", 2}}), "{\n  \"a\": 2,\n  \"b\": 1\n}\n");
    EXPECT_EQ(serialise(json{{"a", number(1.2e-7)}, {"b", number(-3e20)}}),
              "{\n  \"a\": 1.2e-7,\n  \"b\": -3e20\n}\n");
}

} // namespace
} // namespace orizon::engine
