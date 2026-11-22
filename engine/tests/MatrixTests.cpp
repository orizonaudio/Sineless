// The matrix: the engine's rules run at every sample rate a host may use, with spans on both sides
// of each limit the engine branches on, in every display mode and time base, mono and stereo, live
// and frozen. Spans, modes, beat spans and limits come from the protocol, so a new one is covered
// as it is added. Past bugs sat on exactly these edges: a frozen hold sized to the longest span
// left lanes blank at the pan reach, a view at the zoom limit was rejected after rounding, and a
// quiet level read as a number where there was none.
//
// Signals ride on a DC offset, so a trace value near zero is a hole in the data, never a zero
// crossing.

#include "Display.h"
#include "FrameDecoding.h"
#include "SampleRates.h"
#include "engine/Analyzer.h"
#include "engine/CaptureRing.h"
#include "engine/FreeRunning.h"
#include "engine/HoldBuffer.h"
#include "engine/InstanceRegistry.h"
#include "engine/Reconstruction.h"
#include "engine/Sweep.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::BeatSpan;
using protocol::FrameFlag;
using protocol::FrameHeader;
using protocol::FrameMode;
using protocol::SettingId;
using protocol::TimeBase;

constexpr std::size_t kColumns = 400;
constexpr int kBlock = 1024;
// A trace value below this is a hole: every signal here stays above it.
constexpr float kHole = 0.05f;

// "44k1", "48k", "352k8".
std::string rateName(double rate)
{
    const auto tenths = static_cast<long long>(std::llround(rate / 100.0));
    std::string name = std::to_string(tenths / 10) + "k";
    if (tenths % 10 != 0) {
        name += std::to_string(tenths % 10);
    }
    return name;
}

std::string capitalised(std::string_view name)
{
    std::string text(name);
    if (!text.empty()) {
        text.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
    }
    return text;
}

std::string modeName(Display mode)
{
    return displayName(mode);
}

std::string timeBaseName(TimeBase timeBase)
{
    return capitalised(protocol::kTimeBaseNames.at(static_cast<std::size_t>(timeBase)));
}

const char* channelsName(int channels)
{
    return channels == 1 ? "Mono" : "Stereo";
}

std::vector<Display> everyMode()
{
    return everyDisplay();
}

const protocol::SettingDescriptor& setting(SettingId id)
{
    return protocol::kSettings.at(static_cast<std::size_t>(id));
}

// The seconds a capture holds at `rate`. Playing longer than this before measuring makes the
// capture wrap, as it has in any host that has played for a while.
double captureSeconds(double rate)
{
    return static_cast<double>(CaptureRing::capacityFor(rate)) / rate;
}

// Whether the capture keeps what a freeze in Trigger or Free holds at `rate`, for readers while the
// audio thread writes on (the most the engine reads at once, as frozenLength in Analyzer.cpp).
bool captureHoldsTheFreeze(double rate)
{
    return kSecondsFreezeSeconds * rate <=
           static_cast<double>(CaptureRing::readableLength(CaptureRing::capacityFor(rate)));
}

// Deterministic noise in -0.5 to 0.5.
float noiseAt(std::int64_t sample, std::uint64_t seed)
{
    std::uint64_t x =
        static_cast<std::uint64_t>(sample) * 0x9E3779B97F4A7C15ULL + seed * 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 30U;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27U;
    return static_cast<float>(x >> 40U) / static_cast<float>(1U << 24U) - 0.5f;
}

// What one track plays on its left channel (the right one plays half of it).
struct Signal
{
    enum class Kind : std::uint8_t
    {
        kTone,
        kNoise,
        kSilence,
    };
    Kind kind = Kind::kTone;
    double frequency = 1000.0;
    double amplitude = 0.25;
    double offset = 0.5; // DC, so that a hole in a trace stands out.
    std::uint64_t seed = 1;
    std::int64_t delay = 0; // Noise: samples by which it lags the host.
    // Noise: the share of the sample rate it is limited below (0: white, as the host plays it),
    // and a fraction of a sample more by which it then lags, which only limited noise can.
    double lowPass = 0.0;
    double fraction = 0.0;
    std::int64_t startsAt = 0;  // Silent before this host sample.
    std::int64_t endsAt = -1;   // Silent from this host sample, if any.
    std::int64_t brokenAt = -1; // One sample, at this host sample, that is not finite (`broken`).
    // Every other run of this many samples is at a quarter of the amplitude (none if 0), the first
    // run loud.
    std::int64_t quieterEvery = 0;
    std::int64_t louderFrom = -1; // Four times the amplitude from this host sample, if any.
    // Every `brokenEvery` samples (none if 0), one that is not finite, as a broken plugin upstream
    // might send.
    std::int64_t brokenEvery = 0;
    float broken = std::numeric_limits<float>::quiet_NaN();
    std::int64_t brokenUntil = std::numeric_limits<std::int64_t>::max(); // Then whole again.
    bool brokenRightOnly = false;                                        // Else both channels.
    bool brokenLeftOnly = false;                                         // Else both channels.
    // A tone's right channel: the same tone this far ahead in phase (radians), at the same
    // amplitude, instead of half the left; none if NaN.
    double rightPhase = std::numeric_limits<double>::quiet_NaN();
};

// The taps that make white noise the noise a Signal asks for, limited below `lowPass` of the
// sample rate and `fraction` of a sample late: a sinc under a Blackman-Harris window, 92 dB down
// from the limit on, shifted by the fraction as a whole, so the late noise is the same wave read
// a fraction of a sample earlier.
constexpr int kBandTaps = 96;
std::array<double, kBandTaps> bandTaps(double lowPass, double fraction)
{
    // The window's main lobe is 4/kBandTaps of the rate either side of the cutoff.
    const double cutoff = lowPass - (4.0 / kBandTaps);
    std::array<double, kBandTaps> taps{};
    for (int k = 0; k < kBandTaps; ++k) {
        const double t = static_cast<double>(k + 1) - (kBandTaps / 2.0) - fraction;
        const double x = (t / kBandTaps) + 0.5;
        if (x <= 0.0 || x >= 1.0) {
            continue;
        }
        const double turn = 2.0 * std::numbers::pi * x;
        const double window = 0.35875 - (0.48829 * std::cos(turn)) +
                              (0.14128 * std::cos(2.0 * turn)) - (0.01168 * std::cos(3.0 * turn));
        const double sinc = std::abs(t) < 1e-12 ? 2.0 * cutoff
                                                : std::sin(2.0 * std::numbers::pi * cutoff * t) /
                                                      (std::numbers::pi * t);
        taps.at(static_cast<std::size_t>(k)) = window * sinc;
    }
    return taps;
}

// Tracks of group A on one host, each with its own signal and tempo, viewed from the first.
class Session
{
public:
    Session(double sampleRate, int channels) : rate(sampleRate), channelCount(channels) {}

    std::shared_ptr<Engine> join(Signal signal, double tempo = 120.0)
    {
        auto engine = std::make_shared<Engine>();
        engine->prepare(rate);
        engine->setInputChannels(channelCount);
        registry.add(engine);
        tracks.push_back({.engine = engine, .signal = signal, .tempo = tempo});
        if (tracks.size() > 1 && shownAsJoined) {
            // This track's Multi shows every other track, as when the user ticks each.
            std::vector<std::uint64_t> shown = own().shown();
            for (const Member& member : registry.members()) {
                if (member.engine == engine) {
                    shown.push_back(member.serial);
                }
            }
            own().setShown(std::move(shown));
        }
        return engine;
    }

    // Whether a track that joins is shown in this track's Multi at once (the default), or only
    // listed, as an instance added to the host is until the user ticks it.
    bool shownAsJoined = true;

    Engine& own() { return *tracks.front().engine; }

    // The host changes the sample rate: every track is prepared again, as setupProcessing does.
    void prepare(double sampleRate)
    {
        rate = sampleRate;
        for (Track& track : tracks) {
            track.engine->prepare(rate);
        }
    }

    Analyzer& analyzer()
    {
        if (!view) {
            view = std::make_unique<Analyzer>(own(), registry);
            view->setColumns(kColumns);
            if (paced) {
                view->setClock([this] { return nanos; });
            }
        }
        return *view;
    }

    // Plays `seconds`, producing a frame every `frameSeconds` of audio as a page asks (none if 0).
    void play(double seconds, double frameSeconds = 0.0)
    {
        const auto total = static_cast<std::uint64_t>(std::llround(seconds * rate));
        const auto frameEvery = static_cast<std::uint64_t>(std::llround(frameSeconds * rate));
        std::uint64_t sinceFrame = 0;
        for (std::uint64_t done = 0; done < total; done += kBlock) {
            for (std::size_t index = 0; index < tracks.size(); ++index) {
                writeBlock(index, position);
            }
            position += kBlock;
            sinceFrame += kBlock;
            if (frameEvery > 0 && sinceFrame >= frameEvery) {
                sinceFrame = 0;
                std::vector<std::byte> frame;
                (void)analyzer().produce(frame);
            }
        }
    }

    // Plays `seconds` on a simulated clock, as a host and the editor do: a block each time one
    // falls due, stamped as the audio thread stamps it, the analysis worker's step every 2 ms and
    // a frame at each 60 Hz draw. The analyzer paces its edge from the stamps (AD-29), so it
    // trails the newest audio by about a block. The other tracks' blocks of each cycle arrive
    // `lateMillis` after this track's, as when a host processes them after it; `stopInGap` ends
    // just after this track's block, before theirs. Blocks are `block` samples (at most kBlock).
    void playPaced(double seconds, int lateMillis = 0, bool stopInGap = false, int block = kBlock)
    {
        const auto blockSamples = static_cast<std::uint64_t>(block);
        const auto steps = static_cast<int>(std::llround(seconds * 1000.0));
        std::optional<std::pair<int, std::uint64_t>> pending; // When, and which block.
        for (int step = 0;; ++step) {
            nanos += 1'000'000;
            if (pending && step >= pending->first) {
                for (std::size_t index = 1; index < tracks.size(); ++index) {
                    writeBlock(index, pending->second, block);
                }
                pending.reset();
            }
            while (static_cast<double>(position + blockSamples) / rate <=
                   static_cast<double>(nanos - kPacedStart) / 1e9) {
                if (pending) { // Their previous block first: a cycle ends before the next.
                    for (std::size_t index = 1; index < tracks.size(); ++index) {
                        writeBlock(index, pending->second, block);
                    }
                    pending.reset();
                }
                writeBlock(0, position, block);
                own().blockStamp().publish(
                    {.index = own().captureRing().endIndex(), .nanos = nanos, .paced = true});
                if (lateMillis > 0) {
                    pending = {step + lateMillis, position};
                } else {
                    for (std::size_t index = 1; index < tracks.size(); ++index) {
                        writeBlock(index, position, block);
                    }
                }
                position += blockSamples;
                if (stopInGap && step >= steps && pending) {
                    analyzer().advance();
                    return;
                }
            }
            if (step % 2 == 0) {
                analyzer().advance();
            }
            if (step % 17 == 0) {
                (void)analyzer().produce(bytes);
            }
            if (step >= steps && !stopInGap) {
                return;
            }
        }
    }

    // What one track plays from here on, as when its routing changes.
    void setSignal(std::size_t track, const Signal& signal) { tracks.at(track).signal = signal; }

    // Writes `samples` more to one track's capture only, as if its audio arrived early.
    void lead(std::size_t track, int samples)
    {
        std::vector<float> values(static_cast<std::size_t>(samples), 0.5f);
        std::array<const float*, 2> channels{values.data(), values.data()};
        tracks.at(track).engine->captureRing().write(channels.data(), channelCount, samples);
    }

    // Plays `seconds` on the simulated clock, as a host and the editor do (paced): a block each
    // time one falls due, the analysis worker's step every 2 ms and a frame at each 60 Hz draw.
    // The analyzer paces its edge from the blocks' stamps (AD-29), so it trails the newest audio
    // by about a block.
    void playPaced(double seconds)
    {
        const auto steps = static_cast<int>(std::llround(seconds * 1000.0));
        const double blockSeconds = static_cast<double>(kBlock) / rate;
        for (int step = 0; step < steps; ++step) {
            nanos += 1'000'000;
            while (static_cast<double>(position + kBlock) / rate <=
                   static_cast<double>(nanos - kPacedStart) / 1e9) {
                play(blockSeconds);
            }
            if (step % 2 == 0) {
                analyzer().advance();
            }
            if (step % 17 == 0) {
                (void)analyzer().produce(bytes);
            }
        }
    }

    // The next frame, if one comes within 64 ticks.
    std::optional<DecodedFrame> produce()
    {
        // As a page asks every tick: a search or gains running over frames draw nothing until
        // they end, which may take dozens of ticks at the highest rates.
        for (int attempt = 0; attempt < 64; ++attempt) {
            if (analyzer().produce(bytes)) {
                return decode(bytes);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] float field(std::size_t offset) const { return get<float>(bytes, offset); }
    [[nodiscard]] double wideField(std::size_t offset) const { return get<double>(bytes, offset); }

    double rate;
    bool playing = true;             // The host transport, as play reports it.
    double secondsOffset = 0.0;      // Project time at the start, as after a tempo change earlier.
    double quarterNotesOffset = 0.0; // The song's position at the start, in quarter notes.
    // Paced on a simulated clock (set before the first analyzer() call; see playPaced).
    bool paced = false;
    static constexpr std::uint64_t kPacedStart = 1'000'000'000;
    std::uint64_t nanos = kPacedStart;
    InstanceRegistry registry;

private:
    struct Track
    {
        std::shared_ptr<Engine> engine;
        Signal signal;
        double tempo = 120.0;
    };

    // Writes a block of `count` samples (at most kBlock) of track `index`'s signal from host
    // sample `at`, with its position.
    void writeBlock(std::size_t index, std::uint64_t at, int count = kBlock)
    {
        Track& track = tracks.at(index);
        fill(track.signal, at, count);
        std::array<const float*, 2> channels{left.data(), right.data()};
        track.engine->captureRing().write(channels.data(), channelCount, count);
        track.engine->timeline().advance(
            {.playing = playing,
             .positionValid = true,
             .quarterNotes =
                 quarterNotesOffset + (static_cast<double>(at) * track.tempo / (60.0 * rate)),
             .tempoValid = true,
             .tempo = track.tempo,
             .secondsValid = true,
             .seconds = secondsOffset + (static_cast<double>(at) / rate)},
            count, rate);
    }

    void fill(const Signal& signal, std::uint64_t from, int count = kBlock)
    {
        const double step = 2.0 * std::numbers::pi * signal.frequency / rate;
        const bool limited = signal.kind == Signal::Kind::kNoise && signal.lowPass > 0.0;
        const std::array<double, kBandTaps> taps =
            limited ? bandTaps(signal.lowPass, signal.fraction) : std::array<double, kBandTaps>{};
        for (int i = 0; i < count; ++i) {
            const auto index = static_cast<std::int64_t>(from) + i;
            double value = 0.0;
            switch (signal.kind) {
            case Signal::Kind::kTone:
                value =
                    signal.offset + signal.amplitude * std::sin(step * static_cast<double>(index));
                break;
            case Signal::Kind::kNoise:
                if (limited) {
                    for (int k = 0; k < kBandTaps; ++k) {
                        value +=
                            taps.at(static_cast<std::size_t>(k)) *
                            noiseAt(index - signal.delay - (k - (kBandTaps / 2) + 1), signal.seed);
                    }
                    value *= signal.amplitude * 2.0;
                    break;
                }
                value = signal.amplitude * 2.0 * noiseAt(index - signal.delay, signal.seed);
                break;
            case Signal::Kind::kSilence:
                break;
            }
            if (index < signal.startsAt || (signal.endsAt >= 0 && index >= signal.endsAt)) {
                value = 0.0;
            }
            if (signal.quieterEvery > 0 && (index / signal.quieterEvery) % 2 == 1) {
                value *= 0.25;
            }
            if (signal.louderFrom >= 0 && index >= signal.louderFrom) {
                value *= 4.0;
            }
            if (index == signal.brokenAt) {
                value = signal.broken;
            }
            left.at(static_cast<std::size_t>(i)) = static_cast<float>(value);
            right.at(static_cast<std::size_t>(i)) =
                std::isnan(signal.rightPhase)
                    ? static_cast<float>(0.5 * value)
                    : static_cast<float>(
                          signal.offset +
                          signal.amplitude *
                              std::sin(step * static_cast<double>(index) + signal.rightPhase));
            if (signal.brokenEvery > 0 && index % signal.brokenEvery == 0 &&
                index < signal.brokenUntil) {
                if (!signal.brokenRightOnly) {
                    left.at(static_cast<std::size_t>(i)) = signal.broken;
                }
                if (!signal.brokenLeftOnly) {
                    right.at(static_cast<std::size_t>(i)) = signal.broken;
                }
            }
        }
    }

    int channelCount;
    std::vector<Track> tracks;
    std::unique_ptr<Analyzer> view;
    std::array<float, kBlock> left{};
    std::array<float, kBlock> right{};
    std::uint64_t position = 0;
    std::vector<std::byte> bytes;
};

void configure(Engine& engine, Display mode, TimeBase timeBase)
{
    show(engine.settings(), mode);
    engine.settings().set(SettingId::kTimeBase, static_cast<double>(timeBase));
    engine.settings().set(SettingId::kChannelViews, 0b0011); // L and R asked for.
}

// Switches the Meter, which shows the readouts: off, the engine measures the levels alone.
void setMeter(Engine& engine, bool on)
{
    engine.settings().set(SettingId::kMeter,
                          static_cast<double>(on ? protocol::Toggle::kOn : protocol::Toggle::kOff));
}

// The span setting in seconds of `timeBase`: Free's in Free, Trigger's otherwise (Beats uses
// neither).
SettingId spanSetting(TimeBase timeBase)
{
    return timeBase == TimeBase::kFree ? SettingId::kFreeSpan : SettingId::kTriggerSpan;
}

// Sets the span of `timeBase`, Trigger's or Free's (Beats has none in seconds); a span the time
// base does not offer fails the test.
void setSpan(Engine& engine, TimeBase timeBase, double seconds)
{
    if (timeBase != TimeBase::kBeats) {
        EXPECT_TRUE(engine.settings().set(spanSetting(timeBase), seconds))
            << seconds << " s in " << timeBaseName(timeBase);
    }
}

// The traces a frame should hold: views in Single (one for a mono track, and with the Compare
// switch on too), else one lane per track and, with Compare on, the sum.
std::size_t expectedTraces(Display mode, int channels, std::size_t tracks)
{
    switch (mode) {
    case Display::kSingle:
    case Display::kSingleCompareOn: // Shows Single.
        return channels == 1 ? 1 : 2;
    case Display::kMulti:
        return tracks;
    case Display::kCompare:
        return tracks + 1;
    }
    ADD_FAILURE() << "no expectation for display mode " << static_cast<int>(mode);
    return 0;
}

std::size_t tracesIn(const DecodedFrame& frame)
{
    return frame.laneCount > 0 ? frame.laneCount
                               : static_cast<std::size_t>(std::popcount(frame.views));
}

std::size_t traceLength(const DecodedFrame& frame)
{
    return static_cast<std::size_t>(frame.pointCount) * (frame.mode == FrameMode::kMinMax ? 2 : 1);
}

std::span<const float> trace(const DecodedFrame& frame, std::size_t index)
{
    const std::size_t length = traceLength(frame);
    return std::span(frame.data).subspan(index * length, length);
}

// How many values the page decodes after the header, as protocol/src/frame-decoder.ts reads them:
// a trace per view, then per lane, then a match per lane but the sum when the frame is flagged as
// compared. A frame holding fewer is dropped; one holding more shows its first values, which then
// belong to another frame.
std::size_t announcedValues(const DecodedFrame& frame)
{
    const std::size_t traces =
        static_cast<std::size_t>(std::popcount(frame.views)) + std::size_t{frame.laneCount};
    const std::size_t matches = frame.has(FrameFlag::kCompared) && frame.laneCount > 0
                                    ? (std::size_t{frame.laneCount} - 1) * protocol::kMatchValues
                                    : 0;
    return (traces * traceLength(frame)) + matches;
}

// Whether two frames' data are the same value for value, a missing value (NaN) matching another.
bool sameValues(const std::vector<float>& a, const std::vector<float>& b)
{
    return std::ranges::equal(
        a, b, [](float x, float y) { return x == y || (std::isnan(x) && std::isnan(y)); });
}

// Checks that the frame's data holds every trace and exactly what its header announces, and that
// no value of trace `index` is a hole (or not a number); a trace in `silent` must be all zeros
// instead.
void expectTraces(const DecodedFrame& frame, std::size_t expected,
                  std::optional<std::size_t> silent = std::nullopt)
{
    ASSERT_EQ(tracesIn(frame), expected);
    ASSERT_GT(frame.pointCount, 0U);
    ASSERT_EQ(frame.data.size(), announcedValues(frame)) << "the page decodes what is announced";
    for (std::size_t index = 0; index < expected; ++index) {
        const std::span<const float> values = trace(frame, index);
        if (silent == index) {
            EXPECT_TRUE(std::ranges::all_of(values, [](float value) { return value == 0.0f; }))
                << "trace " << index << " should be silent";
            continue;
        }
        const auto hole =
            std::ranges::find_if(values, [](float value) { return !(std::abs(value) >= kHole); });
        EXPECT_TRUE(hole == values.end())
            << "trace " << index << " has a hole at " << (hole - values.begin()) << " of "
            << values.size() << ": " << (hole != values.end() ? *hole : 0.0f);
    }
}

// --- Capacities -----------------------------------------------------------------------------

class CaptureCapacity : public ::testing::TestWithParam<double>
{
};

class SweepCapacity : public ::testing::TestWithParam<double>
{
};

std::string rateTestName(const ::testing::TestParamInfo<double>& info)
{
    return rateName(info.param);
}

// An engine prepared for the rate sizes its capture for it: the smallest that holds what a freeze
// holds, and the freeze holds the longest span and its margin, so a frame of the longest span can
// be read live and frozen.
TEST_P(CaptureCapacity, HoldsAFreeze)
{
    const double rate = GetParam();
    Engine engine;
    engine.prepare(rate);
    const std::size_t capacity = engine.captureRing().capacity();
    EXPECT_EQ(capacity, CaptureRing::capacityFor(rate));
    EXPECT_TRUE(captureHoldsTheFreeze(rate))
        << kSecondsFreezeSeconds * rate << " samples, " << capacity << " held";
    EXPECT_TRUE(capacity == CaptureRing::kMinimumCapacity ||
                static_cast<double>(CaptureRing::readableLength(capacity / 2)) <
                    kSecondsFreezeSeconds * rate)
        << "no larger than it needs";
    const HoldBuffer hold(capacity);
    EXPECT_EQ(hold.holdLength(rate),
              static_cast<std::size_t>(std::llround(kSecondsFreezeSeconds * rate)));
    EXPECT_EQ(protocol::kFrozenHoldSeconds, setting(SettingId::kFreeSpan).maximum)
        << "a freeze holds the longest span";
}

// The tempo-synced sweep of every beat span, at slow, common and fast tempos and in 4/4 and 7/8:
// within the most a sweep keeps, the span asked for when it fits, else the longest that does.
TEST_P(SweepCapacity, KeepsTheLongestBeatSpanThatFits)
{
    const double rate = GetParam();
    for (const double tempo : {30.0, 120.0, 300.0}) {
        for (const auto [numerator, denominator] : {std::pair{4, 4}, std::pair{7, 8}}) {
            const MusicalStamp stamp{.sampleIndex = 0,
                                     .quarterNotes = 0.0,
                                     .tempo = tempo,
                                     .numerator = numerator,
                                     .denominator = denominator,
                                     .playing = true};
            const double barQuarterNotes = 4.0 * numerator / denominator;
            const double perQuarterNote = 60.0 * rate / tempo;
            const auto lengthOf = [&](std::size_t span) {
                const protocol::BeatSpanLength& length = protocol::kBeatSpanLengths.at(span);
                return length.amount * barQuarterNotes;
            };
            for (std::size_t span = 0; span < protocol::kBeatSpanCount; ++span) {
                SCOPED_TRACE(::testing::Message()
                             << protocol::kBeatSpanNames.at(span) << " at " << tempo << " BPM in "
                             << numerator << "/" << denominator);
                const auto sweep = sweepWindow(stamp, std::uint64_t{1} << 26U, rate,
                                               static_cast<BeatSpan>(span), kMaxSweepSamples);
                ASSERT_TRUE(sweep);
                EXPECT_GT(sweep->sampleCount, 0U);
                EXPECT_LE(sweep->sampleCount, kMaxSweepSamples);
                EXPECT_GT(sweep->cursor, 0.0);
                EXPECT_LE(sweep->cursor, 1.0);
                const bool fits =
                    lengthOf(span) * perQuarterNote <= static_cast<double>(kMaxSweepSamples);
                EXPECT_EQ(sweep->limited, !fits);
                if (fits) {
                    EXPECT_DOUBLE_EQ(sweep->spanQuarterNotes, lengthOf(span));
                    EXPECT_EQ(sweep->sampleCount, static_cast<std::uint64_t>(std::llround(
                                                      lengthOf(span) * perQuarterNote)));
                    continue;
                }
                // The longest shorter span that fits.
                std::size_t chosen = span;
                while (chosen > 0 &&
                       lengthOf(chosen) * perQuarterNote > static_cast<double>(kMaxSweepSamples)) {
                    --chosen;
                }
                EXPECT_DOUBLE_EQ(sweep->spanQuarterNotes, lengthOf(chosen));
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(EveryRate, SweepCapacity, ::testing::ValuesIn(kSampleRates), rateTestName);
INSTANTIATE_TEST_SUITE_P(EveryRate, CaptureCapacity, ::testing::ValuesIn(kSampleRates),
                         rateTestName);

// --- Free spans -----------------------------------------------------------------------------

// A span of the Free time base, from the protocol: the shortest, one second and the longest.
struct SecondsSpan
{
    std::string name;
    double seconds = 0.0;
};

std::vector<SecondsSpan> secondsSpans()
{
    return {{.name = "Shortest", .seconds = protocol::kShortestFreeSpanSeconds},
            {.name = "OneSecond", .seconds = 1.0},
            {.name = "Longest", .seconds = setting(SettingId::kFreeSpan).maximum}};
}

struct SpanCase
{
    double rate = 48000.0;
    SecondsSpan span;
    Display mode = Display::kSingle;
    int channels = 2;
};

std::string nameOf(const SpanCase& c)
{
    return rateName(c.rate) + "_" + c.span.name + "_" + modeName(c.mode) + "_" +
           channelsName(c.channels);
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const SpanCase& c, std::ostream* out)
{
    *out << nameOf(c);
}

// Every rate, span and mode. Channels alternate so that each rate and each span runs both mono
// and stereo: the engine branches on the channel count only where it reads views, the same at
// every rate and span.
std::vector<SpanCase> spanCases()
{
    std::vector<SpanCase> cases;
    const std::vector<SecondsSpan> spans = secondsSpans();
    for (std::size_t rate = 0; rate < kSampleRates.size(); ++rate) {
        for (std::size_t span = 0; span < spans.size(); ++span) {
            for (const Display mode : everyMode()) {
                cases.push_back({.rate = kSampleRates.at(rate),
                                 .span = spans[span],
                                 .mode = mode,
                                 .channels = (rate + span) % 2 == 0 ? 2 : 1});
            }
        }
    }
    return cases;
}

// Two tracks of a case in the Free time base, played past a whole capture, so that it has wrapped
// as in a host that has played for a while, and `extraSeconds` more.
struct SecondsSession
{
    SecondsSession(const SpanCase& c, double extraSeconds) : session(c.rate, c.channels)
    {
        session.join(own);
        session.join({.frequency = 1500.0});
        configure(session.own(), c.mode, TimeBase::kFree);
        EXPECT_TRUE(session.own().settings().set(SettingId::kFreeSpan, c.span.seconds));
        session.play(captureSeconds(c.rate) + extraSeconds);
    }

    // Freezes, with another track's capture a little ahead of this one's and audio going on.
    std::optional<DecodedFrame> freeze()
    {
        session.lead(1, kBlock * 4);
        session.analyzer().setFrozen(true);
        session.play(static_cast<double>(kBlock) / session.rate);
        return session.produce();
    }

    Signal own{.frequency = 1000.0};
    Session session;
};

class SecondsSpans : public ::testing::TestWithParam<SpanCase>
{
};

// Live, the frame holds exactly the whole span, runs free (the cases are in the Free time base) at
// every span, and measures it.
// Frozen, it holds the same. Neither says the span is limited: the capture and the freeze keep
// the longest span at every rate, the freeze exactly.
TEST_P(SecondsSpans, DrawsTheWholeSpanLiveAndFrozen)
{
    const SpanCase& c = GetParam();
    SecondsSession seconds(c, c.span.seconds + 0.5);
    Session& session = seconds.session;
    const std::size_t traces = expectedTraces(c.mode, c.channels, 2);
    const double spanSamples = c.span.seconds * c.rate;

    auto frame = session.produce();
    ASSERT_TRUE(frame) << "no live frame";
    EXPECT_FALSE(frame->has(FrameFlag::kFrozen));
    EXPECT_TRUE(frame->has(FrameFlag::kFreeRunning)) << "Free runs free at every span";
    EXPECT_EQ(frame->has(FrameFlag::kCompared), c.mode == Display::kCompare);
    EXPECT_NEAR(frame->visibleSamples, spanSamples, 0.5) << "exactly the span, in whole samples";
    EXPECT_FALSE(frame->has(FrameFlag::kLimited));
    EXPECT_NEAR(frame->spanSeconds, c.span.seconds, 1e-6);
    expectTraces(*frame, traces);
    EXPECT_TRUE(std::isfinite(frame->peak));
    EXPECT_GT(frame->peak, 0.1f) << "readouts measured nothing";
    EXPECT_GT(frame->rms, 0.1f);

    frame = seconds.freeze();
    ASSERT_TRUE(frame) << "no frozen frame";
    ASSERT_TRUE(frame->has(FrameFlag::kFrozen));
    EXPECT_NEAR(frame->visibleSamples, spanSamples, kColumns / 2.0 + 1.0);
    EXPECT_FALSE(frame->has(FrameFlag::kLimited));
    EXPECT_NEAR(frame->spanSeconds, c.span.seconds, 1e-6) << "frozen, the span still";
    expectTraces(*frame, traces);
}

// Frozen, a shorter Trigger span keeps the trigger point where the triggered live window had it,
// at the trigger marker (a tenth into the span), so that changing the scale zooms around it: a
// 1.1 kHz tone, triggered on its rising crossings of 0, still crosses 0 rising there. At the
// lowest and highest rates.
TEST(FrozenTrigger, StaysAtTheMarkerWhenTheSpanShortens)
{
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        SCOPED_TRACE(rateName(rate));
        Session session(rate, 1);
        session.join({.frequency = 1100.0, .amplitude = 0.5, .offset = 0.0});
        configure(session.own(), Display::kSingle, TimeBase::kTrigger);
        session.own().settings().set(SettingId::kTriggerMode,
                                     static_cast<double>(protocol::TriggerMode::kNormal));
        session.own().settings().set(SettingId::kTriggerSpan, 0.02);
        session.analyzer().setColumns(std::size_t{1} << 16U); // Raw samples.
        session.play(1.0, protocol::kReadoutIntervalSeconds / 2.0);
        auto frame = session.produce();
        if (!frame) {
            session.play(0.01);
            frame = session.produce();
        }
        ASSERT_TRUE(frame) << "no live frame";
        ASSERT_TRUE(frame->has(FrameFlag::kTriggered));
        session.analyzer().setFrozen(true);
        session.play(static_cast<double>(kBlock) / rate);
        ASSERT_TRUE(session.produce()) << "no frozen frame";
        session.own().settings().set(SettingId::kTriggerSpan, 0.01);
        frame = session.produce();
        ASSERT_TRUE(frame) << "no frame at the shorter span";
        ASSERT_EQ(frame->mode, FrameMode::kRaw);
        const DecodedFrame samples = samplesOnly(*frame); // Which samples, not the wave between.
        const std::span<const float> values = trace(samples, 0);
        const auto marker = static_cast<std::ptrdiff_t>(std::llround(
            protocol::kDefaultTriggerPosition * frame->visibleSamples + frame->startFraction));
        bool rising = false;
        for (std::ptrdiff_t i = std::max<std::ptrdiff_t>(marker - 1, 1);
             i <= marker + 1 && i < std::ssize(values); ++i) {
            rising = rising || (values[i - 1] <= 0.0f && values[i] > 0.0f) ||
                     (values[i] <= 0.0f && i + 1 < std::ssize(values) && values[i + 1] > 0.0f);
        }
        EXPECT_TRUE(rising) << "no rising crossing at sample " << marker << " of " << values.size();
    }
}

// A mono track triggers at the level set, in every mode: Single triggers on its one channel and
// Multi on its Mid, which for a mono track is that channel, not half of it. A 0.5 tone
// triggered at 0.3 (above half its peak) in Normal mode is triggered, and crosses 0.3 rising at
// the trigger marker. At the lowest and highest rates.
class MonoTrigger : public ::testing::TestWithParam<Display>
{
};

TEST_P(MonoTrigger, TriggersAtTheLevelSet)
{
    constexpr float kLevel = 0.3f;
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        SCOPED_TRACE(rateName(rate));
        Session session(rate, 1);
        session.join({.frequency = 1100.0, .amplitude = 0.5, .offset = 0.0});
        configure(session.own(), GetParam(), TimeBase::kTrigger);
        session.own().settings().set(SettingId::kTriggerMode,
                                     static_cast<double>(protocol::TriggerMode::kNormal));
        session.own().settings().set(SettingId::kTriggerLevel, kLevel);
        session.own().settings().set(SettingId::kTriggerSpan, 0.02);
        session.analyzer().setColumns(std::size_t{1} << 16U); // Raw samples.
        session.play(1.0, protocol::kReadoutIntervalSeconds / 2.0);
        auto frame = session.produce();
        if (!frame) {
            session.play(0.01);
            frame = session.produce();
        }
        ASSERT_TRUE(frame) << "never triggered";
        ASSERT_TRUE(frame->has(FrameFlag::kTriggered));
        ASSERT_EQ(frame->mode, FrameMode::kRaw);
        const DecodedFrame samples = samplesOnly(*frame); // Which samples, not the wave between.
        const std::span<const float> values = trace(samples, 0);
        const auto marker = static_cast<std::ptrdiff_t>(std::llround(
            protocol::kDefaultTriggerPosition * frame->visibleSamples + frame->startFraction));
        bool rising = false;
        for (std::ptrdiff_t i = std::max<std::ptrdiff_t>(marker - 1, 1);
             i <= marker + 1 && i < std::ssize(values); ++i) {
            rising = rising || (values[i - 1] <= kLevel && values[i] > kLevel);
        }
        EXPECT_TRUE(rising) << "no rising crossing of " << kLevel << " at sample " << marker;
    }
}

INSTANTIATE_TEST_SUITE_P(EveryMode, MonoTrigger, ::testing::ValuesIn(everyMode()),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

// Frozen in Trigger, a shorter span shows the part around the trigger marker of the last live
// window, and its readouts describe that part: a tone four times louder in its newest 6 ms, which
// the 10 ms span chosen after freezing at 20 ms no longer shows, reads at its quiet level. In Free,
// the shorter span keeps the right edge instead, and reads the loud newest part (200 ms to 100 ms
// there, as Free starts at 100 ms). In every mode, at the lowest and highest rates.
class FrozenSecondsShorterSpan : public ::testing::TestWithParam<Display>
{
};

TEST_P(FrozenSecondsShorterSpan, ReadsThePartShown)
{
    const Display mode = GetParam();
    constexpr double kAmplitude = 0.1;
    constexpr double kPlayed = 2.0;
    for (const auto [rate, free] :
         {std::pair{kSampleRates.front(), false}, std::pair{kSampleRates.back(), false},
          std::pair{kSampleRates.front(), true}, std::pair{kSampleRates.back(), true}}) {
        SCOPED_TRACE(rateName(rate) + (free ? " Free" : " Trigger"));
        const SettingId spanId = free ? SettingId::kFreeSpan : SettingId::kTriggerSpan;
        const double span = free ? 0.2 : 0.02;
        const auto played = static_cast<std::int64_t>(
            std::llround(kPlayed * rate / static_cast<double>(kBlock)) * kBlock);
        Session session(rate, 1);
        session.join(
            {.frequency = 1100.0,
             .amplitude = kAmplitude,
             .offset = 0.0,
             .louderFrom = played - static_cast<std::int64_t>(std::llround(0.006 * rate))});
        session.join({.frequency = 1500.0});
        configure(session.own(), mode, free ? TimeBase::kFree : TimeBase::kTrigger);
        session.own().settings().set(spanId, span);
        session.play(static_cast<double>(played) / rate, protocol::kReadoutIntervalSeconds / 2.0);
        ASSERT_TRUE(session.produce()) << "no live frame";
        session.analyzer().setFrozen(true);
        ASSERT_TRUE(session.produce()) << "no frozen frame";
        session.own().settings().set(spanId, span / 2.0);
        const auto frame = session.produce();
        ASSERT_TRUE(frame) << "no frame at the shorter span";
        ASSERT_TRUE(frame->has(FrameFlag::kFrozen));
        if (free) {
            EXPECT_NEAR(frame->peak, 4.0 * kAmplitude, 0.02 * kAmplitude) << "the loud part's peak";
            continue;
        }
        EXPECT_NEAR(frame->peak, kAmplitude, 0.02 * kAmplitude) << "the quiet part's peak";
        EXPECT_NEAR(frame->rms, kAmplitude / std::numbers::sqrt2, 0.05 * kAmplitude);
    }
}

INSTANTIATE_TEST_SUITE_P(EveryMode, FrozenSecondsShorterSpan, ::testing::ValuesIn(everyMode()),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

// A host that changes the sample rate while processing (the VST3 rules allow it only while
// inactive) leaves every track's capture sized for the old rate: at 384 kHz after 192 kHz it keeps
// 2.4 s. A 4 s span shows all that is kept, live and frozen, and says it is limited, in every
// display mode.
class RateChangedWhileProcessing : public ::testing::TestWithParam<Display>
{
};

TEST_P(RateChangedWhileProcessing, ShowsWhatTheCaptureKeepsOfALongerSpan)
{
    Session session(192000.0, 2);
    const auto own = session.join({.frequency = 1000.0});
    const auto other = session.join({.frequency = 1500.0});
    session.rate = 384000.0;
    own->setSampleRate(session.rate);
    other->setSampleRate(session.rate);
    configure(session.own(), GetParam(), TimeBase::kFree);
    ASSERT_TRUE(session.own().settings().set(SettingId::kFreeSpan, 4.0));
    session.play(4.0);
    const auto kept =
        static_cast<double>(CaptureRing::readableLength(own->captureRing().capacity()));

    auto frame = session.produce();
    ASSERT_TRUE(frame) << "no live frame";
    EXPECT_TRUE(frame->has(FrameFlag::kLimited));
    EXPECT_NEAR(frame->visibleSamples, kept, kColumns / 2.0 + 1.0) << "all that is kept";
    EXPECT_NEAR(frame->spanSeconds, kept / session.rate, 1e-6) << "the span shown, said exactly";
    {
        SCOPED_TRACE("live");
        expectTraces(*frame, expectedTraces(GetParam(), 2, 2));
    }

    // Frozen with another track's capture a little ahead of this one's, as tracks are: its lane
    // is placed by its own clock, and still drawn whole.
    session.lead(1, kBlock * 4);
    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / session.rate);
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frozen frame";
    EXPECT_TRUE(frame->has(FrameFlag::kLimited));
    // Most of what is held, every lane drawn whole: Multi keeps some of it back for
    // the offset between tracks.
    EXPECT_LE(frame->visibleSamples, kept + kColumns);
    EXPECT_GT(frame->visibleSamples, kept / 2.0);
    EXPECT_NEAR(frame->spanSeconds, frame->visibleSamples / frame->sampleRate, 1e-6)
        << "frozen, what is held, less the margin kept for other tracks";
    EXPECT_LT(frame->visibleSamples, 4.0 * session.rate);
    expectTraces(*frame, expectedTraces(GetParam(), 2, 2));
}

INSTANTIATE_TEST_SUITE_P(EveryMode, RateChangedWhileProcessing, ::testing::ValuesIn(everyMode()),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

// A freeze holds less than a longer span when it comes before a whole span of audio has
// arrived, and holds all of it when the host prepares again at a lower rate afterwards: in every
// display mode, the frozen frame shows what is held, every lane whole, limited only in the first.
class FrozenSpans : public ::testing::TestWithParam<Display>
{
};

TEST_P(FrozenSpans, ShowsWhatAFreezeHoldsOfALongerSpan)
{
    Session session(48000.0, 2);
    session.join({.frequency = 1000.0});
    session.join({.frequency = 1500.0});
    configure(session.own(), GetParam(), TimeBase::kFree);
    ASSERT_TRUE(session.own().settings().set(SettingId::kFreeSpan, 4.0));
    session.play(1.0);
    ASSERT_TRUE(session.produce()) << "no live frame";
    session.analyzer().setFrozen(true);
    const auto frame = session.produce();
    ASSERT_TRUE(frame) << "no frozen frame";
    EXPECT_TRUE(frame->has(FrameFlag::kLimited));
    EXPECT_LT(frame->visibleSamples, 1.0 * session.rate + kColumns);
    EXPECT_GT(frame->visibleSamples, 0.5 * session.rate);
    EXPECT_NEAR(frame->spanSeconds, frame->visibleSamples / frame->sampleRate, 1e-6)
        << "the second held, not the span set";
    expectTraces(*frame, expectedTraces(GetParam(), 2, 2));
}

TEST_P(FrozenSpans, StaysWholeWhenTheHostLowersTheRate)
{
    Session session(384000.0, 2);
    session.join({.frequency = 1000.0});
    session.join({.frequency = 1500.0});
    configure(session.own(), GetParam(), TimeBase::kFree);
    ASSERT_TRUE(session.own().settings().set(SettingId::kFreeSpan, 4.0));
    session.play(4.5);
    ASSERT_TRUE(session.produce()) << "no live frame";
    session.analyzer().setFrozen(true);
    ASSERT_TRUE(session.produce()) << "no frozen frame";
    session.prepare(48000.0);
    session.analyzer().setColumns(kColumns / 2); // Something to draw again for.
    const auto frame = session.produce();
    ASSERT_TRUE(frame) << "no frozen frame after the rate change";
    EXPECT_FALSE(frame->has(FrameFlag::kLimited));
    EXPECT_NEAR(frame->visibleSamples, 4.0 * 384000.0, kColumns);
    EXPECT_NEAR(frame->spanSeconds, 4.0, 1e-6) << "at the rate held, not the new one";
    expectTraces(*frame, expectedTraces(GetParam(), 2, 2));
}

// At a rate that is not a whole number, the longest span's length has a fraction that holds round
// away; with the margin Multi keeps for other tracks' lanes, a whole freeze is still
// whole, and not limited. At rates just under 192 and 384 kHz, where that margin is all the
// capture spares beyond the span.
TEST_P(FrozenSpans, DoesNotLimitAWholeFreezeAtARateThatIsNotAWholeNumber)
{
    for (const double rate : {192000.0 / 1.001, 384000.0 / 1.001}) {
        SCOPED_TRACE(rate);
        Session session(rate, 2);
        session.join({.frequency = 1000.0});
        session.join({.frequency = 1500.0});
        configure(session.own(), GetParam(), TimeBase::kFree);
        ASSERT_TRUE(session.own().settings().set(SettingId::kFreeSpan, 4.0));
        session.play(4.5);
        ASSERT_TRUE(session.produce()) << "no live frame";
        session.analyzer().setFrozen(true);
        const auto frame = session.produce();
        ASSERT_TRUE(frame) << "no frozen frame";
        EXPECT_FALSE(frame->has(FrameFlag::kLimited));
        EXPECT_NEAR(frame->visibleSamples, 4.0 * rate, kColumns);
    }
}

INSTANTIATE_TEST_SUITE_P(EveryMode, FrozenSpans, ::testing::ValuesIn(everyMode()),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

class FrozenPan : public ::testing::TestWithParam<SpanCase>
{
};

// Frozen, the frame reports how far back it can be panned through the freeze, and is whole there
// and at the zoom limit.
TEST_P(FrozenPan, PansBackThroughTheWholeFreeze)
{
    const SpanCase& c = GetParam();
    // More than a freeze holds, so that the view can pan back through all of it.
    SecondsSession seconds(c, protocol::kFrozenHoldSeconds + 0.5);
    Session& session = seconds.session;
    const std::size_t traces = expectedTraces(c.mode, c.channels, 2);
    const double spanSamples = c.span.seconds * c.rate;
    auto frame = seconds.freeze();
    ASSERT_TRUE(frame) << "no frozen frame";
    ASSERT_TRUE(frame->has(FrameFlag::kFrozen));

    // The freeze holds the longest span: the view pans back over what the span leaves of it.
    const float reach = session.field(FrameHeader::kPanReachOffset);
    ASSERT_TRUE(std::isfinite(reach));
    EXPECT_GE(reach, 0.0f);
    const double held = protocol::kFrozenHoldSeconds;
    const double block = static_cast<double>(kBlock) / c.rate;
    EXPECT_LE(reach, held - c.span.seconds + 2.0 * block);
    EXPECT_GE(reach, held - c.span.seconds - 2.0 * block - c.span.seconds / kColumns);
    if (reach > 0.0f) { // At no reach, the frame above is the one at the reach.
        session.analyzer().setPan(reach);
        frame = session.produce();
        ASSERT_TRUE(frame) << "no frame at the pan reach";
        SCOPED_TRACE("panned to the reach");
        expectTraces(*frame, traces);
    }

    // At the zoom limit, a few ulps narrower as the UI may compute it.
    const double width = protocol::kMinimumViewSamples / spanSamples * (1.0 - 1e-12);
    session.analyzer().setView(0.5, 0.5 + width);
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frame at the zoom limit";
    EXPECT_EQ(frame->viewStart, 0.5);
    EXPECT_EQ(frame->viewEnd, 0.5 + width);
    EXPECT_NEAR(frame->visibleSamples, protocol::kMinimumViewSamples, 0.01);
    {
        SCOPED_TRACE("zoomed to the limit");
        expectTraces(*frame, traces);
    }
}

std::string spanTestName(const ::testing::TestParamInfo<SpanCase>& info)
{
    return nameOf(info.param);
}

INSTANTIATE_TEST_SUITE_P(EveryRate, SecondsSpans, ::testing::ValuesIn(spanCases()), spanTestName);
INSTANTIATE_TEST_SUITE_P(EveryRate, FrozenPan, ::testing::ValuesIn(spanCases()), spanTestName);

// A longer Free span reads more of each stereo track's Mid and Side than any frame before it
// did: live and frozen, from the shortest span to one second and to the longest; and live, zoomed
// into a roll, from one second to the longest. The frame is whole,
// and nothing reads past what it allocated (the Debug build's assert and the AddressSanitizer build
// stop it if it does). In every mode, at the lowest and highest rates.
class LongerSpan : public ::testing::TestWithParam<Display>
{
};

TEST_P(LongerSpan, ReadsMoreOfEveryTrackSafely)
{
    const Display mode = GetParam();
    const auto longest = static_cast<double>(setting(SettingId::kFreeSpan).maximum);
    const std::uint32_t midAndSide = (1U << static_cast<unsigned>(protocol::ChannelView::kMid)) |
                                     (1U << static_cast<unsigned>(protocol::ChannelView::kSide));
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        SCOPED_TRACE(rateName(rate));
        const std::size_t traces = expectedTraces(mode, 2, 2);
        // Live and frozen, to one second and to the longest span.
        for (const bool frozen : {false, true}) {
            for (const SecondsSpan& longer : {secondsSpans().at(1), secondsSpans().back()}) {
                SCOPED_TRACE(std::string(frozen ? "frozen" : "live") + ", shortest to " +
                             longer.name);
                SecondsSession seconds({.rate = rate, .span = secondsSpans().front(), .mode = mode},
                                       1.0);
                seconds.session.own().settings().set(SettingId::kChannelViews, midAndSide);
                ASSERT_TRUE(seconds.session.produce()) << "no live frame";
                if (frozen) {
                    ASSERT_TRUE(seconds.freeze()) << "no frozen frame";
                }
                seconds.session.own().settings().set(SettingId::kFreeSpan, longer.seconds);
                const auto frame = seconds.session.produce();
                ASSERT_TRUE(frame) << "no frame at the longer span";
                EXPECT_EQ(frame->has(FrameFlag::kFrozen), frozen);
                expectTraces(*frame, traces);
            }
        }
        {
            SCOPED_TRACE("live, zoomed into a roll, one second to longest");
            const std::vector<SecondsSpan> spans = secondsSpans();
            SecondsSession seconds({.rate = rate, .span = spans.at(1), .mode = mode}, 1.0);
            seconds.session.own().settings().set(SettingId::kChannelViews, midAndSide);
            seconds.session.analyzer().setView(0.5, 1.0);
            ASSERT_TRUE(seconds.session.produce()) << "no live frame";
            seconds.session.own().settings().set(SettingId::kFreeSpan, longest);
            seconds.session.play(static_cast<double>(kBlock) / rate);
            const auto frame = seconds.session.produce();
            ASSERT_TRUE(frame) << "no frame at the longer span";
            EXPECT_TRUE(frame->has(FrameFlag::kFreeRunning));
            expectTraces(*frame, traces);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(EveryMode, LongerSpan, ::testing::ValuesIn(everyMode()),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

// --- A change of sample rate ----------------------------------------------------------------

struct RateChange
{
    double from = 48000.0;
    double to = 384000.0;
    Display mode = Display::kSingle;
};

std::string nameOf(const RateChange& c)
{
    return rateName(c.from) + "_To_" + rateName(c.to) + "_" + modeName(c.mode);
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const RateChange& c, std::ostream* out)
{
    *out << nameOf(c);
}

// Up across the size change, down across it, and within one size, in every mode.
std::vector<RateChange> rateChanges()
{
    std::vector<RateChange> changes;
    for (const auto [from, to] :
         {std::pair{kSampleRates.front(), kSampleRates.back()},
          std::pair{kSampleRates.back(), kSampleRates.front()}, std::pair{48000.0, 192000.0}}) {
        for (const Display mode : everyMode()) {
            changes.push_back({.from = from, .to = to, .mode = mode});
        }
    }
    return changes;
}

class RateChanges : public ::testing::TestWithParam<RateChange>
{
};

// The host changes the rate with the editor open: the captures are sized for the new rate, and a
// freeze there holds what it promises, for the whole pan reach, after the editor's freeze hold was
// sized for the old one.
TEST_P(RateChanges, SizeTheCaptureForTheNewRate)
{
    const RateChange& c = GetParam();
    Session session(c.from, 2);
    session.join({.frequency = 1000.0});
    session.join({.frequency = 1500.0});
    configure(session.own(), c.mode, TimeBase::kTrigger);
    const double span = setting(SettingId::kTriggerSpan).minimum;
    EXPECT_TRUE(session.own().settings().set(SettingId::kTriggerSpan, span));
    EXPECT_EQ(session.own().captureRing().capacity(), CaptureRing::capacityFor(c.from));
    session.play(1.0, protocol::kReadoutIntervalSeconds);
    ASSERT_TRUE(session.produce()) << "no frame before the change";
    session.analyzer().setFrozen(true);
    ASSERT_TRUE(session.produce()) << "no frozen frame before the change";
    session.analyzer().setFrozen(false);

    session.prepare(c.to);
    EXPECT_EQ(session.own().captureRing().capacity(), CaptureRing::capacityFor(c.to));
    session.play(captureSeconds(c.to) + protocol::kFrozenHoldSeconds + 0.5,
                 protocol::kReadoutIntervalSeconds);
    const std::size_t traces = expectedTraces(c.mode, 2, 2);
    auto frame = session.produce();
    ASSERT_TRUE(frame) << "no live frame";
    expectTraces(*frame, traces);

    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / c.to);
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frozen frame";
    ASSERT_TRUE(frame->has(FrameFlag::kFrozen));
    const float reach = session.field(FrameHeader::kPanReachOffset);
    const double block = static_cast<double>(kBlock) / c.to;
    EXPECT_GE(reach, protocol::kFrozenHoldSeconds - span - 2.0 * block - span / kColumns);
    session.analyzer().setPan(reach);
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frame at the pan reach";
    expectTraces(*frame, traces);
}

INSTANTIATE_TEST_SUITE_P(Changes, RateChanges, ::testing::ValuesIn(rateChanges()),
                         [](const ::testing::TestParamInfo<RateChange>& info) {
                             return nameOf(info.param);
                         });

// --- Beat spans -----------------------------------------------------------------------------

struct ModeCase
{
    double rate = 48000.0;
    Display mode = Display::kSingle;
    int channels = 2;
    TimeBase timeBase = TimeBase::kBeats;
};

std::string nameOf(const ModeCase& c)
{
    return rateName(c.rate) + "_" + modeName(c.mode) + "_" + timeBaseName(c.timeBase) + "_" +
           channelsName(c.channels);
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const ModeCase& c, std::ostream* out)
{
    *out << nameOf(c);
}

std::string modeTestName(const ::testing::TestParamInfo<ModeCase>& info)
{
    return nameOf(info.param);
}

// Every rate and mode in Beats, channels alternating by rate.
std::vector<ModeCase> beatCases()
{
    std::vector<ModeCase> cases;
    for (std::size_t rate = 0; rate < kSampleRates.size(); ++rate) {
        for (const Display mode : everyMode()) {
            cases.push_back({.rate = kSampleRates.at(rate),
                             .mode = mode,
                             .channels = rate % 2 == 0 ? 2 : 1,
                             .timeBase = TimeBase::kBeats});
        }
    }
    return cases;
}

// Frozen, a shorter beat span shows part of the held sweep, and its readouts describe that part:
// Peak and RMS of what is shown. A tone alternates every two beats between loud and quiet; frozen
// at the end of a 1-bar cycle that holds a loud half and then a quiet half, the newest two beats
// shown are the quiet half, read as such: its level, not the loud bar's; and with the Meter then
// switched on, by its other rows too. In every mode, at every rate.
class FrozenShorterSpan : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(FrozenShorterSpan, ReadsThePartShown)
{
    const ModeCase c = GetParam();
    constexpr double kAmplitude = 0.5;
    const auto twoBeats = static_cast<std::int64_t>(std::llround(c.rate)); // At 120 BPM.
    Session session(c.rate, c.channels);
    session.join(
        {.frequency = 1000.0, .amplitude = kAmplitude, .offset = 0.0, .quieterEvery = twoBeats});
    session.join({.frequency = 1500.0});
    configure(session.own(), c.mode, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
    // To just before quarter note 12: the bar [8, 12) is loud, then quiet from quarter note 10.
    session.play(6.0 - (2.0 * static_cast<double>(kBlock) / c.rate),
                 protocol::kReadoutIntervalSeconds / 2.0);
    session.analyzer().setFrozen(true);
    ASSERT_TRUE(session.produce()) << "no frozen frame";
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kHalfBar));
    const auto frame = session.produce();
    ASSERT_TRUE(frame) << "no frame at the shorter span";
    ASSERT_TRUE(frame->has(FrameFlag::kFrozen));
    // Single measures L, the first view drawn; Multi this track's Mid, of L and half L.
    const double quiet = 0.25 * kAmplitude * (c.channels == 2 && isMulti(c.mode) ? 0.75 : 1.0);
    EXPECT_NEAR(frame->peak, quiet, 0.02 * quiet) << "the quiet half's peak, not the bar's";
    EXPECT_NEAR(frame->rms, quiet / std::numbers::sqrt2, 0.02 * quiet);

    // The Meter switched on while frozen reads that part too, Left and Right with it: in Multi
    // this track's lane is built again from the held audio with both channels, and shows what it
    // showed.
    setMeter(session.own(), true);
    const auto metered = session.produce();
    ASSERT_TRUE(metered) << "no frame with the Meter switched on";
    if (c.mode != Display::kCompare) {
        // Compare's matches, searched again at the freeze, may still arrive between two frames.
        EXPECT_TRUE(sameValues(metered->data, frame->data)) << "the picture changed";
    }
    EXPECT_NEAR(metered->peak, quiet, 0.02 * quiet);
    EXPECT_NEAR(metered->rms, quiet / std::numbers::sqrt2, 0.02 * quiet);
    EXPECT_GE(metered->truePeak, metered->peak);
    EXPECT_TRUE(std::isfinite(metered->loudness)) << "a second of a tone has a loudness";
    if (c.channels == 2) {
        EXPECT_NEAR(metered->correlation, 1.0, 1e-3) << "Right is Left at half its level";
    } else {
        EXPECT_TRUE(std::isnan(metered->correlation)) << "a mono track has none";
    }
}

class BeatSpans : public ::testing::TestWithParam<ModeCase>
{
};

std::uint64_t beatSamples(double rate, double tempo, double quarterNotes)
{
    return static_cast<std::uint64_t>(std::llround(quarterNotes * 60.0 * rate / tempo));
}

// The samples of a beat span at `tempo` in the time signature Session plays (the host default).
std::uint64_t beatSpanSamples(double rate, double tempo, std::size_t span)
{
    return beatSamples(rate, tempo,
                       beatSpanQuarterNotes(static_cast<BeatSpan>(span),
                                            Timeline::barQuarterNotes(MusicalStamp{})));
}

// The sweep is whole once it has played, and frozen it stays whole: as it is, cropped to a shorter
// span that divides it, rebuilt from the held audio for a shorter one that does not and for a
// longer one, and at the zoom limit.
TEST_P(BeatSpans, DrawsTheWholeSweepLiveAndFrozen)
{
    const ModeCase c = GetParam();
    const double rate = c.rate;
    Session session(rate, c.channels);
    session.join({.frequency = 1000.0});
    session.join({.frequency = 1500.0});
    configure(session.own(), c.mode, TimeBase::kBeats);
    const auto span = static_cast<std::size_t>(BeatSpan::kQuarterBar);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(span));
    const std::size_t traces = expectedTraces(c.mode, c.channels, 2);
    const auto lengthOf = [&](std::size_t beatSpan) {
        return beatSpanSamples(rate, 120.0, beatSpan);
    };

    // Past a whole capture, then two and a half beats, producing frames as a page does.
    session.play(captureSeconds(rate) + 1.25, protocol::kReadoutIntervalSeconds / 2.0);
    auto frame = session.produce();
    if (!frame) {
        session.play(0.01);
        frame = session.produce();
    }
    ASSERT_TRUE(frame) << "no live frame";
    EXPECT_TRUE(frame->has(FrameFlag::kSynced));
    EXPECT_FALSE(frame->has(FrameFlag::kFrozen));
    EXPECT_FALSE(frame->has(FrameFlag::kPartial)) << "still filling after two passes";
    EXPECT_EQ(frame->visibleSamples, static_cast<float>(lengthOf(span)));
    EXPECT_GE(frame->cursor, 0.0f);
    EXPECT_LE(frame->cursor, 1.0f);
    expectTraces(*frame, traces);
    EXPECT_GT(frame->peak, 0.1f) << "readouts measured nothing";

    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / rate);
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frozen frame";
    ASSERT_TRUE(frame->has(FrameFlag::kFrozen));
    EXPECT_EQ(frame->visibleSamples, static_cast<float>(lengthOf(span)));
    expectTraces(*frame, traces);

    // Shorter and longer spans chosen while frozen, then back: 1/8 bar divides 1/4 bar, 1/6 does
    // not.
    for (const auto other : {static_cast<std::size_t>(BeatSpan::kEighthBar),
                             static_cast<std::size_t>(BeatSpan::kSixthBar),
                             static_cast<std::size_t>(BeatSpan::kHalfBar), span}) {
        SCOPED_TRACE(protocol::kBeatSpanNames.at(other));
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(other));
        frame = session.produce();
        ASSERT_TRUE(frame);
        EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
        EXPECT_NEAR(frame->visibleSamples, static_cast<float>(lengthOf(other)), 1.0f);
        expectTraces(*frame, traces);
    }

    const double width =
        protocol::kMinimumViewSamples / static_cast<double>(lengthOf(span)) * (1.0 - 1e-12);
    session.analyzer().setView(1.0 - width, 1.0);
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frame at the zoom limit";
    EXPECT_EQ(frame->viewStart, 1.0 - width);
    EXPECT_EQ(frame->viewEnd, 1.0);
    EXPECT_NEAR(frame->visibleSamples, protocol::kMinimumViewSamples, 0.01);
    SCOPED_TRACE("zoomed to the limit");
    expectTraces(*frame, traces);
}

INSTANTIATE_TEST_SUITE_P(EveryRate, BeatSpans, ::testing::ValuesIn(beatCases()), modeTestName);
INSTANTIATE_TEST_SUITE_P(EveryRate, FrozenShorterSpan, ::testing::ValuesIn(beatCases()),
                         modeTestName);

// --- Zoom and pan ---------------------------------------------------------------------------

// A frozen view zoomed to either half shows the same columns as that half of the whole view drawn
// with twice as many: the same audio, cut at the same places. Every trace, in every mode, in Beats
// (a one-beat sweep) and in Free (a rolling 2 s span), at the lowest and highest rates; the column
// count divides the span exactly, so the columns line up. (Trigger places its window by the
// trigger, between samples, not on a grid of columns.)
class ZoomedHalves : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(ZoomedHalves, ShowTheSameColumnsAsTheWholeView)
{
    const ModeCase c = GetParam();
    Session session(c.rate, c.channels);
    session.join({.kind = Signal::Kind::kNoise, .seed = 3});
    session.join({.kind = Signal::Kind::kNoise, .seed = 4});
    configure(session.own(), c.mode, c.timeBase);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kQuarterBar));
    const double spanSeconds = c.timeBase == TimeBase::kBeats ? 0.5 : 2.0;
    setSpan(session.own(), c.timeBase, spanSeconds);
    const auto half = static_cast<std::size_t>(std::llround(spanSeconds * c.rate / 2.0));
    std::size_t columns = 300;
    while (half % columns != 0) {
        --columns;
    }
    session.play(captureSeconds(c.rate) + 0.9);
    session.play(0.1, protocol::kReadoutIntervalSeconds / 2.0);
    session.analyzer().setFrozen(true);
    session.analyzer().setColumns(2 * columns);
    session.play(0.01);
    const auto whole = session.produce();
    ASSERT_TRUE(whole) << "no frame of the whole view";
    ASSERT_TRUE(whole->has(FrameFlag::kFrozen));
    ASSERT_EQ(whole->mode, FrameMode::kMinMax);
    ASSERT_EQ(whole->pointCount, 2 * columns);
    session.analyzer().setColumns(columns);
    for (const bool second : {false, true}) {
        SCOPED_TRACE(second ? "second half" : "first half");
        session.analyzer().setView(second ? 0.5 : 0.0, second ? 1.0 : 0.5);
        const auto zoomed = session.produce();
        ASSERT_TRUE(zoomed) << "no zoomed frame";
        ASSERT_EQ(zoomed->mode, FrameMode::kMinMax);
        ASSERT_EQ(zoomed->pointCount, columns);
        ASSERT_EQ(tracesIn(*zoomed), tracesIn(*whole));
        for (std::size_t index = 0; index < tracesIn(*whole); ++index) {
            const std::span<const float> all = trace(*whole, index);
            const std::span<const float> part =
                all.subspan(second ? all.size() / 2 : 0, all.size() / 2);
            const std::span<const float> shown = trace(*zoomed, index);
            EXPECT_TRUE(std::ranges::equal(shown, part)) << "trace " << index;
        }
    }
}

// Live in Beats, a zoomed sweep keeps its columns from frame to frame and reduces again only those
// that new audio landed in; panned at the same width, it starts again from the new part. After
// zooming into the second quarter and playing, and again on the first frame after panning to the
// third (the same width), every trace is what a new analyzer draws of that part at once. In every
// mode, at the lowest and highest rates.
class LiveZoom : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(LiveZoom, KeepsZoomedAndPannedTracesInStep)
{
    const ModeCase c = GetParam();
    Session session(c.rate, c.channels);
    session.join({.kind = Signal::Kind::kNoise, .seed = 5});
    session.join({.kind = Signal::Kind::kNoise, .seed = 6});
    configure(session.own(), c.mode, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kQuarterBar));
    constexpr std::size_t kZoomedColumns = 200;
    session.analyzer().setColumns(kZoomedColumns);
    session.play(captureSeconds(c.rate) + 1.0, protocol::kReadoutIntervalSeconds / 2.0);
    for (const auto& [start, end] : {std::pair{0.25, 0.5}, std::pair{0.5, 0.75}}) {
        SCOPED_TRACE(testing::Message() << "view " << start << " to " << end);
        const bool panned = start > 0.25;
        session.analyzer().setView(start, end);
        if (!panned) { // Zoomed, frames go on; panned, the first frame shows the new part.
            session.play(0.3, protocol::kReadoutIntervalSeconds / 2.0);
        }
        session.play(0.01);
        const auto frame = session.produce();
        ASSERT_TRUE(frame) << "no frame";
        ASSERT_TRUE(frame->has(FrameFlag::kSynced));
        Analyzer fresh(session.own(), session.registry);
        fresh.setColumns(kZoomedColumns);
        fresh.setView(start, end);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(fresh.produce(bytes));
        const DecodedFrame expected = decode(bytes);
        ASSERT_EQ(tracesIn(*frame), tracesIn(expected));
        ASSERT_EQ(frame->pointCount, expected.pointCount);
        for (std::size_t index = 0; index < tracesIn(expected); ++index) {
            EXPECT_TRUE(std::ranges::equal(trace(*frame, index), trace(expected, index)))
                << "trace " << index;
        }
    }
}

std::vector<ModeCase> zoomCases()
{
    std::vector<ModeCase> cases;
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
            for (const Display mode : everyMode()) {
                cases.push_back({.rate = rate,
                                 .mode = mode,
                                 .channels = rate == kSampleRates.front() ? 2 : 1,
                                 .timeBase = static_cast<TimeBase>(timeBase)});
            }
        }
    }
    return cases;
}

// The Compare cases of zoomCases().
std::vector<ModeCase> zoomedCompareCases()
{
    std::vector<ModeCase> cases = zoomCases();
    std::erase_if(cases, [](const ModeCase& c) { return c.mode != Display::kCompare; });
    return cases;
}

// The cases of zoomCases() whose columns sit on a grid: Beats and Free.
std::vector<ModeCase> gridZoomCases()
{
    std::vector<ModeCase> cases = zoomCases();
    std::erase_if(cases, [](const ModeCase& c) { return c.timeBase == TimeBase::kTrigger; });
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, ZoomedHalves, ::testing::ValuesIn(gridZoomCases()),
                         modeTestName);

// The Meter measures the visible part: a span with one sample that is not finite every 100 ms,
// zoomed onto either half, counts what each half shows, the two adding up to the whole, and reads
// each half's peak and RMS, of which the whole's are the larger peak and the power between them.
// Frozen, so each view shows the same audio. Live, a zoom is measured in the next frame, without
// waiting for 100 ms of audio: as an analyzer measuring afresh reads it. In every mode and time
// base, at the lowest and highest rates.
class MeterView : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(MeterView, MeasuresTheVisiblePart)
{
    const ModeCase c = GetParam();
    // With the Meter off, from the levels per block; on, from the Meter's own statistics.
    for (const bool meter : {false, true}) {
        SCOPED_TRACE(meter ? "Meter on" : "Meter off");
        Session session(c.rate, c.channels);
        session.join({.kind = Signal::Kind::kNoise,
                      .seed = 3,
                      .quieterEvery = std::llround(c.rate * 0.07),
                      .brokenEvery = std::llround(c.rate / 10.0)});
        session.join({.frequency = 1500.0});
        configure(session.own(), c.mode, c.timeBase);
        setMeter(session.own(), meter);
        setSpan(session.own(), c.timeBase, 1.0);
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kHalfBar));
        session.play(c.timeBase == TimeBase::kBeats ? 2.5 : 1.5, protocol::kReadoutIntervalSeconds);

        // Live: zoomed onto the second half, the next frame measures it.
        session.analyzer().setView(0.5, 1.0);
        session.play(0.005);
        const auto live = session.produce();
        ASSERT_TRUE(live);
        Analyzer fresh(session.own(), session.registry);
        fresh.setColumns(kColumns);
        fresh.setView(0.5, 1.0);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(fresh.produce(bytes));
        const DecodedFrame expected = decode(bytes);
        EXPECT_EQ(live->invalidSamples, expected.invalidSamples) << "live zoom measured at once";
        EXPECT_FLOAT_EQ(live->peak, expected.peak) << "live zoom measured at once";

        // Frozen: the whole, then each half, of the same audio.
        session.analyzer().setView(0.0, 1.0);
        session.analyzer().setFrozen(true);
        const auto whole = session.produce();
        ASSERT_TRUE(whole);
        session.analyzer().setView(0.0, 0.5);
        const auto first = session.produce();
        ASSERT_TRUE(first);
        session.analyzer().setView(0.5, 1.0);
        const auto second = session.produce();
        ASSERT_TRUE(second);
        EXPECT_GE(whole->invalidSamples, 4U);
        EXPECT_LT(first->invalidSamples, whole->invalidSamples) << "the first half shows fewer";
        EXPECT_LT(second->invalidSamples, whole->invalidSamples) << "the second half shows fewer";
        EXPECT_EQ(first->invalidSamples + second->invalidSamples, whole->invalidSamples)
            << "each counted once";
        EXPECT_FLOAT_EQ(whole->peak, std::max(first->peak, second->peak));
        const double halves = ((first->rms * first->rms) + (second->rms * second->rms)) / 2.0;
        EXPECT_NEAR(whole->rms * whole->rms, halves, 2e-3 * halves);
        EXPECT_NE(first->rms, second->rms) << "the halves read their own audio";
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, MeterView, ::testing::ValuesIn(zoomCases()), modeTestName);

// The Meter's other rows through the analyzer: a 1 kHz tone of amplitude 0.25 over a DC offset of
// 0.5, its right channel the left's tone inverted over the same offset. True peak at or above
// Peak; DC the offset; Loudness the tone's in both channels (the K-weighting's high-pass takes the
// offset out): 10·log10(2 · 0.25² / 2) − 0.691 + the filter's +0.691 dB at 1 kHz, −12.04 LUFS;
// Correlation at zero offset, which counts the offset common to both: (0.5² − 0.25²/2) / (0.5² +
// 0.25²/2) = 0.778, read from Left and Right whatever channel the levels are measured on (Left
// here, Mid in Multi's lanes). A mono
// track has no correlation and its one channel's loudness, 3 dB lower. With the Meter on, in every
// mode and time base, live and frozen, at the lowest and highest rates.
class MeterRows : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(MeterRows, ReadLeftAndRightAndTheLevelsChannel)
{
    const ModeCase c = GetParam();
    Session session(c.rate, c.channels);
    session.join({.frequency = 1000.0, .rightPhase = std::numbers::pi});
    session.join({.frequency = 1500.0});
    configure(session.own(), c.mode, c.timeBase);
    setMeter(session.own(), true);
    session.own().settings().set(SettingId::kReadoutSource,
                                 static_cast<double>(protocol::ChannelView::kLeft));
    setSpan(session.own(), c.timeBase, 1.0);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kHalfBar));
    session.play(c.timeBase == TimeBase::kBeats ? 2.5 : 1.5, protocol::kReadoutIntervalSeconds);
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        if (frozen) {
            session.analyzer().setFrozen(true);
        } else {
            session.play(0.2, protocol::kReadoutIntervalSeconds);
        }
        const auto frame = session.produce();
        ASSERT_TRUE(frame);
        EXPECT_GE(frame->truePeak, frame->peak);
        EXPECT_NEAR(frame->dcOffset, 0.5, 0.01);
        if (c.channels == 2) {
            EXPECT_NEAR(frame->correlation, 0.7778, 1e-3);
            EXPECT_NEAR(frame->loudness, -12.04, 0.15);
        } else {
            EXPECT_TRUE(std::isnan(frame->correlation)) << "a mono track has none";
            EXPECT_NEAR(frame->loudness, -15.05, 0.15);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, MeterRows, ::testing::ValuesIn(zoomCases()), modeTestName);

// With the Meter off nothing shows True peak, DC, Loudness or Correlation, and the engine does not
// measure them: the frame carries no value for the four, the Peak, RMS and invalid count it would
// with the Meter on, and in a sweep reads less audio (Left and Right are not read). Switched on,
// the next frame carries all four, as an engine that had the Meter on throughout measures them,
// without waiting for 100 ms of audio; switched off, none again. Live, then frozen, in every mode
// and time base, at the lowest and highest rates.
class MeterOff : public ::testing::TestWithParam<ModeCase>
{
protected:
    static void expectLevelsAlone(const DecodedFrame& frame)
    {
        EXPECT_TRUE(std::isnan(frame.truePeak)) << "True peak measured with the Meter off";
        EXPECT_TRUE(std::isnan(frame.dcOffset)) << "DC measured with the Meter off";
        EXPECT_TRUE(std::isnan(frame.loudness)) << "Loudness measured with the Meter off";
        EXPECT_TRUE(std::isnan(frame.correlation)) << "Correlation measured with the Meter off";
        EXPECT_TRUE(std::isfinite(frame.peak) && std::isfinite(frame.rms)) << "the levels stay";
    }

    static void expectSameRows(const DecodedFrame& frame, const DecodedFrame& expected,
                               int channels)
    {
        EXPECT_FLOAT_EQ(frame.peak, expected.peak);
        EXPECT_FLOAT_EQ(frame.rms, expected.rms);
        EXPECT_EQ(frame.invalidSamples, expected.invalidSamples);
        EXPECT_NEAR(frame.truePeak, expected.truePeak, 1e-6);
        EXPECT_NEAR(frame.dcOffset, expected.dcOffset, 1e-6);
        EXPECT_NEAR(frame.loudness, expected.loudness, 1e-3);
        if (channels == 2) {
            EXPECT_NEAR(frame.correlation, expected.correlation, 1e-6);
        } else {
            EXPECT_TRUE(std::isnan(frame.correlation)) << "a mono track has none";
        }
    }
};

TEST_P(MeterOff, MeasuresTheLevelsAloneUntilSwitchedOn)
{
    const ModeCase c = GetParam();
    // The same audio, broken on every channel every 100 ms, in two sessions: the Meter off in
    // one, on in the other.
    const auto start = [&](Session& session, bool meter) {
        session.join({.frequency = 1000.0,
                      .brokenEvery = std::llround(c.rate / 10.0),
                      .rightPhase = std::numbers::pi});
        session.join({.frequency = 1500.0});
        configure(session.own(), c.mode, c.timeBase);
        setMeter(session.own(), meter);
        setSpan(session.own(), c.timeBase, 1.0);
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kHalfBar));
        session.play(c.timeBase == TimeBase::kBeats ? 2.5 : 1.5, protocol::kReadoutIntervalSeconds);
    };
    Session off(c.rate, c.channels);
    Session on(c.rate, c.channels);
    start(off, false);
    start(on, true);
    // Zoomed onto a part whose edges fall inside the blocks the levels are kept in, so that both
    // read the samples at the edges.
    off.analyzer().setView(0.2137, 0.8311);
    on.analyzer().setView(0.2137, 0.8311);

    const std::uint64_t offBefore = off.analyzer().work().samplesRead;
    const std::uint64_t onBefore = on.analyzer().work().samplesRead;
    off.play(0.2, protocol::kReadoutIntervalSeconds);
    on.play(0.2, protocol::kReadoutIntervalSeconds);
    const auto levels = off.produce();
    const auto all = on.produce();
    ASSERT_TRUE(levels);
    ASSERT_TRUE(all);
    expectLevelsAlone(*levels);
    EXPECT_FLOAT_EQ(levels->peak, all->peak);
    EXPECT_FLOAT_EQ(levels->rms, all->rms);
    EXPECT_GT(levels->invalidSamples, 0U);
    EXPECT_EQ(levels->invalidSamples, all->invalidSamples);
    EXPECT_TRUE(std::isfinite(all->loudness)) << "measured with the Meter on";
    if (c.timeBase == TimeBase::kBeats) {
        // In a sweep both measure the new audio: the Meter on reads Left and Right of it too.
        // (A window's levels, with the Meter off, come from the visible part read again, where
        // the Meter on follows the new audio: MeterStream.)
        EXPECT_LT(off.analyzer().work().samplesRead - offBefore,
                  on.analyzer().work().samplesRead - onBefore)
            << "Left and Right read with the Meter off";
    }

    {
        SCOPED_TRACE("switched on, live");
        setMeter(off.own(), true);
        off.play(0.005);
        on.play(0.005);
        const auto switched = off.produce();
        const auto kept = on.produce();
        ASSERT_TRUE(switched);
        ASSERT_TRUE(kept);
        expectSameRows(*switched, *kept, c.channels);
    }
    {
        SCOPED_TRACE("switched off, live");
        setMeter(off.own(), false);
        off.play(0.005);
        // The same audio and the same last frame in both, so that their frozen windows are the
        // same.
        on.play(0.005);
        ASSERT_TRUE(on.produce());
        const auto again = off.produce();
        ASSERT_TRUE(again);
        expectLevelsAlone(*again);
    }
    {
        SCOPED_TRACE("frozen");
        off.analyzer().setFrozen(true);
        on.analyzer().setFrozen(true);
        const auto held = off.produce();
        const auto kept = on.produce();
        ASSERT_TRUE(held);
        ASSERT_TRUE(kept);
        expectLevelsAlone(*held);
        setMeter(off.own(), true);
        const auto switched = off.produce();
        ASSERT_TRUE(switched);
        // The levels are those read with the Meter off, of the same view.
        EXPECT_FLOAT_EQ(switched->peak, held->peak);
        EXPECT_NEAR(switched->rms, held->rms, 1e-6 * held->rms) << "switched on against off";
        EXPECT_NEAR(kept->rms, held->rms, 1e-6 * held->rms) << "on throughout against off";
        expectSameRows(*switched, *kept, c.channels);
        // And off again, still frozen: the levels alone, of the same picture.
        setMeter(off.own(), false);
        const auto again = off.produce();
        ASSERT_TRUE(again);
        expectLevelsAlone(*again);
        EXPECT_FLOAT_EQ(again->peak, held->peak);
        EXPECT_FLOAT_EQ(again->rms, held->rms);
        EXPECT_EQ(again->invalidSamples, held->invalidSamples);
        if (c.mode != Display::kCompare) {
            // Compare's matches, searched again at the freeze, may still arrive between frames.
            EXPECT_TRUE(sameValues(again->data, switched->data)) << "the picture changed";
        }
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, MeterOff, ::testing::ValuesIn(zoomCases()), modeTestName);

// Beats chosen while frozen in Trigger or Free draws the sweep of the audio held, in Multi and
// Compare too, on a stereo track as on a mono one: this track's lane, which holds both channels
// of a stereo track for the Meter, is built from audio held in both.
std::vector<ModeCase> windowCases();

class FrozenIntoBeats : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(FrozenIntoBeats, DrawsTheSweepOfTheAudioHeld)
{
    const ModeCase c = GetParam();
    for (const int channels : {1, 2}) {
        for (const bool meter : {false, true}) {
            SCOPED_TRACE(testing::Message()
                         << channels << " channels, the Meter " << (meter ? "on" : "off"));
            Session session(c.rate, channels);
            session.join({.frequency = 1000.0});
            session.join({.frequency = 1500.0});
            configure(session.own(), c.mode, c.timeBase);
            setMeter(session.own(), meter);
            session.own().settings().set(SettingId::kBeatSpan,
                                         static_cast<double>(BeatSpan::kHalfBar));
            session.play(3.0, protocol::kReadoutIntervalSeconds);
            session.analyzer().setFrozen(true);
            ASSERT_TRUE(session.produce()) << "no frozen frame";
            session.own().settings().set(SettingId::kTimeBase,
                                         static_cast<double>(TimeBase::kBeats));
            const auto frame = session.produce();
            ASSERT_TRUE(frame) << "no frame once Beats is chosen";
            EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
            EXPECT_GT(frame->pointCount, 0U);
            EXPECT_NEAR(frame->peak, isMulti(c.mode) && channels == 2 ? 0.5625 : 0.75, 0.01)
                << "the Meter reads the held tone";
            EXPECT_EQ(std::isfinite(frame->loudness), meter);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, FrozenIntoBeats, ::testing::ValuesIn(windowCases()),
                         modeTestName);

// The Beats cases of zoomCases().
std::vector<ModeCase> sweepCases()
{
    std::vector<ModeCase> cases = zoomCases();
    std::erase_if(cases, [](const ModeCase& c) { return c.timeBase != TimeBase::kBeats; });
    return cases;
}

// The Meter of a sweep reads its visible part to the sample, not to the 10 ms blocks its
// statistics are kept in (as it did: a view's readouts took in up to a block of audio either side
// that was not on screen). One sample in every hundred is not finite, and a view whose edges fall
// inside blocks counts those on screen, at positions from viewStart to viewEnd of the sweep's
// length, and no other. Live and frozen, with the Meter off (from the levels per block) and on
// (from the Meter's own), in Single, Multi and Compare, at the lowest and highest rates.
class SweepEdge : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(SweepEdge, IsMeasuredToTheSample)
{
    const ModeCase c = GetParam();
    constexpr std::int64_t kEvery = 100;
    constexpr double kViewStart = 0.2137;
    constexpr double kViewEnd = 0.8311;
    // A half bar at 120 BPM: a second, a whole number of the hundreds.
    const double length = c.rate;
    ASSERT_EQ(std::llround(length) % kEvery, 0);
    const auto first = static_cast<std::int64_t>(std::ceil(kViewStart * length));
    const auto end = static_cast<std::int64_t>(std::ceil(kViewEnd * length));
    ASSERT_NE(first % std::llround(c.rate / 100.0), 0) << "the view starts on a block's edge";
    ASSERT_NE(end % std::llround(c.rate / 100.0), 0) << "the view ends on a block's edge";
    // The multiples of a hundred from `first` up to, not including, `end`.
    const std::int64_t onScreen = ((end + kEvery - 1) / kEvery) - ((first + kEvery - 1) / kEvery);
    for (const bool meter : {false, true}) {
        Session session(c.rate, c.channels);
        session.join({.frequency = 1000.0, .brokenEvery = kEvery});
        session.join({.frequency = 1500.0});
        configure(session.own(), c.mode, TimeBase::kBeats);
        setMeter(session.own(), meter);
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kHalfBar));
        session.play(2.5, protocol::kReadoutIntervalSeconds);
        session.analyzer().setView(kViewStart, kViewEnd);
        session.play(0.005);
        for (const bool frozen : {false, true}) {
            SCOPED_TRACE(testing::Message()
                         << (meter ? "Meter on" : "Meter off") << (frozen ? ", frozen" : ", live"));
            if (frozen) {
                session.analyzer().setFrozen(true);
            }
            const auto frame = session.produce();
            ASSERT_TRUE(frame);
            EXPECT_EQ(frame->invalidSamples, static_cast<std::uint32_t>(onScreen));
        }
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, SweepEdge, ::testing::ValuesIn(sweepCases()), modeTestName);

// A host changes a track's channels while its sweep plays (a mono track made stereo, and back).
// The sweep kept of this track is then drawn again with the channels the track now has, so the
// Meter's Loudness and Correlation read Left and Right once it is stereo, as on a track stereo
// all along, and its one channel once it is mono; a sweep kept with one channel would go on
// reading Left alone, with no correlation. In Beats, in Single, Multi and Compare, at the lowest
// and highest rates, against sessions that never saw the change.
class SweepChannels : public ::testing::TestWithParam<ModeCase>
{
protected:
    static void expectSameRows(const DecodedFrame& frame, const DecodedFrame& expected)
    {
        EXPECT_FLOAT_EQ(frame.peak, expected.peak);
        EXPECT_FLOAT_EQ(frame.rms, expected.rms);
        EXPECT_EQ(frame.invalidSamples, expected.invalidSamples);
        EXPECT_NEAR(frame.truePeak, expected.truePeak, 1e-6);
        EXPECT_NEAR(frame.dcOffset, expected.dcOffset, 1e-6);
        EXPECT_NEAR(frame.loudness, expected.loudness, 1e-3);
        if (std::isnan(expected.correlation)) {
            EXPECT_TRUE(std::isnan(frame.correlation)) << "correlation " << frame.correlation;
        } else {
            EXPECT_NEAR(frame.correlation, expected.correlation, 1e-5);
        }
    }
};

TEST_P(SweepChannels, AreThoseTheMeterReadsOnceTheTrackChanges)
{
    const ModeCase c = GetParam();
    // The same audio in three sessions, Right out of phase with Left (a correlation under 1): one
    // whose track changes, one stereo and one mono throughout.
    const auto start = [&](Session& session, int channels) {
        session.join({.frequency = 1000.0, .rightPhase = std::numbers::pi / 3.0});
        session.join({.frequency = 1500.0});
        session.own().setInputChannels(channels);
        configure(session.own(), c.mode, TimeBase::kBeats);
        setMeter(session.own(), true);
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kHalfBar));
    };
    Session changed(c.rate, 2);
    Session stereo(c.rate, 2);
    Session mono(c.rate, 2);
    start(changed, 1);
    start(stereo, 2);
    start(mono, 1);
    // More than two sweeps of a second each: all of the sweep shown is from after a change.
    const auto playAll = [&] {
        for (Session* session : {&changed, &stereo, &mono}) {
            session->play(2.5, protocol::kReadoutIntervalSeconds);
        }
    };
    const auto expectAs = [&](Session& reference, bool correlated) {
        const auto frame = changed.produce();
        const auto expected = reference.produce();
        ASSERT_TRUE(frame);
        ASSERT_TRUE(expected);
        // Right is not Left: a stereo track's correlation is a value under 1, a mono track's none.
        ASSERT_EQ(std::isfinite(expected->correlation), correlated);
        ASSERT_FALSE(expected->correlation > 0.99F);
        expectSameRows(*frame, *expected);
        if (c.mode != Display::kCompare) {
            // (In Compare the matches of the two sessions need not arrive in the same frame.)
            EXPECT_TRUE(sameValues(frame->data, expected->data)) << "the picture differs";
        }
    };
    playAll();
    {
        SCOPED_TRACE("mono from the start");
        expectAs(mono, false);
    }
    changed.own().setInputChannels(2);
    playAll();
    {
        SCOPED_TRACE("made stereo");
        expectAs(stereo, true);
    }
    changed.own().setInputChannels(1);
    playAll();
    {
        SCOPED_TRACE("made mono again");
        expectAs(mono, false);
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, SweepChannels, ::testing::ValuesIn(sweepCases()), modeTestName);

// The Trigger and Free cases of zoomCases().
std::vector<ModeCase> windowCases()
{
    std::vector<ModeCase> cases = zoomCases();
    std::erase_if(cases, [](const ModeCase& c) { return c.timeBase == TimeBase::kBeats; });
    return cases;
}

// In Trigger and Free, with the Meter on, the engine keeps the Meter's statistics per block of
// this track's captured audio and follows it, as a sweep's in Beats: a readout measures the audio
// that arrived since the last one, not the whole view again (a 4 s view at 96 kHz was 384,000
// samples through the true-peak and K-weighting filters ten times a second, and on every frame of
// a frozen pan). What it reads stays what an analyzer measuring afresh reads: playing on in uneven
// steps, zoomed, frozen and panned back to the oldest audio held, resumed after a short freeze and
// after a long one, and after the readout channel or the track's channels change. In Single,
// Multi and Compare, at the lowest and highest rates.
class MeterStream : public ::testing::TestWithParam<ModeCase>
{
protected:
    static void expectSameRows(const DecodedFrame& frame, const DecodedFrame& expected)
    {
        EXPECT_FLOAT_EQ(frame.peak, expected.peak);
        EXPECT_FLOAT_EQ(frame.rms, expected.rms);
        EXPECT_EQ(frame.invalidSamples, expected.invalidSamples);
        const auto same = [](float value, float wanted, double tolerance, const char* name) {
            if (std::isnan(wanted)) {
                EXPECT_TRUE(std::isnan(value)) << name;
            } else {
                EXPECT_NEAR(value, wanted, tolerance) << name;
            }
        };
        same(frame.truePeak, expected.truePeak, 1e-6, "true peak");
        same(frame.dcOffset, expected.dcOffset, 1e-6, "DC");
        same(frame.loudness, expected.loudness, 1e-3, "loudness");
        same(frame.correlation, expected.correlation, 1e-5, "correlation");
    }

    // What an analyzer new to the session, measuring afresh, reads of the view.
    static DecodedFrame afresh(Session& session, double viewStart, double viewEnd)
    {
        Analyzer fresh(session.own(), session.registry);
        fresh.setColumns(kColumns);
        fresh.setView(viewStart, viewEnd);
        std::vector<std::byte> bytes;
        EXPECT_TRUE(fresh.produce(bytes));
        return decode(bytes);
    }
};

TEST_P(MeterStream, MeasuresTheNewAudioAloneAndReadsAsAfresh)
{
    const ModeCase c = GetParam();
    const double span = c.timeBase == TimeBase::kFree ? 4.0 : 1.0;
    // The same audio and the same steps in two sessions, the Meter on in one alone: what the
    // Meter reads of the audio is the difference between them.
    const auto start = [&](Session& session, bool meter) {
        session.join({.kind = Signal::Kind::kNoise,
                      .seed = 3,
                      .quieterEvery = std::llround(c.rate * 0.37),
                      .brokenEvery = std::llround(c.rate * 0.53)});
        session.join({.frequency = 1500.0});
        configure(session.own(), c.mode, c.timeBase);
        setMeter(session.own(), meter);
        setSpan(session.own(), c.timeBase, span);
        session.play(5.0, protocol::kReadoutIntervalSeconds);
    };
    Session on(c.rate, c.channels);
    Session off(c.rate, c.channels);
    start(on, true);
    start(off, false);
    // A build over the view reads it once for each channel the Meter reads: the bound is far
    // under that, and over what following reads (the new audio three times, and part of a block
    // per 100 ms of the view in two channels).
    const auto bound = static_cast<std::uint64_t>((0.4 + (0.4 * span)) * c.rate);
    const auto meterRead = [&](const auto& step) {
        const std::uint64_t onBefore = on.analyzer().work().samplesRead;
        const std::uint64_t offBefore = off.analyzer().work().samplesRead;
        step();
        const std::uint64_t with = on.analyzer().work().samplesRead - onBefore;
        const std::uint64_t without = off.analyzer().work().samplesRead - offBefore;
        return with > without ? with - without : 0;
    };

    double viewStart = 0.0;
    double viewEnd = 1.0;
    std::optional<DecodedFrame> frame;
    const auto playBoth = [&](double seconds) {
        on.play(seconds);
        off.play(seconds);
        frame = on.produce();
        (void)off.produce();
    };
    for (int round = 0; round < 6; ++round) {
        SCOPED_TRACE(testing::Message() << "live, round " << round);
        if (round == 2) {
            viewStart = 0.21;
            viewEnd = 0.83;
        } else if (round == 4) {
            viewStart = 0.0;
            viewEnd = 1.0;
        }
        on.analyzer().setView(viewStart, viewEnd);
        off.analyzer().setView(viewStart, viewEnd);
        const std::uint64_t read = meterRead([&] { playBoth(0.113 + (0.019 * round)); });
        ASSERT_TRUE(frame);
        EXPECT_TRUE(std::isfinite(frame->truePeak)) << "the Meter is on";
        expectSameRows(*frame, afresh(on, viewStart, viewEnd));
        EXPECT_LT(read, bound) << "the Meter read the view again, not the new audio";
    }

    // Frozen at a moment inside a block: panned back through the audio held and zoomed, each
    // frame reads as a session whose Meter is switched on for it, and reads the edges alone.
    on.analyzer().setFrozen(true);
    off.analyzer().setFrozen(true);
    (void)on.produce();
    (void)off.produce();
    const double reach = c.timeBase == TimeBase::kFree ? 0.0 : 3.0;
    for (int step = 0; step < 5; ++step) {
        SCOPED_TRACE(testing::Message() << "frozen, step " << step);
        const double pan = reach * step / 4.0;
        const double zoomStart = step % 2 == 1 ? 0.3 : 0.0;
        const double zoomEnd = step % 2 == 1 ? 0.9 : 1.0;
        const std::uint64_t read = meterRead([&] {
            for (Session* session : {&on, &off}) {
                session->analyzer().setPan(pan);
                session->analyzer().setView(zoomStart, zoomEnd);
            }
            frame = on.produce();
            (void)off.produce();
        });
        if (!frame) {
            continue; // Nothing moved: a view already there.
        }
        setMeter(off.own(), true);
        const auto fresh = off.produce();
        ASSERT_TRUE(fresh);
        expectSameRows(*frame, *fresh);
        setMeter(off.own(), false);
        (void)off.produce();
        EXPECT_LT(read, bound) << "a frozen view measured again in full";
    }
    for (Session* session : {&on, &off}) {
        session->analyzer().setPan(0.0);
        session->analyzer().setView(0.0, 1.0);
        session->analyzer().setFrozen(false);
    }

    {
        SCOPED_TRACE("resumed after a short freeze");
        const std::uint64_t read = meterRead([&] { playBoth(0.2); });
        ASSERT_TRUE(frame);
        expectSameRows(*frame, afresh(on, 0.0, 1.0));
        EXPECT_LT(read, bound) << "the blocks did not follow through the freeze";
    }
    {
        SCOPED_TRACE("resumed after the audio played on past what is kept");
        for (Session* session : {&on, &off}) {
            session->analyzer().setFrozen(true);
            (void)session->produce();
            session->play(5.0);
            session->analyzer().setFrozen(false);
        }
        playBoth(0.15);
        ASSERT_TRUE(frame);
        expectSameRows(*frame, afresh(on, 0.0, 1.0));
    }
    {
        SCOPED_TRACE("another readout channel");
        for (Session* session : {&on, &off}) {
            const SettingId setting =
                isMulti(c.mode) ? SettingId::kLaneChannel : SettingId::kReadoutSource;
            session->own().settings().set(setting,
                                          static_cast<double>(protocol::ChannelView::kRight));
        }
        playBoth(0.15);
        ASSERT_TRUE(frame);
        expectSameRows(*frame, afresh(on, 0.0, 1.0));
    }
    {
        // Against the twin session, its Meter switched on for the frame: an analyzer new to the
        // session need not show the window one that lived through the change shows.
        SCOPED_TRACE("the track's channels changed");
        for (Session* session : {&on, &off}) {
            session->own().setInputChannels(c.channels == 2 ? 1 : 2);
        }
        playBoth(0.15);
        ASSERT_TRUE(frame);
        setMeter(off.own(), true);
        const auto fresh = off.produce();
        ASSERT_TRUE(fresh);
        expectSameRows(*frame, *fresh);
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, MeterStream, ::testing::ValuesIn(windowCases()), modeTestName);

// A trace Normal holds while no trigger comes stays measured as it stands, also once it is older
// than the audio the Meter's blocks keep: a tone that stops, its last trace held, read zoomed while
// the blocks still cover it and again after the audio has played on past them (longer than a
// freeze holds): the same readouts, of the tone, not of the silence since.
TEST(MeterStreamHeld, MeasuresATraceOlderThanTheBlocksKeep)
{
    const double rate = 48000.0;
    Session session(rate, 2);
    session.join({.frequency = 1000.0,
                  .amplitude = 0.25,
                  .offset = 0.0,
                  .endsAt = std::llround(2.0 * rate)});
    configure(session.own(), Display::kSingle, TimeBase::kTrigger);
    setMeter(session.own(), true);
    session.own().settings().set(SettingId::kTriggerMode,
                                 static_cast<double>(protocol::TriggerMode::kNormal));
    setSpan(session.own(), TimeBase::kTrigger, 1.0);
    // The tone, then silence: the last trace is held from here on, the tone's end in its first
    // tenth (before the trigger point) and silence after it.
    session.play(3.5, protocol::kReadoutIntervalSeconds);
    session.analyzer().setView(0.0, 0.6);
    const auto covered = session.produce();
    ASSERT_TRUE(covered);
    ASSERT_NEAR(covered->peak, 0.25, 0.01) << "the held trace is not the tone";
    ASSERT_TRUE(std::isfinite(covered->loudness));
    session.analyzer().setView(0.0, 1.0);
    ASSERT_TRUE(session.produce());
    session.play(6.0, protocol::kReadoutIntervalSeconds);
    session.analyzer().setView(0.0, 0.6);
    const auto held = session.produce();
    ASSERT_TRUE(held);
    EXPECT_FLOAT_EQ(held->peak, covered->peak);
    EXPECT_FLOAT_EQ(held->rms, covered->rms);
    EXPECT_NEAR(held->truePeak, covered->truePeak, 1e-6);
    EXPECT_NEAR(held->dcOffset, covered->dcOffset, 1e-6);
    EXPECT_NEAR(held->loudness, covered->loudness, 1e-3);
    EXPECT_NEAR(held->correlation, covered->correlation, 1e-5);
}

// A held trace older than the blocks keep is measured to its last sample: a view that ends just
// after a crest between two samples reads that crest as true peak, as it did while the blocks
// covered the trace, not the sample peak below it. A tone of 90 samples a cycle crests halfway
// between two samples; views narrower than half a cycle hold one crest at most, and stepped a
// sample at a time across a cycle they end one to six samples after it in turn.
TEST(MeterStreamHeld, ReadsACrestInTheLastSamplesOfTheView)
{
    const double rate = 48000.0;
    const double span = 0.1;
    Session session(rate, 2);
    session.join({.frequency = rate / 90.0,
                  .amplitude = 0.5,
                  .offset = 0.0,
                  .endsAt = std::llround(2.0 * rate)});
    configure(session.own(), Display::kSingle, TimeBase::kTrigger);
    setMeter(session.own(), true);
    session.own().settings().set(SettingId::kTriggerMode,
                                 static_cast<double>(protocol::TriggerMode::kNormal));
    setSpan(session.own(), TimeBase::kTrigger, span);
    session.play(3.5, protocol::kReadoutIntervalSeconds);
    // Views of 34 samples inside the tone before the trigger point, a tenth into the trace.
    const double samples = span * rate;
    const auto viewAt = [&](int step) {
        const double first = 200.0 + step;
        session.analyzer().setView(first / samples, (first + 34.0) / samples);
        return session.produce();
    };
    constexpr int kSteps = 90;
    std::vector<DecodedFrame> covered;
    int crests = 0;
    for (int step = 0; step < kSteps; ++step) {
        const auto frame = viewAt(step);
        ASSERT_TRUE(frame);
        ASSERT_GT(frame->peak, 0.1f) << "the view is not on the tone";
        crests += frame->truePeak > frame->peak * 1.0001f ? 1 : 0;
        covered.push_back(*frame);
    }
    ASSERT_GT(crests, kSteps / 3) << "views with a crest between their samples";
    session.analyzer().setView(0.0, 1.0);
    ASSERT_TRUE(session.produce());
    session.play(6.0, protocol::kReadoutIntervalSeconds);
    for (int step = 0; step < kSteps; ++step) {
        SCOPED_TRACE(step);
        const auto held = viewAt(step);
        ASSERT_TRUE(held);
        EXPECT_FLOAT_EQ(held->peak, covered[static_cast<std::size_t>(step)].peak);
        EXPECT_NEAR(held->truePeak, covered[static_cast<std::size_t>(step)].truePeak, 1e-6);
    }
}

// Switching the Meter leaves the picture alone, also over a sweep longer than the capture holds
// (sixteen bars at 120 BPM are 32 s; the capture holds under 24 s at 44.1 kHz): this track's lane
// in Multi stays as drawn, where a lane built again for the switch would come back blank in the
// part the capture no longer has. Live, on and then off, with no new audio between the frames.
TEST(MeterSwitch, KeepsALongSweepAsDrawn)
{
    const double rate = 44100.0;
    Session session(rate, 2);
    session.join({.frequency = 1000.0, .rightPhase = std::numbers::pi / 2.0});
    session.join({.frequency = 1500.0});
    configure(session.own(), Display::kMulti, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kSixteenBars));
    session.play(33.0, 1.0);
    ASSERT_GT(33.0 * rate, static_cast<double>(session.own().captureRing().capacity()))
        << "the sweep must be longer than the capture";
    const auto drawn = session.produce();
    ASSERT_TRUE(drawn);
    for (const bool meter : {true, false}) {
        SCOPED_TRACE(meter ? "switched on" : "switched off");
        setMeter(session.own(), meter);
        const auto frame = session.produce();
        ASSERT_TRUE(frame);
        EXPECT_TRUE(sameValues(frame->data, drawn->data)) << "the picture changed";
        EXPECT_EQ(std::isfinite(frame->loudness), meter);
    }
}

// A tempo-synced frame says where its cycle starts in the song, in quarter notes and in the
// host's project time, and how long its span and the host's bar are, which the page draws the beat
// grid and the hover time from. At 120 BPM in 4/4, 5.3 s in, a 1-bar span starts at quarter note 8
// and a 2-beat span at 10; the project time is the host's, here 10 s ahead of the quarter notes
// (as after an earlier tempo change), so 14 s and 15 s. Live, and frozen while the song plays
// on, in every mode, at every rate.
class BeatGrid : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(BeatGrid, PlacesTheCycleInTheSong)
{
    const ModeCase c = GetParam();
    constexpr double kProjectTimeAhead = 10.0;
    struct Expected
    {
        BeatSpan span;
        double cycleStart;
        double spanQuarterNotes;
    };
    for (const Expected expected :
         {Expected{.span = BeatSpan::kOneBar, .cycleStart = 8.0, .spanQuarterNotes = 4.0},
          Expected{.span = BeatSpan::kHalfBar, .cycleStart = 10.0, .spanQuarterNotes = 2.0}}) {
        SCOPED_TRACE(static_cast<int>(expected.span));
        Session session(c.rate, c.channels);
        session.secondsOffset = kProjectTimeAhead;
        session.join({.frequency = 1000.0});
        session.join({.frequency = 1500.0});
        configure(session.own(), c.mode, TimeBase::kBeats);
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(expected.span));
        session.play(5.3, protocol::kReadoutIntervalSeconds / 2.0);
        const auto expectCycle = [&] {
            EXPECT_NEAR(session.wideField(FrameHeader::kCycleStartOffset), expected.cycleStart,
                        1e-3);
            EXPECT_NEAR(session.field(FrameHeader::kSpanQuarterNotesOffset),
                        expected.spanQuarterNotes, 1e-6);
            EXPECT_NEAR(session.field(FrameHeader::kBarQuarterNotesOffset), 4.0, 1e-6);
            EXPECT_NEAR(session.wideField(FrameHeader::kCycleSecondsOffset),
                        kProjectTimeAhead + (expected.cycleStart / 2.0), 1e-3);
        };
        for (const bool frozen : {false, true}) {
            SCOPED_TRACE(frozen ? "frozen" : "live");
            session.analyzer().setFrozen(frozen);
            session.play(0.01);
            ASSERT_TRUE(session.produce()) << "no frame";
            expectCycle();
        }
        // Frozen while the song plays on past a whole bar, a redrawn frame keeps the freeze's
        // cycle.
        SCOPED_TRACE("frozen, played on");
        session.play(2.5);
        session.analyzer().setColumns(kColumns - 1); // Forces a frame.
        ASSERT_TRUE(session.produce()) << "no frame";
        expectCycle();
    }
}

// Late in a song, the frame still says where its cycle starts to double precision: at a view of a
// few samples the grid and the clock are placed from it, and a float would put a beat line
// several percent of the screen off (deep-view.test.ts). A third of a bar starts on no whole
// quarter note, which a float could hold exactly; quarter note 4800 is about 40 minutes in.
TEST(BeatGridLate, PlacesTheCycleExactly)
{
    constexpr double kRate = 48000.0;
    constexpr double kThird = 4.0 / 3.0;
    Session session(kRate, 2);
    session.quarterNotesOffset = 4800.1;
    session.secondsOffset = 4800.1 / 2.0; // At 120 BPM since the song's start.
    session.join({.frequency = 1000.0});
    configure(session.own(), Display::kSingle, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kThirdBar));
    session.play(2.0, protocol::kReadoutIntervalSeconds / 2.0);
    session.play(0.01);
    ASSERT_TRUE(session.produce()) << "no frame";
    const double cycleStart = session.wideField(FrameHeader::kCycleStartOffset);
    EXPECT_GT(cycleStart, 4800.0);
    EXPECT_NEAR(cycleStart, std::round(cycleStart / kThird) * kThird, 1e-9)
        << "on a third of a bar, exactly";
    EXPECT_NEAR(session.wideField(FrameHeader::kCycleSecondsOffset),
                session.secondsOffset + ((cycleStart - session.quarterNotesOffset) / 2.0), 1e-9);
}

// A sweep's playhead stands on a sample (the newest one placed): the frame says where to the
// sample, so zoomed in to a few samples it is drawn on the join between the passes, where the
// engine stops drawing the wave across it. Four bars, 384,000 samples, on which a float would put
// it hundredths of a sample off; live and frozen.
TEST(BeatPlayhead, StandsOnASample)
{
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.join({.frequency = 1000.0});
    configure(session.own(), Display::kSingle, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kFourBars));
    session.play(9.3, protocol::kReadoutIntervalSeconds / 2.0);
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        session.analyzer().setFrozen(frozen);
        session.play(static_cast<double>(kBlock) / kRate);
        ASSERT_TRUE(session.produce()) << "no frame";
        const double count = session.field(FrameHeader::kVisibleSamplesOffset) /
                             (session.wideField(FrameHeader::kViewEndOffset) -
                              session.wideField(FrameHeader::kViewStartOffset));
        ASSERT_EQ(count, 384000.0);
        const double cursor = session.wideField(FrameHeader::kCursorOffset) * count;
        EXPECT_GT(cursor, 0.0);
        EXPECT_NEAR(cursor, std::round(cursor), 1e-6) << "on a sample";
    }
}

INSTANTIATE_TEST_SUITE_P(EveryRate, BeatGrid, ::testing::ValuesIn(beatCases()), modeTestName);

// A beat span longer than a sweep may hold (16 bars at 120 BPM and 384 kHz) is drawn limited and
// says so, which the page shows as "Span limited", live and frozen; the same span at 48 kHz fits
// and does not. Either way the frame claims nothing else: Compare only in Compare, never frozen
// while live.
class LimitedSweep : public ::testing::TestWithParam<Display>
{
};

TEST_P(LimitedSweep, SaysWhenTheSpanIsLimited)
{
    const Display mode = GetParam();
    const auto sixteenBars = static_cast<std::size_t>(BeatSpan::kSixteenBars);
    ASSERT_GT(beatSpanSamples(kSampleRates.back(), 120.0, sixteenBars), kMaxSweepSamples)
        << "no rate goes over the sweep limit: choose a longer span or a slower tempo";
    for (const double rate : {48000.0, kSampleRates.back()}) {
        SCOPED_TRACE(rateName(rate));
        const bool limited = beatSpanSamples(rate, 120.0, sixteenBars) > kMaxSweepSamples;
        Session session(rate, 2);
        session.join({.frequency = 1000.0});
        session.join({.frequency = 1500.0});
        configure(session.own(), mode, TimeBase::kBeats);
        session.own().settings().set(SettingId::kBeatSpan,
                                     static_cast<double>(BeatSpan::kSixteenBars));
        session.play(1.0, protocol::kReadoutIntervalSeconds / 2.0);
        session.play(0.01);
        const auto frame = session.produce();
        ASSERT_TRUE(frame) << "no frame";
        EXPECT_EQ(frame->has(FrameFlag::kLimited), limited);
        EXPECT_EQ(frame->has(FrameFlag::kCompared), mode == Display::kCompare);
        EXPECT_FALSE(frame->has(FrameFlag::kFrozen));
        EXPECT_TRUE(frame->has(FrameFlag::kSynced));
        EXPECT_LE(frame->visibleSamples, static_cast<float>(kMaxSweepSamples));
        // Still filling its first pass, so only the layout is checked, not the values.
        EXPECT_EQ(tracesIn(*frame), expectedTraces(mode, 2, 2));
        EXPECT_EQ(frame->data.size(), announcedValues(*frame));

        // Frozen, the frame still says whether the span was limited.
        session.analyzer().setFrozen(true);
        session.play(0.01);
        const auto frozen = session.produce();
        ASSERT_TRUE(frozen) << "no frozen frame";
        EXPECT_TRUE(frozen->has(FrameFlag::kFrozen));
        EXPECT_EQ(frozen->has(FrameFlag::kLimited), limited) << "frozen";
    }
}

INSTANTIATE_TEST_SUITE_P(EveryMode, LimitedSweep, ::testing::ValuesIn(everyMode()),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

// --- Beat spans at the freeze hold ----------------------------------------------------------

// The longest beat span whose sweep at `tempo` fits in the capture, which bounds what a freeze in
// Beats mode holds. The freeze keeps a little less, leaving the audio thread a margin to write on
// (frozenLength in Analyzer.cpp); spans grow by a third or more, so at these rates none falls in
// that margin, and the test below would find one that did, its frozen frame not whole.
std::size_t longestBeatSpanHeld(double rate, double tempo)
{
    std::size_t span = 0;
    while (span + 1 < protocol::kBeatSpanCount &&
           beatSpanSamples(rate, tempo, span + 1) <= CaptureRing::capacityFor(rate)) {
        ++span;
    }
    return span;
}

// The lowest and highest rates in every mode, channels alternating.
std::vector<ModeCase> heldBeatCases()
{
    std::vector<ModeCase> cases;
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        for (const Display mode : everyMode()) {
            cases.push_back({.rate = rate,
                             .mode = mode,
                             .channels = rate == kSampleRates.front() ? 2 : 1,
                             .timeBase = TimeBase::kBeats});
        }
    }
    return cases;
}

class HeldBeatSpans : public ::testing::TestWithParam<ModeCase>
{
};

// The longest beat span up to `limit` that divides `span`, so that a frozen sweep of `span` crops
// it (frozenSweep): a span that does not divide it, as two bars do not divide three, is rebuilt
// from the held audio instead.
std::size_t longestDividingSpan(std::size_t span, std::size_t limit)
{
    const double bar = Timeline::barQuarterNotes(MusicalStamp{});
    const double length = beatSpanQuarterNotes(static_cast<BeatSpan>(span), bar);
    for (std::size_t candidate = limit; candidate > 0; --candidate) {
        const double ratio = length / beatSpanQuarterNotes(static_cast<BeatSpan>(candidate), bar);
        if (std::abs(ratio - std::round(ratio)) < 1e-9) {
            return candidate;
        }
    }
    return 0; // 1/8 bar divides every span.
}

constexpr double kHeldTempo = 120.0;

// Two tracks playing `span` past a whole capture and two of its sweeps, still live: each test
// freezes when it is ready.
struct HeldSession
{
    HeldSession(const ModeCase& c, std::size_t span) : session(c.rate, c.channels)
    {
        session.join({.frequency = 1000.0}, kHeldTempo);
        session.join({.frequency = 1500.0}, kHeldTempo);
        configure(session.own(), c.mode, TimeBase::kBeats);
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(span));
        const double sweepSeconds =
            static_cast<double>(beatSpanSamples(c.rate, kHeldTempo, span)) / c.rate;
        // Frames often enough that no sweep misses audio the capture overwrote.
        session.play(captureSeconds(c.rate) + 2.0 * sweepSeconds, captureSeconds(c.rate) / 4.0);
    }

    Session session;
};

// A frozen frame of beat span `span`, chosen while frozen.
std::optional<DecodedFrame> frozenAt(Session& session, std::size_t span)
{
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(span));
    return session.produce();
}

// Live and frozen, the longest span the hold covers is whole. A longer span chosen while frozen is
// rebuilt from the held audio, which covers only part of it: the frame says so.
TEST_P(HeldBeatSpans, DrawsTheLongestHeldSpanWholeFrozen)
{
    const ModeCase c = GetParam();
    const std::size_t fits = longestBeatSpanHeld(c.rate, kHeldTempo);
    const std::size_t longer = fits + 1;
    ASSERT_LT(longer, protocol::kBeatSpanCount) << "every beat span fits the capture";
    HeldSession held(c, fits);
    Session& session = held.session;
    const std::size_t traces = expectedTraces(c.mode, c.channels, 2);

    auto frame = session.produce();
    ASSERT_TRUE(frame) << "no live frame";
    EXPECT_FALSE(frame->has(FrameFlag::kPartial));
    EXPECT_EQ(frame->visibleSamples, static_cast<float>(beatSpanSamples(c.rate, kHeldTempo, fits)));
    {
        SCOPED_TRACE("live");
        expectTraces(*frame, traces);
    }

    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / c.rate);
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frozen frame";
    EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
    EXPECT_FALSE(frame->has(FrameFlag::kPartial));
    {
        SCOPED_TRACE("frozen");
        expectTraces(*frame, traces);
    }

    frame = frozenAt(session, longer);
    ASSERT_TRUE(frame) << "no frame of the longer span";
    EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
    EXPECT_NEAR(frame->visibleSamples,
                static_cast<float>(beatSpanSamples(c.rate, kHeldTempo, longer)), 1.0f);
    EXPECT_TRUE(frame->has(FrameFlag::kPartial)) << "the hold covers only part of the span";
}

// Back at the held span after the longer one, the frame is rebuilt from the hold, which covers it,
// not cropped from the longer span's partly empty sweep: whole, as before the round trip.
TEST_P(HeldBeatSpans, StaysWholeFrozenBackFromALongerSpan)
{
    const ModeCase c = GetParam();
    const std::size_t fits = longestBeatSpanHeld(c.rate, kHeldTempo);
    const std::size_t longer = fits + 1;
    ASSERT_LT(longer, protocol::kBeatSpanCount) << "every beat span fits the capture";
    HeldSession held(c, fits);
    Session& session = held.session;
    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / c.rate);
    ASSERT_TRUE(session.produce()) << "no frozen frame";
    ASSERT_TRUE(frozenAt(session, longer)) << "no frame of the longer span";

    const auto frame = frozenAt(session, fits);
    ASSERT_TRUE(frame) << "no frame back at the held span";
    EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
    EXPECT_EQ(frame->visibleSamples, static_cast<float>(beatSpanSamples(c.rate, kHeldTempo, fits)));
    EXPECT_FALSE(frame->has(FrameFlag::kPartial)) << "the hold covers the span";
    expectTraces(*frame, expectedTraces(c.mode, c.channels, 2));
}

// Frozen after playing the next span up, which the hold does not cover but the live sweep kept,
// the frame is whole: as it is, cropped to the longest held span that divides it, and back.
TEST_P(HeldBeatSpans, StaysWholeFrozenFromALongerSpanAndBack)
{
    const ModeCase c = GetParam();
    const std::size_t longer = longestBeatSpanHeld(c.rate, kHeldTempo) + 1;
    ASSERT_LT(longer, protocol::kBeatSpanCount) << "every beat span fits the capture";
    const std::size_t cropped = longestDividingSpan(longer, longer - 1);
    HeldSession held(c, longer);
    Session& session = held.session;
    const std::size_t traces = expectedTraces(c.mode, c.channels, 2);

    auto frame = session.produce();
    ASSERT_TRUE(frame) << "no live frame";
    EXPECT_FALSE(frame->has(FrameFlag::kPartial));
    {
        SCOPED_TRACE("live");
        expectTraces(*frame, traces);
    }

    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / c.rate);
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frozen frame";
    {
        SCOPED_TRACE("frozen");
        expectTraces(*frame, traces);
    }
    for (const std::size_t span : {cropped, longer}) {
        SCOPED_TRACE(protocol::kBeatSpanNames.at(span));
        frame = frozenAt(session, span);
        ASSERT_TRUE(frame);
        EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
        EXPECT_FALSE(frame->has(FrameFlag::kPartial));
        EXPECT_NEAR(frame->visibleSamples,
                    static_cast<float>(beatSpanSamples(c.rate, kHeldTempo, span)), 1.0f);
        expectTraces(*frame, traces);
    }
}

// Frozen at the held span, a longer one chosen, the held one and the longer one again: the longer
// span is as it was the first time, partly filled and flagged so, the held span's rebuilt part
// gone from it.
TEST_P(HeldBeatSpans, ShowsTheLongerSpanAgainAsItWas)
{
    const ModeCase c = GetParam();
    const std::size_t fits = longestBeatSpanHeld(c.rate, kHeldTempo);
    const std::size_t longer = fits + 1;
    ASSERT_LT(longer, protocol::kBeatSpanCount) << "every beat span fits the capture";
    HeldSession held(c, fits);
    Session& session = held.session;
    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / c.rate);
    ASSERT_TRUE(session.produce()) << "no frozen frame";
    const auto first = frozenAt(session, longer);
    ASSERT_TRUE(first) << "no frame of the longer span";
    ASSERT_TRUE(first->has(FrameFlag::kPartial));
    const auto back = frozenAt(session, fits);
    ASSERT_TRUE(back) << "no frame back at the held span";
    ASSERT_FALSE(back->has(FrameFlag::kPartial));

    const auto again = frozenAt(session, longer);
    ASSERT_TRUE(again) << "no frame of the longer span again";
    EXPECT_TRUE(again->has(FrameFlag::kPartial)) << "the hold covers only part of the span";
    EXPECT_EQ(again->visibleSamples, first->visibleSamples);
    EXPECT_TRUE(sameValues(again->data, first->data)) << "the longer span as it was";
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, HeldBeatSpans, ::testing::ValuesIn(heldBeatCases()),
                         modeTestName);
INSTANTIATE_TEST_SUITE_P(EdgeRates, LiveZoom, ::testing::ValuesIn(heldBeatCases()), modeTestName);

// The host changes the rate with the editor open in Beats mode: the freeze then holds the longest
// beat span the capture sized for the new rate covers, whole when it is rebuilt from the hold (on
// coming back to it from a longer span).
class BeatRateChanges : public ::testing::TestWithParam<RateChange>
{
};

TEST_P(BeatRateChanges, HoldTheLongestSpanTheNewCaptureCovers)
{
    const RateChange& c = GetParam();
    Session session(c.from, 2);
    session.join({.frequency = 1000.0}, kHeldTempo);
    session.join({.frequency = 1500.0}, kHeldTempo);
    configure(session.own(), c.mode, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan,
                                 static_cast<double>(longestBeatSpanHeld(c.from, kHeldTempo)));
    session.play(1.0, protocol::kReadoutIntervalSeconds);
    ASSERT_TRUE(session.produce()) << "no frame before the change";
    session.analyzer().setFrozen(true);
    ASSERT_TRUE(session.produce()) << "no frozen frame before the change";
    session.analyzer().setFrozen(false);

    session.prepare(c.to);
    const std::size_t fits = longestBeatSpanHeld(c.to, kHeldTempo);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(fits));
    const double sweepSeconds = static_cast<double>(beatSpanSamples(c.to, kHeldTempo, fits)) / c.to;
    session.play(captureSeconds(c.to) + 2.0 * sweepSeconds, captureSeconds(c.to) / 4.0);
    const std::size_t traces = expectedTraces(c.mode, 2, 2);
    auto frame = session.produce();
    ASSERT_TRUE(frame) << "no live frame";
    EXPECT_FALSE(frame->has(FrameFlag::kPartial));
    expectTraces(*frame, traces);

    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / c.to);
    ASSERT_TRUE(session.produce()) << "no frozen frame";
    ASSERT_LT(fits + 1, protocol::kBeatSpanCount) << "every beat span fits the capture";
    ASSERT_TRUE(frozenAt(session, fits + 1)) << "no frame of the longer span";
    frame = frozenAt(session, fits);
    ASSERT_TRUE(frame) << "no frame back at the held span";
    EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
    EXPECT_FALSE(frame->has(FrameFlag::kPartial)) << "the new hold covers the span";
    EXPECT_EQ(frame->visibleSamples, static_cast<float>(beatSpanSamples(c.to, kHeldTempo, fits)));
    expectTraces(*frame, traces);
}

INSTANTIATE_TEST_SUITE_P(Changes, BeatRateChanges, ::testing::ValuesIn(rateChanges()),
                         [](const ::testing::TestParamInfo<RateChange>& info) {
                             return nameOf(info.param);
                         });

// Frozen before a long sweep is whole, in every mode: a shorter span is cropped from it, still
// filling in, and the sweep is as it was drawn back at its span, with all the audio it had beyond
// what the freeze holds. At 120 BPM, 16 bars is 32 s, of which 25 s was drawn, more than the
// 19.1 s held at 48 kHz.
class FrozenSweepFillingIn : public ::testing::TestWithParam<Display>
{
};

TEST_P(FrozenSweepFillingIn, IsAsItWasBackFromAShorterSpan)
{
    constexpr double kRate = 48000.0;
    const Display mode = GetParam();
    Session session(kRate, 2);
    session.join({.frequency = 1000.0}, kHeldTempo);
    session.join({.frequency = 1500.0}, kHeldTempo);
    configure(session.own(), mode, TimeBase::kBeats);
    const auto longSpan = static_cast<std::size_t>(protocol::BeatSpan::kSixteenBars);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(longSpan));
    session.play(25.0, 2.0);
    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / kRate);
    const auto drawn = session.produce();
    ASSERT_TRUE(drawn) << "no frozen frame";
    ASSERT_TRUE(drawn->has(FrameFlag::kPartial)) << "the sweep is still filling in";

    const auto shorter = frozenAt(session, static_cast<std::size_t>(protocol::BeatSpan::kFourBars));
    ASSERT_TRUE(shorter) << "no frame of the shorter span";
    EXPECT_FALSE(shorter->has(FrameFlag::kPartial)) << "the hold covers four bars";
    expectTraces(*shorter, expectedTraces(mode, 2, 2));

    const auto back = frozenAt(session, longSpan);
    ASSERT_TRUE(back) << "no frame back at the sweep's span";
    EXPECT_TRUE(back->has(FrameFlag::kPartial));
    EXPECT_TRUE(sameValues(back->data, drawn->data)) << "the sweep as it was drawn";
}

INSTANTIATE_TEST_SUITE_P(EveryMode, FrozenSweepFillingIn, ::testing::ValuesIn(everyDisplay()),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

// Frozen partway through a sweep's first pass, with audio that arrived after the last frame: the
// frozen sweep draws that audio too, so its playhead stands at the newest drawn sample, with no
// gap behind it. In every mode, at the lowest and highest rates.
class FrozenPlayhead : public ::testing::TestWithParam<ModeCase>
{
};

// Plays two tracks for `seconds` of a two-bar sweep (4 s), with a frame every 100 ms, then 80 ms
// more after the last frame.
void playSweep(Session& session, const ModeCase& c, double seconds)
{
    session.join({.kind = Signal::Kind::kNoise, .seed = 1}); // Differs from pass to pass.
    session.join({.kind = Signal::Kind::kNoise, .seed = 2});
    configure(session.own(), c.mode, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kTwoBars));
    session.play(seconds, 0.1);
    session.play(0.08);
}

TEST_P(FrozenPlayhead, StandsAtTheNewestDrawnSample)
{
    const ModeCase c = GetParam();
    Session session(c.rate, c.channels);
    playSweep(session, c, 1.0); // Into the first pass.
    session.analyzer().setFrozen(true);
    const auto frame = session.produce();
    ASSERT_TRUE(frame) << "no frozen frame";
    ASSERT_TRUE(frame->has(FrameFlag::kFrozen));
    ASSERT_EQ(frame->mode, FrameMode::kMinMax);
    const auto playhead = static_cast<std::size_t>(frame->cursor * kColumns);
    ASSERT_GT(playhead, 20U);
    // The columns just behind the playhead, in every trace: drawn, not holes.
    const std::size_t traces = tracesIn(*frame);
    for (std::size_t index = 0; index < traces; ++index) {
        const std::span<const float> values = trace(*frame, index);
        for (std::size_t column = playhead - 20; column < playhead - 1; ++column) {
            EXPECT_GE(std::abs(values[column * 2 + 1]), kHole)
                << "trace " << index << ", column " << column << " of playhead " << playhead;
        }
    }
}

// A shorter span chosen in the same step as the freeze is shown as when chosen just after it:
// cropped from the whole sweep drawn up to the freeze, on the span it was drawn with.
TEST_P(FrozenPlayhead, IsTheSameWithASpanChosenAsItFreezes)
{
    const ModeCase c = GetParam();
    const auto oneBar = static_cast<double>(BeatSpan::kOneBar);
    Session together(c.rate, c.channels);
    playSweep(together, c, 5.0); // Past a whole pass.
    together.own().settings().set(SettingId::kBeatSpan, oneBar);
    together.analyzer().setFrozen(true);
    const auto atOnce = together.produce();
    ASSERT_TRUE(atOnce) << "no frame frozen with the span";

    Session after(c.rate, c.channels);
    playSweep(after, c, 5.0);
    after.analyzer().setFrozen(true);
    ASSERT_TRUE(after.produce()) << "no frozen frame";
    after.own().settings().set(SettingId::kBeatSpan, oneBar);
    const auto later = after.produce();
    ASSERT_TRUE(later) << "no frame of the span chosen after";

    EXPECT_EQ(atOnce->cursor, later->cursor);
    EXPECT_EQ(atOnce->visibleSamples, later->visibleSamples);
    EXPECT_TRUE(sameValues(atOnce->data, later->data)) << "the same frame";
}

// A shorter span drawn between frames, as the sweep advances while the page is not ready for one,
// then frozen: the freeze catches up on the span it was drawn with, as when a frame drew it.
TEST_P(FrozenPlayhead, IsTheSameWithASpanDrawnBetweenFrames)
{
    const ModeCase c = GetParam();
    const auto oneBar = static_cast<double>(BeatSpan::kOneBar);
    Session between(c.rate, c.channels);
    playSweep(between, c, 5.0); // Past a whole pass.
    between.own().settings().set(SettingId::kBeatSpan, oneBar);
    between.analyzer().advance();
    between.analyzer().setFrozen(true);
    const auto advanced = between.produce();
    ASSERT_TRUE(advanced) << "no frame frozen after the sweep advanced";

    Session framed(c.rate, c.channels);
    playSweep(framed, c, 5.0);
    framed.own().settings().set(SettingId::kBeatSpan, oneBar);
    ASSERT_TRUE(framed.produce()) << "no live frame of the span";
    framed.analyzer().setFrozen(true);
    const auto produced = framed.produce();
    ASSERT_TRUE(produced) << "no frame frozen after a frame";

    EXPECT_EQ(advanced->cursor, produced->cursor);
    EXPECT_EQ(advanced->visibleSamples, produced->visibleSamples);
    EXPECT_TRUE(sameValues(advanced->data, produced->data)) << "the same frame";
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, FrozenPlayhead, ::testing::ValuesIn(heldBeatCases()),
                         modeTestName);

// Frozen with a lane still filling in, beside whole ones: a shorter span longer than the hold is
// cropped from the whole lanes, which keep all they drew, and rebuilt from the held audio only for
// the lane that is filling in. At 90 BPM, 16 bars is 42.7 s and 8 bars 21.3 s, longer than the
// 19.1 s held at 48 kHz.
class MixedFrozenLanes : public ::testing::TestWithParam<Display>
{
};

TEST_P(MixedFrozenLanes, KeepsWholeLanesWholeBesideALaneFillingIn)
{
    constexpr double kRate = 48000.0;
    constexpr double kTempo = 90.0;
    const Display mode = GetParam();
    Session session(kRate, 2);
    session.join({.frequency = 1000.0}, kTempo);
    session.join({.frequency = 1500.0}, kTempo);
    configure(session.own(), mode, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan,
                                 static_cast<double>(protocol::BeatSpan::kSixteenBars));
    session.play(30.0, 2.0);
    session.join({.frequency = 2000.0}, kTempo); // Joins partway through the 16-bar cycle.
    session.play(20.0, 2.0);
    auto frame = session.produce();
    ASSERT_TRUE(frame) << "no live frame";
    EXPECT_TRUE(frame->has(FrameFlag::kPartial)) << "the third lane is still filling in";

    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / kRate);
    ASSERT_TRUE(session.produce()) << "no frozen frame";
    frame = frozenAt(session, static_cast<std::size_t>(protocol::BeatSpan::kEightBars));
    ASSERT_TRUE(frame) << "no frame of the shorter span";
    EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
    EXPECT_NEAR(frame->visibleSamples,
                static_cast<float>(beatSpanSamples(
                    kRate, kTempo, static_cast<std::size_t>(protocol::BeatSpan::kEightBars))),
                1.0f);
    ASSERT_EQ(tracesIn(*frame), expectedTraces(mode, 2, 3));
    // The whole lanes, and in Compare the sum of this track and the first other one.
    std::vector<std::size_t> whole{0, 1};
    if (mode == Display::kCompare) {
        whole.push_back(3);
    }
    for (const std::size_t index : whole) {
        const std::span<const float> values = trace(*frame, index);
        const auto hole =
            std::ranges::find_if(values, [](float value) { return !(std::abs(value) >= kHole); });
        EXPECT_TRUE(hole == values.end()) << "trace " << index << " has a hole at "
                                          << (hole - values.begin()) << " of " << values.size();
    }

    // Back at the frozen span, every lane is as it was drawn.
    frame = frozenAt(session, static_cast<std::size_t>(protocol::BeatSpan::kSixteenBars));
    ASSERT_TRUE(frame) << "no frame back at the frozen span";
    for (const std::size_t index : whole) {
        const std::span<const float> values = trace(*frame, index);
        EXPECT_TRUE(
            std::ranges::none_of(values, [](float value) { return !(std::abs(value) >= kHole); }))
            << "trace " << index << " lost audio";
    }
}

INSTANTIATE_TEST_SUITE_P(MultiTrack, MixedFrozenLanes,
                         ::testing::Values(Display::kMulti, Display::kCompare),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return info.param == Display::kMulti ? std::string("Multi")
                                                                  : std::string("Compare");
                         });

// --- Lane lengths around the sweep tolerance ------------------------------------------------

// The most two sweep lengths may differ, the shorter being `length`, for a lane to take the own
// track's length: what sweepLengthsSimilar allows, and always one sample (sharedLength in
// Analyzer.cpp).
std::uint64_t laneTolerance(std::uint64_t length)
{
    std::uint64_t apart = 1;
    while (sweepLengthsSimilar(length, length + apart + 1)) {
        ++apart;
    }
    return apart;
}

// The shortest length whose tolerance is `apart` samples.
std::uint64_t toleranceStep(std::uint64_t apart)
{
    std::uint64_t length = 1;
    while (laneTolerance(length) < apart) {
        ++length;
    }
    return length;
}

// The shortest length that sweepLengthsSimilar lets a sweep one sample longer match.
std::uint64_t firstSimilarLength()
{
    std::uint64_t length = 1;
    while (!sweepLengthsSimilar(length, length + 1)) {
        ++length;
    }
    return length;
}

struct LengthCase
{
    double rate = 48000.0;
    std::uint64_t length = 0; // The own track's sweep, in samples; 0 for 1/8 bar at 120 BPM.
    bool within = true;       // The other track's sweep: at the tolerance, or one sample beyond.
    Display mode = Display::kMulti;
};

// The shortest beat span, 1/8 bar, in quarter notes of the bar Session plays.
double eighthBarQuarterNotes()
{
    return beatSpanQuarterNotes(BeatSpan::kEighthBar, Timeline::barQuarterNotes(MusicalStamp{}));
}

std::uint64_t ownLength(const LengthCase& c)
{
    return c.length > 0 ? c.length : beatSamples(c.rate, 120.0, eighthBarQuarterNotes());
}

std::string nameOf(const LengthCase& c)
{
    return rateName(c.rate) + "_" + std::to_string(ownLength(c)) + "Samples_" +
           (c.within ? "AtTolerance" : "BeyondTolerance") + "_" + modeName(c.mode);
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const LengthCase& c, std::ostream* out)
{
    *out << nameOf(c);
}

std::string lengthTestName(const ::testing::TestParamInfo<LengthCase>& info)
{
    return nameOf(info.param);
}

// Every rate at 1/8 bar, the lanes' modes; and at 48 kHz, lengths on both sides of each
// edge sweepLengthsSimilar has, each within and beyond the tolerance. Below the first (1023
// samples) it allows nothing and only the one sample lets a lane through; from it, it allows that
// sample too; from the next ones (2046, 3069) it allows two and three samples.
std::vector<LengthCase> lengthCases()
{
    std::vector<LengthCase> cases;
    for (const double rate : kSampleRates) {
        for (const bool within : {true, false}) {
            for (const Display mode : {Display::kMulti, Display::kCompare}) {
                cases.push_back({.rate = rate, .within = within, .mode = mode});
            }
        }
    }
    for (const std::uint64_t edge : {firstSimilarLength(), toleranceStep(2), toleranceStep(3)}) {
        for (const std::uint64_t length : {edge - 1, edge}) {
            for (const bool within : {true, false}) {
                cases.push_back({.rate = 48000.0, .length = length, .within = within});
            }
        }
    }
    return cases;
}

class LaneLengths : public ::testing::TestWithParam<LengthCase>
{
};

// Another track's sweep a little longer than this one's (its tempo a little slower) is drawn on
// this track's length as long as it is within the tolerance, and silent beyond it, live and
// frozen.
TEST_P(LaneLengths, DrawsALaneWithinTheToleranceOnly)
{
    const LengthCase c = GetParam();
    const double rate = c.rate;
    const std::uint64_t length = ownLength(c);
    const std::uint64_t apart = laneTolerance(length) + (c.within ? 0 : 1);
    // The tempo at which 1/8 bar lasts `samples`: the short lengths come at fast tempi in 4/4,
    // as they would in a short bar at a slower one.
    const double eighthBar = eighthBarQuarterNotes();
    const auto tempoFor = [&](std::uint64_t samples) {
        return eighthBar * 60.0 * rate / static_cast<double>(samples);
    };
    ASSERT_EQ(beatSamples(rate, tempoFor(length), eighthBar), length);
    ASSERT_EQ(beatSamples(rate, tempoFor(length + apart), eighthBar), length + apart);

    Session session(rate, 2);
    session.join({.frequency = 1000.0}, tempoFor(length));
    session.join({.frequency = 1500.0}, tempoFor(length + apart));
    configure(session.own(), c.mode, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kEighthBar));
    const std::size_t traces = expectedTraces(c.mode, 2, 2);
    const std::optional<std::size_t> silent =
        c.within ? std::nullopt : std::optional<std::size_t>(1);

    session.play(static_cast<double>(6 * length) / rate, static_cast<double>(length) / rate);
    session.play(static_cast<double>(kBlock) / rate);
    auto frame = session.produce();
    ASSERT_TRUE(frame) << "no live frame";
    EXPECT_EQ(frame->visibleSamples, static_cast<float>(length));
    {
        SCOPED_TRACE("live");
        expectTraces(*frame, traces, silent);
    }

    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / rate);
    session.analyzer().setColumns(kColumns - 1); // Forces a frame.
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frozen frame";
    EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
    SCOPED_TRACE("frozen");
    expectTraces(*frame, traces, silent);
}

INSTANTIATE_TEST_SUITE_P(EveryRate, LaneLengths, ::testing::ValuesIn(lengthCases()),
                         lengthTestName);

// --- Compare's match reach ------------------------------------------------------------------

struct ReachCase
{
    double rate = 48000.0;
    int beyond = 0; // Samples beyond the reach; the other track lags by reach + beyond.
    bool leads = false;
};

std::string nameOf(const ReachCase& c)
{
    return rateName(c.rate) + "_" + (c.leads ? "Leads" : "Lags") +
           (c.beyond > 0 ? "BeyondReach" : "AtReach");
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const ReachCase& c, std::ostream* out)
{
    *out << nameOf(c);
}

std::string reachTestName(const ::testing::TestParamInfo<ReachCase>& info)
{
    return nameOf(info.param);
}

std::vector<ReachCase> reachCases()
{
    std::vector<ReachCase> cases;
    for (const double rate : kSampleRates) {
        for (const bool leads : {false, true}) {
            for (const int beyond : {0, 1}) {
                cases.push_back({.rate = rate, .beyond = beyond, .leads = leads});
            }
        }
    }
    return cases;
}

class MatchReach : public ::testing::TestWithParam<ReachCase>
{
};

// Compare finds a track up to the match reach late or early, to the sample, at any rate; a track
// a sample beyond the reach either way is reported neither at its offset nor at the reach's edge.
TEST_P(MatchReach, FindsAnOffsetUpToTheReachOnly)
{
    const ReachCase c = GetParam();
    const double rate = c.rate;
    const auto reach = static_cast<std::int64_t>(std::llround(protocol::kMatchReachSeconds * rate));
    const std::int64_t delay = (reach + c.beyond) * (c.leads ? -1 : 1);
    Session session(rate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 7});
    session.join({.kind = Signal::Kind::kNoise, .seed = 7, .delay = delay});
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    // A span of ten times the reach: the match is searched over the visible span, so even a track
    // offset by the whole reach overlaps nine tenths of it.
    ASSERT_TRUE(
        session.own().settings().set(SettingId::kFreeSpan, 10.0 * protocol::kMatchReachSeconds));
    session.play(0.25);
    const auto frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->has(FrameFlag::kCompared));
    ASSERT_EQ(frame->laneCount, 3);
    const std::size_t at = std::size_t{frame->laneCount} * traceLength(*frame) +
                           std::size_t{1} * protocol::kMatchValues;
    ASSERT_LT(at + 2, frame->data.size());
    const float correlation = frame->data.at(at);
    const float offset = frame->data.at(at + 1);
    const float polarity = frame->data.at(at + 2);
    EXPECT_TRUE(std::isfinite(correlation)) << "both tracks sound: a correlation is measured";
    if (c.beyond == 0) {
        EXPECT_EQ(offset, static_cast<float>(delay));
        EXPECT_EQ(polarity, 1.0f);
    } else if (polarity == 0.0f) {
        EXPECT_TRUE(std::isnan(offset)) << "no match: no offset";
    } else {
        // Not clipped to the edge of the reach, as a search that clamped a farther match would.
        EXPECT_LT(std::abs(offset), static_cast<float>(reach));
    }
}

INSTANTIATE_TEST_SUITE_P(EveryRate, MatchReach, ::testing::ValuesIn(reachCases()), reachTestName);

// --- Readouts at the analysis gate, and no value as NaN -------------------------------------

std::vector<ModeCase> timeBaseCases(Display mode)
{
    std::vector<ModeCase> cases;
    for (std::size_t rate = 0; rate < kSampleRates.size(); ++rate) {
        for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
            cases.push_back({.rate = kSampleRates.at(rate),
                             .mode = mode,
                             .channels = (rate + timeBase) % 2 == 0 ? 2 : 1,
                             .timeBase = static_cast<TimeBase>(timeBase)});
        }
    }
    return cases;
}

// Longer than any span the readout cases below show (a sweep of one beat, a 50 ms triggered window,
// or Free's shortest, 100 ms), so that span holds only what was played.
constexpr double kReadoutPlaySeconds = 1.0;

// Plays enough for the readouts of `timeBase`, producing frames as a page does, and returns the
// last frame.
std::optional<DecodedFrame> playForReadouts(Session& session, TimeBase timeBase,
                                            unsigned views = 0b0011)
{
    configure(session.own(), displayOf(session.own().settings().get()), timeBase);
    session.own().settings().set(SettingId::kChannelViews, static_cast<double>(views));
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kQuarterBar));
    session.own().settings().set(SettingId::kTriggerSpan, 0.05);
    session.own().settings().set(SettingId::kFreeSpan, protocol::kShortestFreeSpanSeconds);
    session.play(kReadoutPlaySeconds + 0.1, protocol::kReadoutIntervalSeconds);
    session.play(protocol::kReadoutIntervalSeconds);
    return session.produce();
}

// Live in Beats, a view hidden for a while and shown again draws what is in the sweep now, not the
// columns it had when it was hidden: Side, hidden for a beat and shown again, equals what a new
// analyzer draws. At every rate, stereo.
TEST(ChannelViewsInBeats, ShowAHiddenViewAsItIsNow)
{
    for (const double rate : kSampleRates) {
        SCOPED_TRACE(rateName(rate));
        Session session(rate, 2);
        session.join({.kind = Signal::Kind::kNoise, .seed = 7});
        configure(session.own(), Display::kSingle, TimeBase::kBeats);
        session.own().settings().set(SettingId::kBeatSpan,
                                     static_cast<double>(BeatSpan::kQuarterBar));
        const auto side = 1U << static_cast<unsigned>(protocol::ChannelView::kSide);
        session.own().settings().set(SettingId::kChannelViews, static_cast<double>(0b0011U | side));
        session.play(1.0, protocol::kReadoutIntervalSeconds / 2.0);
        session.own().settings().set(SettingId::kChannelViews, 0b0011);
        session.play(0.5, protocol::kReadoutIntervalSeconds / 2.0); // A beat.
        session.own().settings().set(SettingId::kChannelViews, static_cast<double>(0b0011U | side));
        session.play(0.01);
        const auto frame = session.produce();
        ASSERT_TRUE(frame) << "no frame";
        Analyzer fresh(session.own(), session.registry);
        fresh.setColumns(kColumns);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(fresh.produce(bytes));
        const DecodedFrame expected = decode(bytes);
        ASSERT_EQ(tracesIn(*frame), 3U);
        EXPECT_TRUE(std::ranges::equal(trace(*frame, 2), trace(expected, 2))) << "Side";
    }
}

// Where the readouts are measured: a time base, a readout source drawn or not, a rate and a track.
struct ReadoutChannelCase
{
    double rate = 0.0;
    int channels = 2;
    TimeBase timeBase = TimeBase::kTrigger;
    protocol::ChannelView source = protocol::ChannelView::kLeft;
    bool sourceDrawn = true;
};

std::string nameOf(const ReadoutChannelCase& c)
{
    return rateName(c.rate) + "_" + channelsName(c.channels) + "_" + timeBaseName(c.timeBase) +
           "_" + capitalised(protocol::kChannelViewNames.at(static_cast<std::size_t>(c.source))) +
           (c.sourceDrawn ? "Drawn" : "Hidden");
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const ReadoutChannelCase& c, std::ostream* out)
{
    *out << nameOf(c);
}

constexpr unsigned viewBit(protocol::ChannelView view)
{
    return 1U << static_cast<unsigned>(view);
}

// The views drawn for a case: drawn, the source and L (or, for L, Side), so the source is not
// simply the first view drawn; hidden, every view but the source.
unsigned drawnViews(const ReadoutChannelCase& c)
{
    const unsigned all = (1U << protocol::kChannelViewCount) - 1U;
    const unsigned other = c.source == protocol::ChannelView::kLeft
                               ? viewBit(protocol::ChannelView::kSide)
                               : viewBit(protocol::ChannelView::kLeft);
    return c.sourceDrawn ? viewBit(c.source) | other : all & ~viewBit(c.source);
}

// The level of each view of a track whose right channel is half its left, against the left's.
double levelOf(protocol::ChannelView view)
{
    switch (view) {
    case protocol::ChannelView::kLeft:
        return 1.0;
    case protocol::ChannelView::kRight:
        return 0.5;
    case protocol::ChannelView::kMid:
        return 0.75;
    case protocol::ChannelView::kSide:
        return 0.25;
    }
    return 0.0;
}

// In Single display mode the readouts measure the readout source when it is drawn, and the first
// view drawn when it is not, whatever the trigger reads: a track whose right channel is half its
// left reads at that view's level, and the frame names the view. A mono track draws and measures
// its one channel, whatever the source. Trigger and Free windows and Beats sweeps, live and
// frozen, at the lowest and highest rates.
class ReadoutChannel : public ::testing::TestWithParam<ReadoutChannelCase>
{
};

TEST_P(ReadoutChannel, IsTheSourceWhenDrawn)
{
    const ReadoutChannelCase c = GetParam();
    constexpr double kAmplitude = 0.5;
    Session session(c.rate, c.channels);
    session.join({.frequency = 1000.0, .amplitude = kAmplitude, .offset = 0.0});
    const auto first = static_cast<protocol::ChannelView>(std::countr_zero(drawnViews(c)));
    const protocol::ChannelView expectedView =
        c.channels == 1 ? protocol::ChannelView::kLeft : (c.sourceDrawn ? c.source : first);
    // The trigger on a drawn view other than the one measured, where there is one: the trigger
    // reads only views drawn, and the readouts follow their own source whatever it reads.
    const unsigned others = drawnViews(c) & ~(1U << static_cast<unsigned>(expectedView));
    const auto triggerSource =
        others != 0 ? static_cast<protocol::ChannelView>(std::countr_zero(others)) : expectedView;
    session.own().settings().set(SettingId::kReadoutSource, static_cast<double>(c.source));
    session.own().settings().set(SettingId::kTriggerSource, static_cast<double>(triggerSource));
    configure(session.own(), Display::kSingle, c.timeBase);
    session.own().settings().set(SettingId::kChannelViews, static_cast<double>(drawnViews(c)));
    std::optional<DecodedFrame> frame = playForReadouts(session, c.timeBase, drawnViews(c));
    ASSERT_TRUE(frame) << "no frame";
    ASSERT_EQ(frame->has(protocol::FrameFlag::kFreeRunning), c.timeBase == TimeBase::kFree);
    if (c.channels == 2) {
        ASSERT_EQ(frame->triggerView, triggerSource) << "the trigger reads another view";
    }
    const double expected = (c.channels == 1 ? 1.0 : levelOf(expectedView)) * kAmplitude;
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        if (frozen) {
            session.analyzer().setFrozen(true);
            session.play(static_cast<double>(kBlock) / c.rate);
            frame = session.produce();
        }
        ASSERT_TRUE(frame) << "no frame";
        EXPECT_EQ(frame->readoutView, expectedView);
        EXPECT_NEAR(frame->peak, expected, 0.01 * kAmplitude);
    }
}

// Every source, drawn and hidden, in each time base, stereo at the edge rates; and a mono track
// with each source, where only L exists.
std::vector<ReadoutChannelCase> readoutChannelCases()
{
    std::vector<ReadoutChannelCase> cases;
    for (std::size_t source = 0; source < protocol::kChannelViewCount; ++source) {
        const auto view = static_cast<protocol::ChannelView>(source);
        for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
            const auto base = static_cast<TimeBase>(timeBase);
            for (const bool drawn : {true, false}) {
                for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
                    cases.push_back({.rate = rate,
                                     .channels = 2,
                                     .timeBase = base,
                                     .source = view,
                                     .sourceDrawn = drawn});
                }
            }
            cases.push_back(
                {.rate = kSampleRates.front(), .channels = 1, .timeBase = base, .source = view});
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, ReadoutChannel, ::testing::ValuesIn(readoutChannelCases()),
                         [](const ::testing::TestParamInfo<ReadoutChannelCase>& info) {
                             return nameOf(info.param);
                         });

// A stereo track measuring R turns mono, as a host may change a track's channels while playing:
// the next frame measures and names L, the one channel drawn, at once, not the R measured before
// until the readouts are next due. In each time base, at the lowest and highest rates.
class ReadoutChannelCount : public ::testing::TestWithParam<ReadoutChannelCase>
{
};

TEST_P(ReadoutChannelCount, FollowsATrackTurnedMono)
{
    const ReadoutChannelCase c = GetParam();
    constexpr double kAmplitude = 0.5;
    Session session(c.rate, 2);
    session.join({.frequency = 1000.0, .amplitude = kAmplitude, .offset = 0.0});
    session.own().settings().set(SettingId::kReadoutSource,
                                 static_cast<double>(protocol::ChannelView::kRight));
    configure(session.own(), Display::kSingle, c.timeBase);
    auto frame = playForReadouts(session, c.timeBase);
    ASSERT_TRUE(frame) << "no frame";
    ASSERT_EQ(frame->readoutView, protocol::ChannelView::kRight);
    session.own().setInputChannels(1);
    session.play(static_cast<double>(kBlock) / c.rate);
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frame";
    EXPECT_EQ(frame->readoutView, protocol::ChannelView::kLeft);
}

std::vector<ReadoutChannelCase> channelCountCases()
{
    std::vector<ReadoutChannelCase> cases;
    for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
        for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
            cases.push_back({.rate = rate, .timeBase = static_cast<TimeBase>(timeBase)});
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, ReadoutChannelCount, ::testing::ValuesIn(channelCountCases()),
                         [](const ::testing::TestParamInfo<ReadoutChannelCase>& info) {
                             return rateName(info.param.rate) + "_" +
                                    timeBaseName(info.param.timeBase);
                         });

// Where the trigger reads: a trigger source drawn or not, a rate and a track.
struct TriggerChannelCase
{
    double rate = 0.0;
    int channels = 2;
    protocol::ChannelView source = protocol::ChannelView::kLeft;
    unsigned drawn = 0b1111;
};

std::string nameOf(const TriggerChannelCase& c)
{
    return rateName(c.rate) + "_" + channelsName(c.channels) + "_" +
           capitalised(protocol::kChannelViewNames.at(static_cast<std::size_t>(c.source))) + "_" +
           std::to_string(c.drawn);
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const TriggerChannelCase& c, std::ostream* out)
{
    *out << nameOf(c);
}

// The value of trace `view` (drawn in `frame`, raw) at the trigger point, a tenth into the span,
// between the samples either side of it.
double atTriggerPoint(const DecodedFrame& points, protocol::ChannelView view)
{
    const DecodedFrame frame = samplesOnly(points); // Between the samples, as a line.
    const auto below = (1U << static_cast<unsigned>(view)) - 1U;
    const auto values = trace(frame, static_cast<std::size_t>(std::popcount(frame.views & below)));
    const double at =
        protocol::kDefaultTriggerPosition * frame.visibleSamples + frame.startFraction;
    const auto index = static_cast<std::size_t>(std::floor(at));
    const double part = at - static_cast<double>(index);
    return values[index] + part * (values[index + 1] - values[index]);
}

// In Single display mode the trigger reads the trigger source when it is drawn, and the first view
// drawn when it is not, whatever the readouts measure: with R a quarter cycle ahead of L, the four
// views rise through the level at four places, and the window places the one read at the trigger
// point, rising through 0, which the frame names. A mono track triggers on its one channel. Live,
// and frozen, where the window stays as placed. At the lowest and highest rates.
class TriggerChannel : public ::testing::TestWithParam<TriggerChannelCase>
{
};

TEST_P(TriggerChannel, IsTheSourceWhenDrawn)
{
    const TriggerChannelCase c = GetParam();
    constexpr double kAmplitude = 0.5;
    Session session(c.rate, c.channels);
    session.join({.frequency = 1000.0,
                  .amplitude = kAmplitude,
                  .offset = 0.0,
                  .rightPhase = std::numbers::pi / 2.0});
    configure(session.own(), Display::kSingle, TimeBase::kTrigger);
    session.own().settings().set(SettingId::kChannelViews, static_cast<double>(c.drawn));
    session.own().settings().set(SettingId::kTriggerSource, static_cast<double>(c.source));
    session.own().settings().set(SettingId::kReadoutSource,
                                 static_cast<double>(protocol::ChannelView::kLeft));
    session.own().settings().set(SettingId::kTriggerSpan, 0.005);
    session.analyzer().setColumns(4096); // Raw samples at every rate.
    session.play(0.2, 0.01);
    // The source if drawn, else the first view drawn; L alone is drawn on a mono track.
    const unsigned drawn = c.channels == 1 ? 1U : c.drawn;
    const protocol::ChannelView expected =
        (drawn & (1U << static_cast<unsigned>(c.source))) != 0
            ? c.source
            : static_cast<protocol::ChannelView>(std::countr_zero(drawn));
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        if (frozen) {
            session.analyzer().setFrozen(true);
        }
        session.play(static_cast<double>(kBlock) / c.rate);
        const auto frame = session.produce();
        ASSERT_TRUE(frame) << "no frame";
        ASSERT_EQ(frame->mode, protocol::FrameMode::kRaw);
        ASSERT_TRUE(frame->has(protocol::FrameFlag::kTriggered));
        EXPECT_EQ(frame->triggerView, expected);
        // A sample's step is at most 2 pi f A / rate, a seventh of the amplitude at 44.1 kHz; the
        // views' crossings lie an eighth of a cycle apart or more, half the amplitude away.
        EXPECT_NEAR(atTriggerPoint(*frame, expected), 0.0, 0.2 * kAmplitude)
            << "placed by the view the trigger reads";
    }
}

// Every source, drawn and hidden (the first view drawn other than the source), stereo at the
// edge rates; the default (the Mid, with L and R drawn); a mono track with each source.
std::vector<TriggerChannelCase> triggerChannelCases()
{
    std::vector<TriggerChannelCase> cases;
    for (std::size_t source = 0; source < protocol::kChannelViewCount; ++source) {
        const auto view = static_cast<protocol::ChannelView>(source);
        const unsigned bit = 1U << source;
        for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
            // Drawn with a view before it where there is one, so the source is not merely the
            // first.
            cases.push_back(
                {.rate = rate, .source = view, .drawn = bit | (bit == 1U ? 0b1000U : 1U)});
            cases.push_back({.rate = rate, .source = view, .drawn = 0b1111U & ~bit});
        }
        cases.push_back(
            {.rate = kSampleRates.front(), .channels = 1, .source = view, .drawn = 0b1111});
    }
    cases.push_back(
        {.rate = kSampleRates.front(), .source = protocol::ChannelView::kMid, .drawn = 0b0011});
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, TriggerChannel, ::testing::ValuesIn(triggerChannelCases()),
                         [](const ::testing::TestParamInfo<TriggerChannelCase>& info) {
                             return nameOf(info.param);
                         });

// A time base, a rate, and whether the readouts are switched back while frozen.
struct SwitchBackCase
{
    double rate = 0.0;
    TimeBase timeBase = TimeBase::kTrigger;
    bool frozen = false;
};

std::string nameOf(const SwitchBackCase& c)
{
    return rateName(c.rate) + "_" + timeBaseName(c.timeBase) + (c.frozen ? "_Frozen" : "_Live");
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const SwitchBackCase& c, std::ostream* out)
{
    *out << nameOf(c);
}

// Readouts on L, with L and M drawn. L stops being measured, by hiding it or by choosing M, while
// the track gets 12 dB quieter over the whole span; frozen, the freeze comes before L is measured
// again. Measured again, L reads the quieter audio, as a new analyzer does, not what it read
// before. In each time base, at the lowest and highest rates.
class ReadoutSwitch : public ::testing::TestWithParam<SwitchBackCase>
{
};

TEST_P(ReadoutSwitch, MeasuresTheAudioNowInTheSpan)
{
    const SwitchBackCase c = GetParam();
    constexpr double kLoud = 0.5;
    constexpr double kQuiet = kLoud / 4.0;
    constexpr unsigned kLeftAndMid =
        viewBit(protocol::ChannelView::kLeft) | viewBit(protocol::ChannelView::kMid);
    for (const bool byViews : {true, false}) {
        SCOPED_TRACE(byViews ? "L hidden" : "M chosen");
        Session session(c.rate, 2);
        session.join({.frequency = 1000.0, .amplitude = kLoud, .offset = 0.0});
        session.own().settings().set(SettingId::kReadoutSource,
                                     static_cast<double>(protocol::ChannelView::kLeft));
        configure(session.own(), Display::kSingle, c.timeBase);
        auto frame = playForReadouts(session, c.timeBase, kLeftAndMid);
        ASSERT_TRUE(frame) << "no frame";
        ASSERT_NEAR(frame->peak, kLoud, 0.01 * kLoud) << "L measured";

        if (byViews) {
            session.own().settings().set(SettingId::kChannelViews,
                                         viewBit(protocol::ChannelView::kMid));
        } else {
            session.own().settings().set(SettingId::kReadoutSource,
                                         static_cast<double>(protocol::ChannelView::kMid));
        }
        session.setSignal(0, {.frequency = 1000.0, .amplitude = kQuiet, .offset = 0.0});
        session.play(kReadoutPlaySeconds + 0.6, protocol::kReadoutIntervalSeconds);
        if (c.frozen) {
            session.analyzer().setFrozen(true);
            session.play(static_cast<double>(kBlock) / c.rate);
            ASSERT_TRUE(session.produce()) << "no frame";
        }
        session.own().settings().set(SettingId::kChannelViews, kLeftAndMid);
        session.own().settings().set(SettingId::kReadoutSource,
                                     static_cast<double>(protocol::ChannelView::kLeft));
        session.play(static_cast<double>(kBlock) / c.rate);
        frame = session.produce();
        ASSERT_TRUE(frame) << "no frame";
        EXPECT_EQ(frame->readoutView, protocol::ChannelView::kLeft);
        EXPECT_NEAR(frame->peak, kQuiet, 0.01 * kLoud) << "the quieter audio, not the loud";
    }
}

std::vector<SwitchBackCase> switchBackCases()
{
    std::vector<SwitchBackCase> cases;
    for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
        for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
            for (const bool frozen : {false, true}) {
                cases.push_back(
                    {.rate = rate, .timeBase = static_cast<TimeBase>(timeBase), .frozen = frozen});
            }
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, ReadoutSwitch, ::testing::ValuesIn(switchBackCases()),
                         [](const ::testing::TestParamInfo<SwitchBackCase>& info) {
                             return nameOf(info.param);
                         });

// The trigger's view follows what is drawn and the track, on the next frame: the source R, drawn,
// read; R hidden, L, the first drawn; R shown again, R; live, the track turned mono, L. In Trigger
// the window is then placed by the view named; in Free and Beats, which do not trigger, the frame
// still names it. Live and frozen (the window stays as placed), at the lowest and highest rates.
class TriggerChannelChange : public ::testing::TestWithParam<SwitchBackCase>
{
};

TEST_P(TriggerChannelChange, FollowsTheViewsAndTheTrack)
{
    const SwitchBackCase c = GetParam();
    constexpr double kAmplitude = 0.5;
    Session session(c.rate, 2);
    session.join({.frequency = 1000.0,
                  .amplitude = kAmplitude,
                  .offset = 0.0,
                  .rightPhase = std::numbers::pi / 2.0});
    configure(session.own(), Display::kSingle, c.timeBase);
    session.own().settings().set(SettingId::kTriggerSource,
                                 static_cast<double>(protocol::ChannelView::kRight));
    session.own().settings().set(SettingId::kTriggerSpan, 0.005);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kQuarterBar));
    session.analyzer().setColumns(4096);
    session.play(0.6, 0.01);
    if (c.frozen) {
        session.analyzer().setFrozen(true);
    }
    const auto check = [&](protocol::ChannelView expected, const char* step) {
        SCOPED_TRACE(step);
        session.play(static_cast<double>(kBlock) / c.rate);
        const auto frame = session.produce();
        ASSERT_TRUE(frame) << "no frame";
        EXPECT_EQ(frame->triggerView, expected);
        if (c.timeBase == TimeBase::kTrigger && !c.frozen) {
            ASSERT_EQ(frame->mode, protocol::FrameMode::kRaw);
            EXPECT_NEAR(atTriggerPoint(*frame, expected), 0.0, 0.2 * kAmplitude)
                << "placed by the view named";
        }
    };
    check(protocol::ChannelView::kRight, "R drawn");
    session.own().settings().set(SettingId::kChannelViews, 0b0001);
    check(protocol::ChannelView::kLeft, "R hidden");
    session.own().settings().set(SettingId::kChannelViews, 0b0011);
    check(protocol::ChannelView::kRight, "R shown again");
    // Frozen, the held audio keeps its channels, and the view held is not drawn again.
    if (!c.frozen) {
        session.own().setInputChannels(1);
        check(protocol::ChannelView::kLeft, "mono");
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, TriggerChannelChange, ::testing::ValuesIn(switchBackCases()),
                         [](const ::testing::TestParamInfo<SwitchBackCase>& info) {
                             return nameOf(info.param);
                         });

// A mono track is its one channel, not a stereo pair with a silent right: its trace (Single's
// view, or its lane in the multi-track view and Compare) reaches the tone's amplitude, and its
// readouts are the tone's level, not 6 dB lower. In every time base, live, frozen,
// and frozen at another span (whose lanes are rebuilt from the held audio), at every rate.
class MonoLanes : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(MonoLanes, ShowTheTrackAtItsLevel)
{
    const ModeCase c = GetParam();
    constexpr double kAmplitude = 0.5;
    Session session(c.rate, 1);
    session.join({.frequency = 1000.0, .amplitude = kAmplitude, .offset = 0.0});
    session.join({.frequency = 1500.0, .amplitude = kAmplitude, .offset = 0.0});
    configure(session.own(), c.mode, c.timeBase);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kQuarterBar));
    auto frame = playForReadouts(session, c.timeBase);
    // Live; frozen; and frozen at another span, which rebuilds the lanes from what is held.
    for (const int step : {0, 1, 2}) {
        SCOPED_TRACE(step == 0 ? "live" : step == 1 ? "frozen" : "frozen, another span");
        if (step == 1) {
            session.analyzer().setFrozen(true);
            session.play(static_cast<double>(kBlock) / c.rate);
            frame = session.produce();
        } else if (step == 2) {
            session.own().settings().set(SettingId::kTriggerSpan, 0.03);
            session.own().settings().set(SettingId::kFreeSpan, 0.2);
            session.own().settings().set(SettingId::kBeatSpan,
                                         static_cast<double>(BeatSpan::kHalfBar));
            frame = session.produce();
        }
        ASSERT_TRUE(frame) << "no frame";
        ASSERT_EQ(frame->laneCount > 0, isMulti(c.mode));
        const std::span<const float> own = trace(*frame, 0);
        const float loudest = std::ranges::max(own, {}, [](float v) { return std::abs(v); });
        EXPECT_NEAR(std::abs(loudest), kAmplitude, 0.02 * kAmplitude) << "this track's trace";
        EXPECT_NEAR(frame->peak, kAmplitude, 0.02 * kAmplitude);
        EXPECT_NEAR(frame->rms, kAmplitude / std::numbers::sqrt2, 0.02 * kAmplitude);
    }
}

std::vector<ModeCase> monoLaneCases()
{
    std::vector<ModeCase> cases;
    for (const Display mode : everyMode()) {
        for (const ModeCase& c : timeBaseCases(mode)) {
            cases.push_back({.rate = c.rate, .mode = mode, .channels = 1, .timeBase = c.timeBase});
        }
    }
    return cases;
}

class AnalysisGate : public ::testing::TestWithParam<ModeCase>
{
};

// A tone just above the analysis gate and one just below it: either way the level travels as
// measured, not floored, since the gate holds back analysis (Compare), not the readouts.
TEST_P(AnalysisGate, SendsTheLevelAsMeasuredOnEitherSide)
{
    const ModeCase c = GetParam();
    for (const double aboveGate : {1.0, -1.0}) {
        SCOPED_TRACE(aboveGate > 0.0 ? "above the gate" : "below the gate");
        Session session(c.rate, c.channels);
        // An RMS level aboveGate dB from the gate, as the readout view measures it: in Single L,
        // the first view drawn; in Multi and Compare the Mid of L and half of L; on a mono track
        // its one channel.
        const double viewGain = c.channels == 2 && isMulti(c.mode) ? 0.75 : 1.0;
        const double rms = std::pow(10.0, (protocol::kAnalysisGateDbfs + aboveGate) / 20.0);
        const double amplitude = rms * std::numbers::sqrt2 / viewGain;
        session.join({.frequency = 1000.0, .amplitude = amplitude, .offset = 0.0});
        show(session.own().settings(), c.mode);
        session.own().settings().set(SettingId::kTriggerSource,
                                     static_cast<double>(protocol::ChannelView::kMid));
        const auto frame = playForReadouts(session, c.timeBase);
        ASSERT_TRUE(frame);
        EXPECT_NEAR(frame->peak, amplitude * viewGain, amplitude * viewGain * 1e-3)
            << "the peak as measured";
        EXPECT_NEAR(frame->rms, rms, rms * 0.02);
    }
}

// Every display mode: the lanes and Compare read this track's channels as Single's views do.
std::vector<ModeCase> gateCases()
{
    std::vector<ModeCase> cases;
    for (const Display mode : everyMode()) {
        const std::vector<ModeCase> modeCases = timeBaseCases(mode);
        cases.insert(cases.end(), modeCases.begin(), modeCases.end());
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EveryRate, AnalysisGate, ::testing::ValuesIn(gateCases()), modeTestName);
INSTANTIATE_TEST_SUITE_P(EveryRate, MonoLanes, ::testing::ValuesIn(monoLaneCases()), modeTestName);

// Samples that are not finite, one every 100 ms on this track (NaN or infinite): measured around,
// so the readouts are those of the tone, and counted. The frame says how many the readouts measured
// around, and which lanes hold any: this track's and, in Compare, the sum's, never the other
// track's; none once they are past. In every mode and time base, and in Free at a 2 s span too,
// at the lowest and highest rates.
struct BrokenCase
{
    ModeCase mode;
    bool longSpan = false; // Free at 2 s rather than 200 ms.
    bool infinite = false;
};

std::string nameOf(const BrokenCase& c)
{
    return nameOf(c.mode) + (c.longSpan ? "_Long" : "") + (c.infinite ? "_Infinity" : "_NaN");
}

// What the readouts of a broken case measure: a beat at 120 BPM, or the span.
double brokenSpan(const BrokenCase& c)
{
    return c.mode.timeBase == TimeBase::kBeats ? 0.5 : c.longSpan ? 2.0 : 0.2;
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const BrokenCase& c, std::ostream* out)
{
    *out << nameOf(c);
}

std::vector<BrokenCase> brokenCases()
{
    std::vector<BrokenCase> cases;
    std::size_t index = 0;
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        for (const Display mode : everyMode()) {
            for (const bool infinite : {false, true}) {
                for (std::size_t base = 0; base < protocol::kTimeBaseCount; ++base) {
                    const auto timeBase = static_cast<TimeBase>(base);
                    for (const bool longSpan : {false, true}) {
                        if (longSpan && timeBase != TimeBase::kFree) {
                            continue; // Only Free has spans above a second.
                        }
                        cases.push_back({.mode = {.rate = rate,
                                                  .mode = mode,
                                                  .channels = index++ % 2 == 0 ? 2 : 1,
                                                  .timeBase = timeBase},
                                         .longSpan = longSpan,
                                         .infinite = infinite});
                    }
                }
            }
        }
    }
    return cases;
}

class BrokenSamples : public ::testing::TestWithParam<BrokenCase>
{
};

TEST_P(BrokenSamples, AreMeasuredAroundAndCounted)
{
    const BrokenCase c = GetParam();
    const double rate = c.mode.rate;
    const double span = brokenSpan(c);
    const double played = c.longSpan ? 3.0 : 1.5;
    Session session(rate, c.mode.channels);
    session.join({.frequency = 1000.0,
                  .brokenEvery = std::llround(rate / 10.0),
                  .broken = c.infinite ? std::numeric_limits<float>::infinity()
                                       : std::numeric_limits<float>::quiet_NaN(),
                  .brokenUntil = std::llround(rate * (played + 0.1))});
    session.join({.frequency = 1500.0});
    configure(session.own(), c.mode.mode, c.mode.timeBase);
    setSpan(session.own(), c.mode.timeBase, span);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kQuarterBar));
    session.play(played);
    auto frame = session.produce();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(std::isfinite(frame->peak)) << "measured around";
    EXPECT_TRUE(std::isfinite(frame->rms)) << "measured around";
    EXPECT_EQ(frame->invalidSamples, static_cast<std::uint32_t>(std::lround(span * 10.0)))
        << "one every 100 ms";
    switch (c.mode.mode) {
    case Display::kSingle:
    case Display::kSingleCompareOn: // Shows Single.
        EXPECT_EQ(frame->invalidLanes, 1U) << "no lanes: bit 0 for the traces";
        break;
    case Display::kMulti:
        EXPECT_EQ(frame->invalidLanes, 0b01U) << "this track's lane only";
        break;
    case Display::kCompare:
        EXPECT_EQ(frame->invalidLanes, 0b101U) << "this track's lane and the sum's";
        break;
    }

    // Once they are past what the frame shows, nothing is flagged any more: with frames along the
    // way, so a rolling lane keeps its columns and moves their flags with them.
    session.play(span * 2.0 + 0.5, 0.1);
    session.play(static_cast<double>(kBlock) / rate); // Audio since the last frame, for one more.
    frame = session.produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->invalidSamples, 0U) << "none left";
    EXPECT_EQ(frame->invalidLanes, 0U) << "none left";
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, BrokenSamples, ::testing::ValuesIn(brokenCases()),
                         [](const ::testing::TestParamInfo<BrokenCase>& info) {
                             return nameOf(info.param);
                         });

// Single has no lanes: bit 0 of `invalidLanes` says a trace drawn holds samples that are not
// finite, whichever channel they are on. A stereo track broken on its right channel only, the
// trigger on its left, with every set of views drawn: bit 0 is set exactly when R, M or S (each
// holding the right channel) is drawn. The readouts, with the Meter on, read Left and Right
// whatever view they measure the levels on: they count the samples and measure around them. With
// the Meter off they read the levels' channel alone, Left here, which holds none. In each time
// base, and in Free at a 2 s span, at the lowest and highest rates.
class BrokenChannel : public ::testing::TestWithParam<BrokenCase>
{
};

TEST_P(BrokenChannel, IsFlaggedWhenDrawn)
{
    const BrokenCase c = GetParam();
    const unsigned holdingRight = viewBit(protocol::ChannelView::kRight) |
                                  viewBit(protocol::ChannelView::kMid) |
                                  viewBit(protocol::ChannelView::kSide);
    for (const bool meter : {false, true}) {
        for (unsigned drawn = 1; drawn < (1U << protocol::kChannelViewCount); ++drawn) {
            SCOPED_TRACE(meter ? "Meter on" : "Meter off");
            SCOPED_TRACE(drawn);
            Session session(c.mode.rate, 2);
            session.join({.frequency = 1000.0,
                          .brokenEvery = std::llround(c.mode.rate / 10.0),
                          .broken = c.infinite ? std::numeric_limits<float>::infinity()
                                               : std::numeric_limits<float>::quiet_NaN(),
                          .brokenRightOnly = true});
            configure(session.own(), c.mode.mode, c.mode.timeBase);
            setMeter(session.own(), meter);
            session.own().settings().set(SettingId::kTriggerSource,
                                         static_cast<double>(protocol::ChannelView::kLeft));
            session.own().settings().set(SettingId::kReadoutSource,
                                         static_cast<double>(protocol::ChannelView::kLeft));
            session.own().settings().set(SettingId::kChannelViews, static_cast<double>(drawn));
            setSpan(session.own(), c.mode.timeBase, brokenSpan(c));
            session.own().settings().set(SettingId::kBeatSpan,
                                         static_cast<double>(BeatSpan::kQuarterBar));
            session.play(c.longSpan ? 3.0 : 1.5);
            const auto frame = session.produce();
            ASSERT_TRUE(frame);
            EXPECT_EQ(frame->invalidLanes, (drawn & holdingRight) != 0 ? 1U : 0U)
                << "set when a trace drawn holds some";
            EXPECT_TRUE(std::isfinite(frame->peak) && std::isfinite(frame->rms))
                << "measured around them";
            if (meter) {
                // The Meter reads Left and Right too, for Loudness and Correlation, whatever it
                // measures its levels on: the samples are counted whichever view that is.
                EXPECT_GT(frame->invalidSamples, 0U) << "Right, read by the Meter, holds some";
                EXPECT_FALSE(std::isnan(frame->correlation)) << "measured around them";
            } else {
                const bool readsLeft = (drawn & viewBit(protocol::ChannelView::kLeft)) != 0;
                EXPECT_EQ(frame->invalidSamples > 0U, !readsLeft)
                    << "counted on the levels' channel alone";
            }
        }
    }
}

// The Single cases, each on a stereo track.
std::vector<BrokenCase> singleStereoCases()
{
    std::vector<BrokenCase> cases;
    for (BrokenCase c : brokenCases()) {
        if (!isMulti(c.mode.mode)) {
            c.mode.channels = 2;
            cases.push_back(c);
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, BrokenChannel, ::testing::ValuesIn(singleStereoCases()),
                         [](const ::testing::TestParamInfo<BrokenCase>& info) {
                             return nameOf(info.param);
                         });

// Another track broken, the third: its own lane is flagged, never this track's. In Compare the sum,
// which takes every track shown, is flagged with it. In Multi and Compare, each time base and Free
// at a 2 s span, at the lowest and highest rates.
class BrokenOtherTrack : public ::testing::TestWithParam<BrokenCase>
{
};

TEST_P(BrokenOtherTrack, FlagsItsLaneAndTheSumItIsPartOf)
{
    const BrokenCase c = GetParam();
    Session session(c.mode.rate, c.mode.channels);
    session.join({.frequency = 1000.0});
    session.join({.frequency = 1500.0});
    session.join({.frequency = 700.0,
                  .brokenEvery = std::llround(c.mode.rate / 10.0),
                  .broken = c.infinite ? std::numeric_limits<float>::infinity()
                                       : std::numeric_limits<float>::quiet_NaN()});
    configure(session.own(), c.mode.mode, c.mode.timeBase);
    setSpan(session.own(), c.mode.timeBase, brokenSpan(c));
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kQuarterBar));
    session.play(c.longSpan ? 3.0 : 1.5);
    const auto frame = session.produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->invalidLanes, c.mode.mode == Display::kCompare ? 0b1100U : 0b100U)
        << "the third track's lane, and in Compare the sum's";
    EXPECT_EQ(frame->invalidSamples, 0U) << "this track's readouts hold none";
}

// The Multi and Compare cases.
std::vector<BrokenCase> laneCases()
{
    std::vector<BrokenCase> cases;
    std::ranges::copy_if(brokenCases(), std::back_inserter(cases),
                         [](const BrokenCase& c) { return isMulti(c.mode.mode); });
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, BrokenOtherTrack, ::testing::ValuesIn(laneCases()),
                         [](const ::testing::TestParamInfo<BrokenCase>& info) {
                             return nameOf(info.param);
                         });

// The lanes named for holding samples that are not finite are those whose part on screen holds
// one. A single NaN 1.4 s into a 2 s beat span is named in the whole view, zoomed onto the second
// half, and zoomed so that it lies 10 ms from either edge, also with a column for every sample,
// shown raw, but not zoomed onto the first half, onto the last quarter, or raw just before it. In
// Free, one 0.3 s back in a rolling 2 s span is named, also on a frame where the roll did not move;
// frozen, one three quarters into a 200 ms span is named in the views that show it, not in those on
// either side. In Compare, a compared track holding one names the sum it is part of, though this
// track holds none. In every mode, at the lowest and highest rates.
class InvalidOnScreen : public ::testing::TestWithParam<Display>
{
};

TEST_P(InvalidOnScreen, NamesTheLanesWhosePartShownHoldsOne)
{
    const Display mode = GetParam();
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        SCOPED_TRACE(rateName(rate));
        {
            SCOPED_TRACE("Beats");
            Session session(rate, 2);
            session.join({.brokenAt = static_cast<std::int64_t>(std::llround(1.4 * rate))});
            session.join({.frequency = 1500.0});
            configure(session.own(), mode, TimeBase::kBeats);
            session.own().settings().set(SettingId::kBeatSpan,
                                         static_cast<double>(BeatSpan::kOneBar));
            session.play(1.9, protocol::kReadoutIntervalSeconds / 2.0);
            for (const auto& [start, end, named] :
                 {std::tuple{0.0, 1.0, true}, std::tuple{0.0, 0.5, false},
                  std::tuple{0.5, 1.0, true}, std::tuple{0.5, 0.705, true},
                  std::tuple{0.695, 0.9, true}, std::tuple{0.75, 1.0, false}}) {
                SCOPED_TRACE(testing::Message() << "view " << start << " to " << end);
                session.analyzer().setView(start, end);
                session.play(0.01);
                const auto frame = session.produce();
                ASSERT_TRUE(frame) << "no frame";
                EXPECT_EQ((frame->invalidLanes & 1U) != 0, named) << "this track";
            }
            // With a column for every sample shown, raw: 20 ms around it, and 20 ms before.
            session.analyzer().setColumns(8192);
            for (const auto& [start, end, named] :
                 {std::tuple{0.695, 0.705, true}, std::tuple{0.68, 0.69, false}}) {
                SCOPED_TRACE(testing::Message() << "raw, view " << start << " to " << end);
                session.analyzer().setView(start, end);
                session.play(0.01);
                const auto frame = session.produce();
                ASSERT_TRUE(frame) << "no frame";
                ASSERT_EQ(frame->mode, protocol::FrameMode::kRaw);
                EXPECT_EQ((frame->invalidLanes & 1U) != 0, named) << "this track";
            }
        }
        {
            SCOPED_TRACE("Free, rolling");
            Session session(rate, 2);
            const double played = captureSeconds(rate) + 1.0;
            session.join(
                {.brokenAt = static_cast<std::int64_t>(std::llround((played - 0.3) * rate))});
            session.join({.frequency = 1500.0});
            configure(session.own(), mode, TimeBase::kFree);
            session.own().settings().set(SettingId::kFreeSpan, 2.0);
            // The capture wraps; frames only once the roll is nearly where it is checked.
            session.play(played - 0.1);
            session.play(0.1, 0.01);
            // The first frame shows newly arrived audio; the second redraws the roll as it
            // stands, from its kept columns: a setting changes, no audio arrives.
            for (int again = 0; again < 2; ++again) {
                if (again == 0) {
                    session.play(0.01);
                } else {
                    session.own().settings().set(SettingId::kVerticalScale, -6.0);
                }
                const auto frame = session.produce();
                ASSERT_TRUE(frame) << "no frame";
                ASSERT_TRUE(frame->has(FrameFlag::kFreeRunning));
                EXPECT_NE(frame->invalidLanes & 1U, 0U) << "this track, frame " << again;
            }
        }
        {
            SCOPED_TRACE("Free, frozen and zoomed");
            Session session(rate, 2);
            const double played = captureSeconds(rate) + 1.0;
            // 50 ms from the end of a 200 ms span: three quarters of the way across.
            session.join(
                {.brokenAt = static_cast<std::int64_t>(std::llround((played - 0.05) * rate))});
            session.join({.frequency = 1500.0});
            configure(session.own(), mode, TimeBase::kFree);
            session.own().settings().set(SettingId::kFreeSpan, 0.2);
            session.play(played - 0.1);
            session.play(0.1, 0.01); // Its last frame shows the newest audio live.
            session.analyzer().setFrozen(true);
            const auto whole = session.produce();
            ASSERT_TRUE(whole) << "no frozen frame";
            EXPECT_NE(whole->invalidLanes & 1U, 0U) << "this track, in the whole view";
            for (const auto& [start, end, named] :
                 {std::tuple{0.0, 0.5, false}, std::tuple{0.5, 1.0, true},
                  std::tuple{0.85, 1.0, false}}) {
                SCOPED_TRACE(testing::Message() << "view " << start << " to " << end);
                session.analyzer().setView(start, end);
                const auto frame = session.produce();
                ASSERT_TRUE(frame) << "no frame";
                ASSERT_TRUE(frame->has(FrameFlag::kFrozen));
                EXPECT_EQ((frame->invalidLanes & 1U) != 0, named) << "this track";
            }
        }
        if (mode == Display::kCompare) {
            for (std::size_t base = 0; base < protocol::kTimeBaseCount; ++base) {
                const auto timeBase = static_cast<TimeBase>(base);
                SCOPED_TRACE("sum, " + timeBaseName(timeBase));
                Session session(rate, 2);
                session.join({.frequency = 1000.0});
                session.join({.brokenAt = static_cast<std::int64_t>(std::llround(1.85 * rate))});
                configure(session.own(), mode, timeBase);
                session.own().settings().set(SettingId::kBeatSpan,
                                             static_cast<double>(BeatSpan::kOneBar));
                setSpan(session.own(), timeBase, 0.2);
                session.play(1.9, 0.01);
                session.play(0.01);
                const auto frame = session.produce();
                ASSERT_TRUE(frame) << "no frame";
                ASSERT_EQ(frame->laneCount, 3);
                EXPECT_EQ(frame->invalidLanes & 1U, 0U) << "this track holds none";
                EXPECT_NE(frame->invalidLanes & 0b010U, 0U) << "the compared track";
                EXPECT_NE(frame->invalidLanes & 0b100U, 0U) << "the sum it is part of";
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(EveryMode, InvalidOnScreen, ::testing::ValuesIn(everyMode()),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

// Every sample of this track not finite: nothing to measure, so its readouts have no value (NaN),
// never the level of silence, and every sample measured is counted; with the Meter on too, whose
// rows would else read a level, a DC offset or a loudness of audio that has none. In every mode
// and time base, and in Free at a 2 s span, at the lowest and highest rates.
class AllBroken : public ::testing::TestWithParam<BrokenCase>
{
};

TEST_P(AllBroken, HasNoReadouts)
{
    const BrokenCase c = GetParam();
    const double rate = c.mode.rate;
    const double span = brokenSpan(c);
    Session session(rate, c.mode.channels);
    session.join({.frequency = 1000.0,
                  .brokenEvery = 1,
                  .broken = c.infinite ? std::numeric_limits<float>::infinity()
                                       : std::numeric_limits<float>::quiet_NaN()});
    session.join({.frequency = 1500.0});
    configure(session.own(), c.mode.mode, c.mode.timeBase);
    setSpan(session.own(), c.mode.timeBase, span);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kQuarterBar));
    session.play(c.longSpan ? 3.0 : 1.5);
    const auto frame = session.produce();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(std::isnan(frame->peak)) << "no value, not silence";
    EXPECT_TRUE(std::isnan(frame->rms)) << "no value, not silence";
    // Every sample the readouts measure: the span, rounded out to whole blocks of levels.
    const auto spanSamples = static_cast<std::uint32_t>(std::lround(span * rate));
    const auto block = static_cast<std::uint32_t>(std::lround(kStatsBlockSeconds * rate));
    EXPECT_GE(frame->invalidSamples, spanSamples);
    EXPECT_LE(frame->invalidSamples, spanSamples + 2 * block);
    if (c.mode.mode == Display::kCompare) {
        // The sum is never finite, so it is not measured, and neither is the other track there.
        EXPECT_TRUE(std::isnan(session.field(FrameHeader::kSumGainOffset)))
            << "sum gain " << session.field(FrameHeader::kSumGainOffset);
        EXPECT_TRUE(std::isnan(session.field(FrameHeader::kFixedGainOffset)))
            << "fixed gain " << session.field(FrameHeader::kFixedGainOffset);
    }

    setMeter(session.own(), true);
    session.play(0.005);
    const auto metered = session.produce();
    ASSERT_TRUE(metered);
    EXPECT_TRUE(std::isnan(metered->peak)) << "Peak " << metered->peak;
    EXPECT_TRUE(std::isnan(metered->rms)) << "RMS " << metered->rms;
    EXPECT_TRUE(std::isnan(metered->truePeak)) << "True peak " << metered->truePeak;
    EXPECT_TRUE(std::isnan(metered->dcOffset)) << "DC " << metered->dcOffset;
    EXPECT_TRUE(std::isnan(metered->loudness)) << "Loudness " << metered->loudness;
    EXPECT_TRUE(std::isnan(metered->correlation)) << "Correlation " << metered->correlation;
    // The Meter measures the visible part to the sample.
    EXPECT_GE(metered->invalidSamples + block, spanSamples);
    EXPECT_LE(metered->invalidSamples, spanSamples + 2 * block);
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, AllBroken, ::testing::ValuesIn(brokenCases()),
                         [](const ::testing::TestParamInfo<BrokenCase>& info) {
                             return nameOf(info.param);
                         });

// A copy with every other sample of this track not finite still sums as a copy, 10 log10(2) =
// +3.0103 dB, in every time base, live and frozen: where the sum is not finite, neither track
// is measured, rather than the other track's energy counting where the sum's does not.
class BrokenCopy : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(BrokenCopy, SumsAsACopyAroundThem)
{
    const ModeCase c = GetParam();
    const double identical = 10.0 * std::log10(2.0);
    Session session(c.rate, c.channels);
    session.join({.kind = Signal::Kind::kNoise, .brokenEvery = 2});
    session.join({.kind = Signal::Kind::kNoise});
    show(session.own().settings(), Display::kCompare);
    auto frame = playForReadouts(session, c.timeBase);
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        if (frozen) {
            session.analyzer().setFrozen(true);
            session.play(static_cast<double>(kBlock) / c.rate);
            frame = session.produce();
        }
        ASSERT_TRUE(frame);
        EXPECT_NEAR(session.field(FrameHeader::kSumGainOffset), identical, 1e-3);
        EXPECT_NEAR(session.field(FrameHeader::kFixedGainOffset), identical, 1e-3);
    }
}

INSTANTIATE_TEST_SUITE_P(EveryRate, BrokenCopy,
                         ::testing::ValuesIn(timeBaseCases(Display::kCompare)), modeTestName);

// Each compared track's match describes the audio on screen, frozen or live: it is searched again
// when the display freezes, even with no audio after it (a stopped transport), and when the
// freeze ends, without waiting for its next refresh. This track starts just before; a copy of it
// and an inverted copy read as what they are, not as never sounding together. In every time base,
// at every rate.
class FrozenMatch : public ::testing::TestWithParam<ModeCase>
{
protected:
    void SetUp() override
    {
        const ModeCase c = GetParam();
        start = static_cast<std::int64_t>(std::llround(kStart * c.rate));
        session = std::make_unique<Session>(c.rate, c.channels);
        session->join({.kind = Signal::Kind::kNoise, .startsAt = start});
        session->join({.kind = Signal::Kind::kNoise});
        session->join({.kind = Signal::Kind::kNoise, .amplitude = -0.25});
        configure(session->own(), Display::kCompare, c.timeBase);
        setSpan(session->own(), c.timeBase, 0.2);
        session->own().settings().set(SettingId::kBeatSpan,
                                      static_cast<double>(BeatSpan::kQuarterBar));
        std::vector<std::uint64_t> others;
        for (const Member& member : session->registry.members()) {
            if (member.engine.get() != &session->own()) {
                others.push_back(member.serial);
            }
        }
        session->analyzer().setShown(others);
    }

    // The frames that follow with no more audio, the last of them; they stop once every match is
    // searched (a failure, rather than a hang, if they do not).
    std::optional<DecodedFrame> settle()
    {
        std::optional<DecodedFrame> last;
        for (int frames = 0; frames < 64; ++frames) {
            auto produced = session->produce();
            if (!produced) {
                return last;
            }
            last = std::move(produced);
        }
        ADD_FAILURE() << "frames go on with nothing new";
        return last;
    }

    // Both copies read as copies: in time, with their polarity.
    static void expectCopies(const DecodedFrame& frame)
    {
        ASSERT_EQ(frame.laneCount, 4);
        for (const std::size_t track : {1U, 2U}) {
            SCOPED_TRACE(track == 1 ? "copy" : "inverted copy");
            const std::size_t at =
                std::size_t{frame.laneCount} * traceLength(frame) + track * protocol::kMatchValues;
            EXPECT_TRUE(std::isfinite(frame.data.at(at))) << "they sound together";
            EXPECT_EQ(frame.data.at(at + 1), 0.0f) << "in time";
            EXPECT_EQ(frame.data.at(at + 2), track == 1 ? 1.0f : -1.0f) << "related, its polarity";
        }
    }

    static constexpr double kStart = 1.5;
    // Shorter than the spacing of match searches: no search of the live view sees it.
    static constexpr double kBrief = 0.5 * kMatchSpacingSeconds;
    std::int64_t start = 0;
    std::unique_ptr<Session> session;
};

TEST_P(FrozenMatch, DescribesTheAudioFrozen)
{
    session->play(kStart, 0.01);
    session->play(kBrief); // This track sounds.
    ASSERT_TRUE(session->produce());

    session->analyzer().setFrozen(true);
    const auto frame = settle();
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->has(FrameFlag::kFrozen));
    expectCopies(*frame);
}

// Panned, a frozen Trigger or Free view shows other held audio, and its matches are searched again
// over what it shows: moved back to before this track sounds, nothing sounds together with it, and
// each match is no value. (Beats pans by its crop, which is a new view as zooming is.)
TEST_P(FrozenMatch, FollowsAPannedView)
{
    if (GetParam().timeBase == TimeBase::kBeats) {
        GTEST_SKIP() << "Beats pans by its crop";
    }
    session->play(kStart, 0.01);
    session->play(kBrief); // This track sounds.
    ASSERT_TRUE(session->produce());
    session->analyzer().setFrozen(true);
    auto frame = settle();
    ASSERT_TRUE(frame);
    expectCopies(*frame);

    session->analyzer().setPan(0.3); // Back to before it sounds.
    frame = settle();
    ASSERT_TRUE(frame) << "no frame for the pan";
    for (const std::size_t track : {1U, 2U}) {
        const std::size_t at =
            std::size_t{frame->laneCount} * traceLength(*frame) + track * protocol::kMatchValues;
        EXPECT_TRUE(std::isnan(frame->data.at(at))) << "track " << track << " still matched";
    }
}

TEST_P(FrozenMatch, DescribesTheLiveAudioOnceUnfrozen)
{
    session->play(kStart - kBrief, 0.01);
    session->analyzer().setFrozen(true); // Before this track sounds.
    settle();
    session->play(2.0 * kBrief); // This track sounds, while frozen.

    // Live again, long enough to search both, one by one.
    session->analyzer().setFrozen(false);
    std::optional<DecodedFrame> frame;
    for (int step = 0; step < 20; ++step) {
        session->play(0.1 * kMatchSpacingSeconds);
        if (auto produced = session->produce()) {
            frame = std::move(produced);
        }
    }
    ASSERT_TRUE(frame);
    ASSERT_FALSE(frame->has(FrameFlag::kFrozen));
    expectCopies(*frame);
}

// Live with the transport stopped, the host may send no audio: a change of the view still has
// each match searched again.
TEST_P(FrozenMatch, FollowsTheViewWithTheTransportStopped)
{
    session->playing = false;
    session->play(kStart - kBrief, 0.01);
    session->play(2.0 * kBrief); // This track sounds.

    session->analyzer().setView(0.0, 0.9); // Still where it sounds, and the playhead.
    // Both matches, each searched over a few frames.
    std::optional<DecodedFrame> frame;
    for (int step = 0; step < 64; ++step) {
        if (auto produced = session->produce()) {
            frame = std::move(produced);
        }
    }
    ASSERT_TRUE(frame);
    expectCopies(*frame);
}

INSTANTIATE_TEST_SUITE_P(EveryRate, FrozenMatch,
                         ::testing::ValuesIn(timeBaseCases(Display::kCompare)), modeTestName);

class NoDataIsNaN : public ::testing::TestWithParam<ModeCase>
{
};

// Compared with a silent track, nothing sounds together: the correlation and both summing gains
// travel as NaN, not 0, live and frozen.
TEST_P(NoDataIsNaN, ReportsNoMatchWithASilentTrack)
{
    const ModeCase c = GetParam();
    Session session(c.rate, c.channels);
    session.join({.kind = Signal::Kind::kNoise});
    session.join({.kind = Signal::Kind::kSilence});
    show(session.own().settings(), Display::kCompare);
    auto frame = playForReadouts(session, c.timeBase);
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        if (frozen) {
            session.analyzer().setFrozen(true);
            session.play(static_cast<double>(kBlock) / c.rate);
            frame = session.produce();
        }
        ASSERT_TRUE(frame);
        ASSERT_TRUE(frame->has(FrameFlag::kCompared));
        ASSERT_EQ(frame->laneCount, 3);
        const std::size_t at = std::size_t{frame->laneCount} * traceLength(*frame) +
                               std::size_t{1} * protocol::kMatchValues;
        ASSERT_LT(at, frame->data.size());
        EXPECT_TRUE(std::isnan(frame->data.at(at))) << "correlation " << frame->data.at(at);
        EXPECT_TRUE(std::isnan(session.field(FrameHeader::kSumGainOffset)))
            << "sum gain " << session.field(FrameHeader::kSumGainOffset);
        EXPECT_TRUE(std::isnan(session.field(FrameHeader::kFixedGainOffset)))
            << "fixed gain " << session.field(FrameHeader::kFixedGainOffset);
    }
}

INSTANTIATE_TEST_SUITE_P(EveryRate, NoDataIsNaN,
                         ::testing::ValuesIn(timeBaseCases(Display::kCompare)), modeTestName);

// --- A delayed copy, fixed --------------------------------------------------------------------

// A track against a copy of itself `delay` samples late (negative: early), in either time base.
// Delays around the match reach (protocol::kMatchReachSeconds either way): the shortest, either
// side of the reach, the reach early, and just beyond it, where no match is found.
// Where the transport stops in Beats mode: anywhere, or with the playhead within the delay after a
// sweep's start or before its end, where the measured part's edges meet the sweep's.
enum class Stop : std::uint8_t
{
    kAnywhere,
    kAfterAWrap,
    kBeforeAWrap,
};

struct DelayCase
{
    ModeCase mode;
    std::string_view name; // Of the delay, and where the transport stops if not anywhere.
    std::int64_t delay = 0;
    bool found = true; // Within the reach: the copy is matched at its delay.
    Stop stop = Stop::kAnywhere;
};

std::string delayTestName(const ::testing::TestParamInfo<DelayCase>& info)
{
    return nameOf(info.param.mode) + "_" + std::string(info.param.name);
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const DelayCase& c, std::ostream* out)
{
    *out << nameOf(c.mode) << "_" << c.name;
}

std::vector<DelayCase> delayCases()
{
    std::vector<DelayCase> cases;
    for (const ModeCase& mode : timeBaseCases(Display::kCompare)) {
        const std::int64_t reach = std::lround(protocol::kMatchReachSeconds * mode.rate);
        cases.push_back({.mode = mode, .name = "OneSample", .delay = 1});
        cases.push_back({.mode = mode, .name = "UnderTheReach", .delay = reach - 1});
        cases.push_back({.mode = mode, .name = "AtTheReach", .delay = reach});
        cases.push_back({.mode = mode, .name = "LeadingAtTheReach", .delay = -reach});
        cases.push_back({.mode = mode, .name = "LeadingByHalfTheReach", .delay = -reach / 2});
        cases.push_back(
            {.mode = mode, .name = "BeyondTheReach", .delay = reach + 1, .found = false});
        if (mode.timeBase == TimeBase::kBeats) {
            for (const std::int64_t delay : {reach, -reach}) {
                const bool late = delay > 0;
                cases.push_back({.mode = mode,
                                 .name = late ? "AtTheReachAfterAWrap" : "LeadingAfterAWrap",
                                 .delay = delay,
                                 .stop = Stop::kAfterAWrap});
                cases.push_back({.mode = mode,
                                 .name = late ? "AtTheReachBeforeAWrap" : "LeadingBeforeAWrap",
                                 .delay = delay,
                                 .stop = Stop::kBeforeAWrap});
            }
        }
    }
    return cases;
}

// Compare at the lowest and highest rates, in every time base.
std::vector<ModeCase> edgeCompareCases()
{
    std::vector<ModeCase> cases;
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
            cases.push_back({.rate = rate,
                             .mode = Display::kCompare,
                             .channels = rate == kSampleRates.front() ? 2 : 1,
                             .timeBase = static_cast<TimeBase>(timeBase)});
        }
    }
    return cases;
}

// Frozen, changing the tracks compared measures the sum again: with a copy and an inverted copy of
// this track compared, the three sum as this track alone (about -4.8 dB against three unrelated
// tracks, as the copies cancel each other); with the inverted copy unchecked, they sum as a copy
// does, +3 dB, not as measured before. In every time base, at the lowest and highest rates.
class FrozenComparison : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(FrozenComparison, MeasuresTheTracksNowChecked)
{
    const ModeCase c = GetParam();
    const double identical = 10.0 * std::log10(2.0);
    Session session(c.rate, c.channels);
    session.join({.kind = Signal::Kind::kNoise});
    session.join({.kind = Signal::Kind::kNoise});
    session.join({.kind = Signal::Kind::kNoise, .amplitude = -0.25});
    show(session.own().settings(), Display::kCompare);
    std::vector<std::uint64_t> others;
    for (const Member& member : session.registry.members()) {
        if (member.engine.get() != &session.own()) {
            others.push_back(member.serial);
        }
    }
    ASSERT_EQ(others.size(), 2U);
    session.analyzer().setShown(others);
    ASSERT_TRUE(playForReadouts(session, c.timeBase)) << "no live frame";
    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / c.rate);
    ASSERT_TRUE(session.produce()) << "no frozen frame";
    const float both = session.field(FrameHeader::kSumGainOffset);
    ASSERT_LT(both, identical - 1.0) << "the copies cancel each other";
    // No longer shown: the inverted copy, the second other track by creation order.
    session.analyzer().setShown({others.front()});
    std::optional<DecodedFrame> frame;
    for (int step = 0; step < 8; ++step) { // Frames go on while the sum is measured again.
        if (auto produced = session.produce()) {
            frame = std::move(produced);
        }
    }
    ASSERT_TRUE(frame) << "no frame after the change";
    ASSERT_TRUE(frame->has(FrameFlag::kFrozen));
    EXPECT_NEAR(session.field(FrameHeader::kSumGainOffset), identical, 0.05)
        << "a copy, not as before (" << both << " dB)";
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, FrozenComparison, ::testing::ValuesIn(edgeCompareCases()),
                         modeTestName);

// The summing gains count only where the tracks sound together: with this track playing
// throughout and a copy of it that stops half way through what is shown, an exact copy sums at
// about +3 dB, and one 37 samples late at about +3 dB once fixed, not watered down towards +2 dB
// by the half where this track plays alone. In every time base, at the lowest and highest rates.
class PartlyPresentCopy : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(PartlyPresentCopy, SumsWhereBothSound)
{
    const ModeCase c = GetParam();
    const double identical = 10.0 * std::log10(2.0);
    // Spans of 0.5 s (a beat) and 0.4 s, long against the 10 ms blocks the gains are measured in,
    // so that the one block the copy only partly fills hardly counts.
    const double shown = c.timeBase == TimeBase::kBeats ? 0.5 : 0.4;
    constexpr double kPlayed = 2.0;
    const auto until = static_cast<std::int64_t>(std::llround((kPlayed - (shown / 2.0)) * c.rate));
    for (const std::int64_t delay : {std::int64_t{0}, std::int64_t{37}}) {
        SCOPED_TRACE(testing::Message() << "late by " << delay);
        Session session(c.rate, c.channels);
        session.join({.kind = Signal::Kind::kNoise});
        session.join({.kind = Signal::Kind::kNoise, .delay = delay, .endsAt = until});
        configure(session.own(), Display::kCompare, c.timeBase);
        session.own().settings().set(SettingId::kBeatSpan,
                                     static_cast<double>(BeatSpan::kQuarterBar));
        setSpan(session.own(), c.timeBase, shown);
        session.play(kPlayed, protocol::kReadoutIntervalSeconds / 2.0);
        session.play(static_cast<double>(kBlock) / c.rate);
        ASSERT_TRUE(session.produce()) << "no frame";
        if (delay == 0) {
            EXPECT_NEAR(session.field(FrameHeader::kSumGainOffset), identical, 0.1) << "sum";
        }
        EXPECT_NEAR(session.field(FrameHeader::kFixedGainOffset), identical, 0.1) << "fixed";
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, PartlyPresentCopy, ::testing::ValuesIn(edgeCompareCases()),
                         modeTestName);

class DelayedCopy : public ::testing::TestWithParam<DelayCase>
{
};

// Lined up again, a delayed copy sums as a copy does, 10 log10(2) = +3.0103 dB louder than two
// unrelated tracks, in every time base, live and frozen: the fix is measured only where the
// shifted copy has samples of the same moments, never with the silence past the sweep's edges or
// the audio of another pass beyond the playhead, including with the playhead next to those edges.
// Beyond the reach nothing is fixed, and the fix's gain is the sum's. Frozen, the gains are
// measured again (the view is zoomed and back), not kept from before the freeze.
TEST_P(DelayedCopy, SumsAsACopyOnceFixed)
{
    const DelayCase c = GetParam();
    const double identical = 10.0 * std::log10(2.0);
    Session session(c.mode.rate, c.mode.channels);
    session.join({.kind = Signal::Kind::kNoise});
    session.join({.kind = Signal::Kind::kNoise, .delay = c.delay});
    show(session.own().settings(), Display::kCompare);
    auto frame = playForReadouts(session, c.mode.timeBase);
    // A block at a time, until the playhead is within the delay of the sweep's edge.
    const auto distance = static_cast<double>(std::abs(c.delay));
    for (int block = 0; c.stop != Stop::kAnywhere && block < 4000; ++block) {
        ASSERT_TRUE(frame);
        const double playhead = frame->cursor * frame->visibleSamples;
        if (c.stop == Stop::kAfterAWrap
                ? playhead > 0.0 && playhead < distance
                : playhead < frame->visibleSamples && playhead > frame->visibleSamples - distance) {
            break;
        }
        session.play(static_cast<double>(kBlock) / c.mode.rate);
        if (auto produced = session.produce()) {
            frame = std::move(produced);
        }
    }
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        if (frozen) {
            session.analyzer().setFrozen(true);
            session.play(static_cast<double>(kBlock) / c.mode.rate);
            ASSERT_TRUE(session.produce()) << "no frozen frame";
            session.analyzer().setView(0.0, 0.5);
            ASSERT_TRUE(session.produce()) << "no frozen frame zoomed in";
            session.analyzer().setView(0.0, 1.0);
            frame = session.produce();
        }
        ASSERT_TRUE(frame);
        ASSERT_TRUE(frame->has(FrameFlag::kCompared));
        ASSERT_EQ(frame->laneCount, 3);
        const std::size_t at = std::size_t{frame->laneCount} * traceLength(*frame) +
                               std::size_t{1} * protocol::kMatchValues;
        ASSERT_LT(at + 2, frame->data.size());
        const float sumGain = session.field(FrameHeader::kSumGainOffset);
        const float fixedGain = session.field(FrameHeader::kFixedGainOffset);
        if (c.found) {
            EXPECT_EQ(frame->data.at(at + 1), static_cast<float>(c.delay)) << "offset";
            EXPECT_EQ(frame->data.at(at + 2), 1.0f) << "polarity";
            EXPECT_NEAR(fixedGain, identical, 0.01) << "fixed gain";
        } else {
            EXPECT_EQ(frame->data.at(at + 2), 0.0f) << "no match beyond the reach";
            EXPECT_TRUE(std::isnan(frame->data.at(at + 1))) << "no offset beyond the reach";
            EXPECT_NEAR(fixedGain, sumGain, 1e-6) << "nothing to fix";
        }
    }
    if (c.found) {
        // Last (so that the checks above see the playhead where the stop left it): previewing the
        // fix, the copy is lined up: the sum is this track doubled, except at the edge where the
        // shifted copy runs out of samples (the start for a copy that leads, the end for one that
        // lags) and next to a sweep's playhead, where it reaches into the other pass.
        SCOPED_TRACE("previewing the fix");
        session.analyzer().setPreviewFix(true);
        session.play(static_cast<double>(kBlock) / c.mode.rate);
        const auto previewed = session.produce();
        ASSERT_TRUE(previewed) << "no frame previewing the fix";
        ASSERT_EQ(previewed->laneCount, 3);
        const std::span<const float> own = trace(*previewed, 0);
        const std::span<const float> sum = trace(*previewed, 2);
        const double perPoint =
            static_cast<double>(previewed->visibleSamples) / previewed->pointCount;
        const std::size_t valuesPerPoint = previewed->mode == FrameMode::kMinMax ? 2 : 1;
        const std::size_t edge =
            valuesPerPoint * static_cast<std::size_t>(std::ceil(
                                 (std::abs(static_cast<double>(c.delay)) / perPoint) + 2.0));
        const auto playhead = previewed->cursor >= 0.0f
                                  ? static_cast<std::ptrdiff_t>(std::llround(
                                        previewed->cursor * static_cast<float>(own.size())))
                                  : -static_cast<std::ptrdiff_t>(own.size());
        const std::size_t first = c.delay < 0 ? edge : valuesPerPoint;
        const std::size_t last = own.size() - (c.delay > 0 ? edge : valuesPerPoint);
        std::size_t differing = 0;
        for (std::size_t i = first; i < last; ++i) {
            if (std::abs(static_cast<std::ptrdiff_t>(i) - playhead) <=
                static_cast<std::ptrdiff_t>(edge)) {
                continue;
            }
            differing += std::abs(sum[i] - (2.0f * own[i])) > 1e-4f ? 1 : 0;
        }
        EXPECT_EQ(differing, 0U) << "of " << last - first << " values";
        session.analyzer().setPreviewFix(false);
    }
}

INSTANTIATE_TEST_SUITE_P(EveryRate, DelayedCopy, ::testing::ValuesIn(delayCases()), delayTestName);

// Zoomed so that the view starts part way into the span, Compare's search of what is shown still
// succeeds, and finds a copy 37 samples late: in every time base, at the lowest and highest rates.
// (The copy is late by the same amount throughout, so this does not tell a search of the view from
// one of the whole span.)
class ZoomedMatch : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(ZoomedMatch, FindsTheCopyInTheView)
{
    const ModeCase c = GetParam();
    constexpr std::int64_t kDelay = 37;
    for (const auto& [start, end] : {std::pair{0.5, 1.0}, std::pair{0.25, 0.75}}) {
        SCOPED_TRACE(testing::Message() << "view " << start << " to " << end);
        Session session(c.rate, c.channels);
        session.join({.kind = Signal::Kind::kNoise});
        session.join({.kind = Signal::Kind::kNoise, .delay = kDelay});
        configure(session.own(), Display::kCompare, c.timeBase);
        setSpan(session.own(), c.timeBase, 0.2);
        session.own().settings().set(SettingId::kBeatSpan,
                                     static_cast<double>(BeatSpan::kQuarterBar));
        // Zoomed from the start, so that the match shown comes from searches of the zoomed view.
        session.analyzer().setView(start, end);
        session.play(2.0, 0.01);
        session.play(0.01);
        const auto frame = session.produce();
        ASSERT_TRUE(frame) << "no frame";
        ASSERT_TRUE(frame->has(FrameFlag::kCompared));
        ASSERT_EQ(frame->laneCount, 3);
        const std::size_t at = std::size_t{frame->laneCount} * traceLength(*frame) +
                               std::size_t{1} * protocol::kMatchValues;
        EXPECT_EQ(frame->data.at(at + 1), static_cast<float>(kDelay)) << "offset";
        EXPECT_EQ(frame->data.at(at + 2), 1.0f) << "related, in polarity";
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, ZoomedMatch, ::testing::ValuesIn(zoomedCompareCases()),
                         modeTestName);

// The Trigger and Free time bases draw a new frame only when something they show can have changed:
// new audio on any track shown, a setting, the view. The page asks for frames at display rate, so
// with the transport stopped, or a host that has stopped processing, every frame drawn would be the
// last one again. In every display mode: in Trigger with each trigger mode that keeps triggering
// (Single holds the one trace it caught), and in Free at a short and a long span.
struct IdleCase
{
    Display mode;
    TimeBase timeBase;
    protocol::TriggerMode trigger = protocol::TriggerMode::kAuto;
    double span;
};

std::vector<IdleCase> idleCases()
{
    std::vector<IdleCase> cases;
    for (const Display mode : everyMode()) {
        for (std::size_t trigger = 0; trigger < protocol::kTriggerModeCount; ++trigger) {
            if (static_cast<protocol::TriggerMode>(trigger) == protocol::TriggerMode::kSingle) {
                continue;
            }
            cases.push_back({.mode = mode,
                             .timeBase = TimeBase::kTrigger,
                             .trigger = static_cast<protocol::TriggerMode>(trigger),
                             .span = 0.2});
        }
        for (const double span : {0.2, 2.0}) {
            cases.push_back({.mode = mode, .timeBase = TimeBase::kFree, .span = span});
        }
    }
    return cases;
}

class SecondsIdle : public ::testing::TestWithParam<IdleCase>
{
};

TEST_P(SecondsIdle, DrawsNothingNewWithoutNewAudio)
{
    const IdleCase c = GetParam();
    Session session(48000.0, 2);
    // Tones around 0, so that the trigger at level 0 finds their edges. Two other tracks, both
    // compared in Compare, so that a search can wait for audio to space it by.
    session.join({.frequency = 1000.0, .offset = 0.0});
    session.join({.frequency = 1500.0, .offset = 0.0});
    session.join({.frequency = 2000.0, .offset = 0.0});
    configure(session.own(), c.mode, c.timeBase);
    session.own().settings().set(SettingId::kTriggerMode, static_cast<double>(c.trigger));
    setSpan(session.own(), c.timeBase, c.span);
    std::vector<std::uint64_t> others;
    for (const Member& member : session.registry.members()) {
        if (member.engine.get() != &session.own()) {
            others.push_back(member.serial);
        }
    }
    session.analyzer().setShown(others);
    session.play(3.0, 0.05);
    (void)session.produce(); // The last of the audio played.

    std::vector<std::byte> bytes;
    for (int request = 0; request < 20; ++request) {
        EXPECT_FALSE(session.analyzer().produce(bytes)) << "request " << request;
    }
    session.own().settings().set(SettingId::kVerticalScale, -6.0);
    EXPECT_TRUE(session.produce()) << "a setting changed";
    EXPECT_FALSE(session.analyzer().produce(bytes)) << "and nothing since";
    // A zoom draws once; in Compare it also makes the matches due, which with no audio to space
    // their searches by must not keep drawing.
    session.analyzer().setView(0.25, 0.75);
    EXPECT_TRUE(session.produce()) << "the view changed";
    for (int request = 0; request < 20; ++request) {
        EXPECT_FALSE(session.analyzer().produce(bytes)) << "after the zoom, request " << request;
    }
    // A window that ends at the newest audio shows a track processed after this one fill in.
    if (isMulti(c.mode) && c.timeBase == TimeBase::kFree) {
        session.lead(1, 480);
        EXPECT_TRUE(session.produce()) << "another track's newest audio, arriving after this one's";
    }
    // A track that turns mono shows other views, here or in its lane, with no new audio.
    std::shared_ptr<Engine> other;
    for (const Member& member : session.registry.members()) {
        if (member.engine.get() != &session.own()) {
            other = member.engine;
        }
    }
    session.own().setInputChannels(1);
    EXPECT_TRUE(session.produce()) << "this track turned mono";
    EXPECT_FALSE(session.analyzer().produce(bytes)) << "and nothing since";
    if (isMulti(c.mode)) {
        other->setInputChannels(1);
        EXPECT_TRUE(session.produce()) << "another track turned mono";
    }
    session.play(0.05);
    EXPECT_TRUE(session.produce()) << "new audio";
    // A new sample rate whose capture has the same size keeps the audio, and changes its times.
    ASSERT_FALSE(session.analyzer().produce(bytes));
    session.own().setSampleRate(44100.0);
    EXPECT_TRUE(session.produce()) << "a new sample rate";
}

INSTANTIATE_TEST_SUITE_P(EveryMode, SecondsIdle, ::testing::ValuesIn(idleCases()),
                         [](const ::testing::TestParamInfo<IdleCase>& info) {
                             const IdleCase& c = info.param;
                             const auto trigger = static_cast<std::size_t>(c.trigger);
                             return modeName(c.mode) + "_" + timeBaseName(c.timeBase) + "_" +
                                    (c.timeBase == TimeBase::kFree
                                         ? std::string(c.span > 1.0 ? "Long" : "Short")
                                         : capitalised(protocol::kTriggerModeNames.at(trigger)));
                         });

// A page that has just loaded, or loaded again after its process ended, has no frame: asked to,
// the engine draws the last frame again although nothing changed, live and frozen, in every
// display mode and time base.
class Redraw : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(Redraw, DrawsTheFrameAgainForANewPage)
{
    const ModeCase c = GetParam();
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        Session session(c.rate, c.channels);
        session.join({.frequency = 1000.0, .offset = 0.0});
        session.join({.frequency = 1500.0, .offset = 0.0});
        configure(session.own(), c.mode, c.timeBase);
        session.play(2.0, 0.05);
        session.analyzer().setFrozen(frozen);
        (void)session.produce();
        std::vector<std::byte> bytes;
        ASSERT_FALSE(session.analyzer().produce(bytes)) << "nothing has changed";
        session.analyzer().redraw();
        EXPECT_TRUE(session.analyzer().produce(bytes)) << "asked to draw again";
        EXPECT_FALSE(session.analyzer().produce(bytes)) << "once";
    }
}

std::vector<ModeCase> redrawCases()
{
    std::vector<ModeCase> cases;
    for (const Display mode : everyMode()) {
        for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
            cases.push_back({.rate = 48000.0,
                             .mode = mode,
                             .channels = 2,
                             .timeBase = static_cast<TimeBase>(timeBase)});
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EveryMode, Redraw, ::testing::ValuesIn(redrawCases()), modeTestName);

// With no new audio, as when the host has stopped, a track that turns mono or a new sample rate
// whose capture keeps its size still changes what a live frame shows (its views, its times): in
// every display mode and every time base, each draws a frame.
class FormatChange : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(FormatChange, DrawsAFrameForANewFormatWithoutNewAudio)
{
    const ModeCase c = GetParam();
    Session session(c.rate, c.channels);
    session.join({.frequency = 1000.0, .offset = 0.0}); // This track.
    session.join({.frequency = 1500.0, .offset = 0.0});
    configure(session.own(), c.mode, c.timeBase);
    session.play(2.0, 0.05);
    // Until any match search in progress (spread over frames) has ended.
    for (int frame = 0; frame < 64 && session.produce(); ++frame) {
    }
    std::vector<std::byte> bytes;
    ASSERT_FALSE(session.analyzer().produce(bytes)) << "nothing has changed";

    session.own().setInputChannels(1);
    EXPECT_TRUE(session.produce()) << "this track turned mono";
    EXPECT_FALSE(session.analyzer().produce(bytes)) << "and nothing since";
    if (isMulti(c.mode)) {
        for (const Member& member : session.registry.members()) {
            if (member.engine.get() != &session.own()) {
                member.engine->setInputChannels(1);
            }
        }
        EXPECT_TRUE(session.produce()) << "another track turned mono";
        EXPECT_FALSE(session.analyzer().produce(bytes)) << "and nothing since";
    }
    // Another rate, whose capture keeps its size: the host changed it without preparing again.
    const double otherRate = c.rate == 44100.0 ? 48000.0 : 44100.0;
    if (c.timeBase == TimeBase::kTrigger && !isMulti(c.mode) && otherRate > c.rate) {
        // A triggered window held across a higher rate keeps its start, and its span in samples
        // then reaches past the newest audio: no frame is drawn, and the page keeps the last one
        // until audio arrives. An accepted limit of QS-12 (docs/DESIGN.md, 6.5): only a host
        // sending a new rate while active gets here, and a compliant one prepares again.
        GTEST_SKIP() << "a held triggered window at a higher rate keeps its last frame (6.5)";
    }
    session.own().setSampleRate(otherRate);
    const auto newRate = session.produce();
    ASSERT_TRUE(newRate) << "a new sample rate";
    EXPECT_EQ(newRate->sampleRate, static_cast<float>(otherRate))
        << "the page times it at the new rate";
    EXPECT_FALSE(session.analyzer().produce(bytes)) << "and nothing since";
    if (isMulti(c.mode)) {
        for (const Member& member : session.registry.members()) {
            if (member.engine.get() != &session.own()) {
                member.engine->setSampleRate(otherRate);
            }
        }
        EXPECT_TRUE(session.produce()) << "another track's new sample rate";
        EXPECT_FALSE(session.analyzer().produce(bytes)) << "and nothing since";
    }

    // Frozen, the picture is the audio held, placed at the rate it was recorded at: a format
    // changing meanwhile changes nothing shown, and draws no frame (see FrozenRateChange).
    session.analyzer().setFrozen(true);
    (void)session.produce();
    ASSERT_FALSE(session.analyzer().produce(bytes)) << "frozen, nothing has changed";
    session.own().setInputChannels(2);
    EXPECT_FALSE(session.analyzer().produce(bytes)) << "frozen, a track's channels changed";
    session.own().setSampleRate(c.rate);
    EXPECT_FALSE(session.analyzer().produce(bytes)) << "frozen, a track's sample rate changed";
    for (int request = 0; request < 5; ++request) {
        EXPECT_FALSE(session.analyzer().produce(bytes)) << "frozen, request " << request;
    }
}

// Every display mode and time base, at the lowest and highest sample rates hosts run at.
std::vector<ModeCase> formatChangeCases()
{
    std::vector<ModeCase> cases;
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        for (const Display mode : everyMode()) {
            for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
                cases.push_back({.rate = rate,
                                 .mode = mode,
                                 .channels = 2,
                                 .timeBase = static_cast<TimeBase>(timeBase)});
            }
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, FormatChange, ::testing::ValuesIn(formatChangeCases()),
                         modeTestName);

// Frozen, the picture is the audio held, recorded at the rate of its moment: the host changing the
// rate meanwhile, with no new audio, leaves it and its times as they were, in every display mode
// and every time base. Drawn again (as for a page that has just loaded), the frame is the same,
// whether the capture keeps its size or the tracks are prepared again for the new rate.
class FrozenRateChange : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(FrozenRateChange, KeepsTheHeldPictureAndItsTimes)
{
    const ModeCase c = GetParam();
    const double otherRate = c.rate == kSampleRates.front() ? kSampleRates.back() : 48000.0;
    for (const bool prepared : {false, true}) {
        SCOPED_TRACE(prepared ? "prepared again" : "same capture size");
        Session session(c.rate, c.channels);
        session.join({.frequency = 1000.0, .offset = 0.0}); // This track.
        session.join({.frequency = 1500.0, .offset = 0.0});
        configure(session.own(), c.mode, c.timeBase);
        session.play(1.9);
        session.play(0.1, 0.05);
        session.analyzer().setFrozen(true);
        (void)session.produce();
        std::vector<std::byte> before;
        session.analyzer().redraw();
        ASSERT_TRUE(session.analyzer().produce(before)) << "no frozen frame";

        if (prepared) {
            session.prepare(otherRate);
        } else {
            for (const Member& member : session.registry.members()) {
                member.engine->setSampleRate(otherRate);
            }
        }
        std::vector<std::byte> after;
        session.analyzer().redraw();
        ASSERT_TRUE(session.analyzer().produce(after)) << "no frame drawn again";
        EXPECT_EQ(after, before) << "the held picture or its times changed with the rate";
        EXPECT_EQ(decode(after).sampleRate, static_cast<float>(c.rate))
            << "the page times the held audio at the rate it was recorded at";
    }
}

// And through other spans chosen while frozen after the rate changed: a shorter one cropped from
// the sweep, a longer one rebuilt from the held audio (in Free still filling in, as more than the
// 2 s played), the shorter one again beside it, and the span frozen on: in Trigger and Free, half
// the longest span, then a quarter and the longest. Each frame is the one the same view draws with
// no rate change.
TEST_P(FrozenRateChange, KeepsTheHeldPictureThroughOtherSpans)
{
    const ModeCase c = GetParam();
    const double otherRate = c.rate == kSampleRates.front() ? kSampleRates.back() : 48000.0;
    const bool beats = c.timeBase == TimeBase::kBeats;
    const SettingId spanId = beats ? SettingId::kBeatSpan : spanSetting(c.timeBase);
    const auto& span = setting(spanId);
    const double frozenOn = beats ? static_cast<double>(BeatSpan::kOneBar) : span.maximum / 2.0;
    const double shorter = beats ? frozenOn - 1.0 : std::max(frozenOn / 2.0, span.minimum);
    const double longer = beats ? frozenOn + 1.0 : std::min(frozenOn * 2.0, span.maximum);
    ASSERT_LT(shorter, frozenOn);
    ASSERT_GT(longer, frozenOn);

    enum class Change : std::uint8_t
    {
        kNone,
        kSameCaptureSize,
        kPreparedAgain,
    };
    // The frames drawn for each span in turn, frozen after 2 s, with the rate changed as `change`.
    const auto framesFor = [&](Change change) {
        Session session(c.rate, c.channels);
        session.join({.frequency = 1000.0, .offset = 0.0}); // This track.
        session.join({.frequency = 1500.0, .offset = 0.0});
        configure(session.own(), c.mode, c.timeBase);
        session.own().settings().set(spanId, frozenOn);
        session.play(1.9);
        session.play(0.1, 0.05);
        session.analyzer().setFrozen(true);
        (void)session.produce();
        if (change == Change::kPreparedAgain) {
            session.prepare(otherRate);
        } else if (change == Change::kSameCaptureSize) {
            for (const Member& member : session.registry.members()) {
                member.engine->setSampleRate(otherRate);
            }
        }
        std::vector<std::vector<std::byte>> frames;
        for (const double value : {shorter, longer, shorter, frozenOn}) {
            session.own().settings().set(spanId, value);
            std::vector<std::byte> frame;
            for (int attempt = 0; attempt < 4 && !session.analyzer().produce(frame); ++attempt) {
            }
            frames.push_back(frame);
        }
        return frames;
    };
    const auto unchanged = framesFor(Change::kNone);
    for (const std::vector<std::byte>& frame : unchanged) {
        ASSERT_FALSE(frame.empty()) << "no frame for a span chosen while frozen";
    }
    for (const Change change : {Change::kSameCaptureSize, Change::kPreparedAgain}) {
        SCOPED_TRACE(change == Change::kPreparedAgain ? "prepared again" : "same capture size");
        const auto changed = framesFor(change);
        ASSERT_EQ(changed.size(), unchanged.size());
        for (std::size_t step = 0; step < changed.size(); ++step) {
            EXPECT_EQ(changed[step], unchanged[step]) << "span " << step << " in turn";
        }
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, FrozenRateChange, ::testing::ValuesIn(formatChangeCases()),
                         modeTestName);

// Frozen in Single, then Multi or Compare: the lanes are first held when Multi comes on. A track
// whose lane is first held after the host changed the rate holds audio at the new rate, while the
// view shows this track's at the rate it was held at: the lane draws none of it, in every time
// base, rather than a stretch of other length or place. With no rate change, the same lane draws
// its audio.
class FrozenLaneAtAnotherRate : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(FrozenLaneAtAnotherRate, DrawsNoAudio)
{
    const ModeCase c = GetParam();
    const double otherRate = c.rate == kSampleRates.front() ? kSampleRates.back() : 48000.0;
    const bool beats = c.timeBase == TimeBase::kBeats;
    // The lane of the last track, in the frame drawn once Multi shows it.
    const auto lateLane = [&](bool rateChanged) {
        Session session(c.rate, c.channels);
        session.join({.frequency = 1000.0}); // This track.
        session.join({.frequency = 1500.0});
        const auto late = session.join({.frequency = 2000.0});
        configure(session.own(), Display::kSingle, c.timeBase);
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
        session.play(3.9);
        session.play(0.1, 0.05);
        session.analyzer().setFrozen(true);
        (void)session.produce();
        if (rateChanged) {
            late->setSampleRate(otherRate);
        }
        show(session.own().settings(), c.mode);
        if (beats) {
            // A lane comes into a frozen sweep when it is rebuilt from the held audio.
            session.own().settings().set(SettingId::kBeatSpan,
                                         static_cast<double>(BeatSpan::kTwoBars));
        }
        const auto frame = session.produce();
        EXPECT_TRUE(frame) << "no frame with the track shown";
        EXPECT_TRUE(frame && frame->laneCount >= 3) << "the track has no lane";
        return frame && frame->laneCount >= 3
                   ? std::vector<float>(trace(*frame, 2).begin(), trace(*frame, 2).end())
                   : std::vector<float>{};
    };
    const auto none = [](const std::vector<float>& values) {
        return std::ranges::all_of(values,
                                   [](float value) { return value == 0.0f || std::isnan(value); });
    };
    const auto unchanged = lateLane(false);
    ASSERT_FALSE(unchanged.empty());
    EXPECT_FALSE(none(unchanged)) << "with no rate change, the lane draws its audio";
    const auto changed = lateLane(true);
    ASSERT_FALSE(changed.empty());
    EXPECT_TRUE(none(changed)) << "a lane held at another rate draws audio at this one's";
}

// Multi and Compare, every time base, at the lowest and highest rates.
std::vector<ModeCase> laneRateCases()
{
    std::vector<ModeCase> cases;
    for (const ModeCase& c : formatChangeCases()) {
        if (isMulti(c.mode)) {
            cases.push_back(c);
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, FrozenLaneAtAnotherRate, ::testing::ValuesIn(laneRateCases()),
                         modeTestName);

// --- Deep zoom: the wave between the samples ------------------------------------------------

// Where a deep view sits in its span: at the start, in the middle, or at the end.
enum class Place : std::uint8_t
{
    kStart,
    kMiddle,
    kEnd,
};

struct DeepZoomCase
{
    double rate = 48000.0;
    TimeBase timeBase = TimeBase::kBeats;
    Display mode = Display::kSingle;
    bool frozen = false;
    bool longest = false; // The time base's longest span, else its shortest.
    Place place = Place::kMiddle;
    int channels = 2;
};

std::string nameOf(const DeepZoomCase& c)
{
    constexpr std::array kPlaces{"Start", "Middle", "End"};
    return rateName(c.rate) + "_" + timeBaseName(c.timeBase) + "_" + modeName(c.mode) + "_" +
           (c.frozen ? "Frozen" : "Live") + "_" + (c.longest ? "Longest" : "Shortest") + "_" +
           kPlaces.at(static_cast<std::size_t>(c.place)) + "_" + channelsName(c.channels);
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const DeepZoomCase& c, std::ostream* out)
{
    *out << nameOf(c);
}

// The lowest and highest rates, every time base, display mode, live and frozen, the shortest and
// longest spans, and the view's place; channels alternate.
std::vector<DeepZoomCase> deepZoomCases()
{
    std::vector<DeepZoomCase> cases;
    int alternate = 0;
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        for (const TimeBase timeBase : {TimeBase::kBeats, TimeBase::kTrigger, TimeBase::kFree}) {
            for (const Display mode : everyMode()) {
                for (const bool frozen : {false, true}) {
                    for (const bool longest : {false, true}) {
                        for (const Place place : {Place::kStart, Place::kMiddle, Place::kEnd}) {
                            cases.push_back({.rate = rate,
                                             .timeBase = timeBase,
                                             .mode = mode,
                                             .frozen = frozen,
                                             .longest = longest,
                                             .place = place,
                                             .channels = (alternate++ % 2) == 0 ? 2 : 1});
                        }
                    }
                }
            }
        }
    }
    return cases;
}

// The two tones the tracks play, as shares of the rate: below 0.4 of it, the range the help says
// the drawn wave follows.
constexpr double kOwnTone = 0.11;
constexpr double kOtherTone = 0.07;

// Least squares of `positions` (in samples) and `values` on a constant and the two tones' sines
// and cosines: every trace here, a view, a lane or a sum, is such a mix.
struct ToneFit
{
    std::array<double, 5> weights{};

    [[nodiscard]] double at(double position) const
    {
        const std::array<double, 5> basis = basisAt(position);
        double value = 0.0;
        for (std::size_t i = 0; i < basis.size(); ++i) {
            value += weights.at(i) * basis.at(i);
        }
        return value;
    }

    static std::array<double, 5> basisAt(double position)
    {
        const double own = 2.0 * std::numbers::pi * kOwnTone * position;
        const double other = 2.0 * std::numbers::pi * kOtherTone * position;
        return {1.0, std::sin(own), std::cos(own), std::sin(other), std::cos(other)};
    }

    static ToneFit of(std::span<const double> positions, std::span<const double> values)
    {
        constexpr std::size_t kTerms = 5;
        std::array<std::array<double, kTerms + 1>, kTerms> system{};
        for (std::size_t k = 0; k < positions.size(); ++k) {
            const std::array<double, kTerms> basis = basisAt(positions[k]);
            for (std::size_t i = 0; i < kTerms; ++i) {
                for (std::size_t j = 0; j < kTerms; ++j) {
                    system.at(i).at(j) += basis.at(i) * basis.at(j);
                }
                system.at(i).at(kTerms) += basis.at(i) * values[k];
            }
        }
        for (std::size_t column = 0; column < kTerms; ++column) {
            std::size_t pivot = column;
            for (std::size_t row = column + 1; row < kTerms; ++row) {
                if (std::abs(system.at(row).at(column)) > std::abs(system.at(pivot).at(column))) {
                    pivot = row;
                }
            }
            std::swap(system.at(column), system.at(pivot));
            for (std::size_t row = 0; row < kTerms; ++row) {
                if (row == column) {
                    continue;
                }
                const double ratio = system.at(row).at(column) / system.at(column).at(column);
                for (std::size_t j = column; j <= kTerms; ++j) {
                    system.at(row).at(j) -= ratio * system.at(column).at(j);
                }
            }
        }
        ToneFit fit;
        for (std::size_t i = 0; i < kTerms; ++i) {
            fit.weights.at(i) = system.at(i).at(kTerms) / system.at(i).at(i);
        }
        return fit;
    }
};

class DeepZoom : public ::testing::TestWithParam<DeepZoomCase>
{
};

// Zoomed to kMinimumViewSamples, a raw trace carries the wave between its samples: every
// pointsPerSample-th point is a sample, the points between them lie on the tones the tracks play,
// and the first and last reach just past the view's edges (to them at the span's ends). The wave
// reaches the span's ends too, its neighbours read from the audio around the span, live and
// frozen, even on the shortest Trig span (22 samples at 44.1 kHz, too few to zoom): drawn straight
// only where no audio exists past an end (a sweep's ends, and Free's newest sample live). In every
// trace: each view, each lane, and Compare's sum.
TEST_P(DeepZoom, DrawsTheWaveBetweenTheSamplesUpToTheSpansEnds)
{
    const DeepZoomCase c = GetParam();
    Session session(c.rate, c.channels);
    session.join({.frequency = kOwnTone * c.rate});
    session.join({.frequency = kOtherTone * c.rate});
    configure(session.own(), c.mode, c.timeBase);
    double played = 0.0;
    if (c.timeBase == TimeBase::kBeats) {
        const auto span = c.longest ? protocol::kBeatSpanCount - 1 : 0;
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(span));
        played = static_cast<double>(beatSpanSamples(c.rate, 120.0, span)) / c.rate;
    } else {
        const protocol::SettingDescriptor& spans = setting(spanSetting(c.timeBase));
        const double seconds = c.longest ? spans.maximum : spans.minimum;
        setSpan(session.own(), c.timeBase, seconds);
        played = seconds;
    }
    session.play(played + 0.5, protocol::kReadoutIntervalSeconds / 2.0);
    auto frame = session.produce();
    if (!frame) { // The last frame played was the newest: a block more for one.
        session.play(static_cast<double>(kBlock) / c.rate);
        frame = session.produce();
    }
    ASSERT_TRUE(frame) << "no frame";
    if (c.frozen) {
        session.analyzer().setFrozen(true);
        session.play(static_cast<double>(kBlock) / c.rate);
        frame = session.produce();
        ASSERT_TRUE(frame) << "no frozen frame";
    }
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const double width = std::min(1.0, protocol::kMinimumViewSamples / spanSamples);
    const double start = c.place == Place::kStart ? 0.0
                         : c.place == Place::kEnd ? 1.0 - width
                                                  : 0.5 - (width / 2.0);
    if (width < 1.0) {
        session.analyzer().setView(start, std::min(1.0, start + width));
        frame = session.produce();
    } else if (!c.frozen) { // Not zoomed: the next frame as it is (frozen, the one above).
        session.play(2.0 * static_cast<double>(kBlock) / c.rate);
        frame = session.produce();
    }
    ASSERT_TRUE(frame) << "no frame at the deep view";
    if (!c.frozen) {
        // Live, a few more frames: a sweep updates the columns in place as it advances.
        for (int step = 0; step < 3; ++step) {
            session.play(2.0 * static_cast<double>(kBlock) / c.rate);
            if (auto next = session.produce()) {
                frame = std::move(next);
            }
        }
    }
    const std::size_t traces = expectedTraces(c.mode, c.channels, 2);
    ASSERT_EQ(tracesIn(*frame), traces);
    ASSERT_EQ(frame->data.size(), announcedValues(*frame));
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    const std::size_t factor = frame->pointsPerSample;
    ASSERT_GT(factor, 1U) << "zoomed in this far, the wave between the samples";
    const auto perSample = static_cast<double>(factor);
    ASSERT_EQ((frame->pointCount - 1) % factor, 0U) << "the last point is a sample";

    // Point k sits k / factor samples after the first sample, which lies startFraction samples
    // before the view's start: at most one sample before it, and the last at most one past its end.
    const double visible = frame->visibleSamples;
    const double first = -frame->startFraction;
    const double last = first + (static_cast<double>(frame->pointCount - 1) / perSample);
    EXPECT_GE(first, -1.0 - 1e-6);
    EXPECT_LE(first, 0.0 + 1e-6);
    // At the span's end the window holds no sample past it: a triggered window of a span that is
    // not a whole number of samples (0.5 ms at 44.1 kHz) ends up to two samples short of it.
    EXPECT_GE(last, (frame->viewEnd >= 1.0 ? visible - 2.0 : visible) - 1e-6);
    EXPECT_LE(last, visible + 1.0 + 1e-6);

    // Where no audio exists past an end, the points may be drawn straight within a reach of it:
    // live, the newest sample, where Free and Auto drawing untriggered end (this session is not
    // paced, and the tones ride above the trigger level; past a paced edge the wave reads on:
    // PacedAnalyzerTests; a triggered trace waits for the audio past it: SingleTriggerDeepZoom).
    // A sweep's ends read round its wrap, the next pass's audio; within a reach of its playhead,
    // which joins the newest pass to the previous one, the points are straight.
    const auto reach = static_cast<double>(kReconstructionReach);
    const bool endHasNoAudio = c.timeBase != TimeBase::kBeats && !c.frozen;
    const double spanEnd = (1.0 - frame->viewStart) / (frame->viewEnd - frame->viewStart) * visible;
    const double playhead =
        frame->cursor >= 0.0f
            ? (frame->cursor - frame->viewStart) / (frame->viewEnd - frame->viewStart) * visible
            : std::numeric_limits<double>::quiet_NaN();
    const auto exempt = [&](double position) {
        return (endHasNoAudio && position > spanEnd - reach - 1.0) ||
               std::abs(position - playhead) < reach + 1.0;
    };
    for (std::size_t index = 0; index < traces; ++index) {
        SCOPED_TRACE("trace " + std::to_string(index));
        const std::span<const float> values = trace(*frame, index);
        std::vector<double> positions;
        std::vector<double> samples;
        for (std::size_t k = 0; k < values.size(); k += factor) {
            positions.push_back(first + (static_cast<double>(k) / perSample));
            samples.push_back(values[k]);
        }
        const ToneFit fit = ToneFit::of(positions, samples);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            ASSERT_NEAR(samples[n], fit.at(positions[n]), 1e-4)
                << "the samples are the tones' (sample " << n << ")";
        }
        std::size_t checked = 0;
        for (std::size_t k = 0; k < values.size(); ++k) {
            const double position = first + (static_cast<double>(k) / perSample);
            if (k % factor == 0 || exempt(position)) {
                continue;
            }
            ++checked;
            EXPECT_NEAR(values[k], fit.at(position), 1e-3)
                << "point " << k << " of " << values.size();
        }
        if (c.place == Place::kMiddle && spanSamples > 4.0 * reach) {
            EXPECT_GT(checked, values.size() / 2) << "most of a view in the middle is the wave";
        }
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, DeepZoom, ::testing::ValuesIn(deepZoomCases()),
                         [](const ::testing::TestParamInfo<DeepZoomCase>& info) {
                             return nameOf(info.param);
                         });

class SingleTriggerDeepZoom : public ::testing::TestWithParam<double>
{
};

// Single freezes the trace it triggered on at once: the audio past its span's end that the wave
// reads there had arrived before the trigger was shown, so the end of a 1 ms span zoomed in is
// the wave, not lines, for as long as it stays frozen.
TEST_P(SingleTriggerDeepZoom, FreezesATraceWhoseEndIsTheWave)
{
    const double rate = GetParam();
    Session session(rate, 2);
    session.join({.frequency = kOwnTone * rate, .offset = 0.0}); // Crossing the trigger level.
    configure(session.own(), Display::kSingle, TimeBase::kTrigger);
    setSpan(session.own(), TimeBase::kTrigger, 0.001);
    session.own().settings().set(SettingId::kTriggerMode,
                                 static_cast<double>(protocol::TriggerMode::kSingle));
    session.play(0.5); // No frame asked for while it plays: the next one triggers and freezes.
    auto frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->has(FrameFlag::kTriggered)) << "Single captured a triggered trace";
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const double width = protocol::kMinimumViewSamples / spanSamples;
    session.analyzer().setView(1.0 - width, 1.0);
    frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->has(FrameFlag::kFrozen)) << "and froze it";
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    const std::size_t factor = frame->pointsPerSample;
    ASSERT_GT(factor, 1U);
    const auto perSample = static_cast<double>(factor);
    const double first = -frame->startFraction;
    for (std::size_t index = 0; index < tracesIn(*frame); ++index) {
        SCOPED_TRACE("trace " + std::to_string(index));
        const std::span<const float> values = trace(*frame, index);
        std::vector<double> positions;
        std::vector<double> samples;
        for (std::size_t k = 0; k < values.size(); k += factor) {
            positions.push_back(first + (static_cast<double>(k) / perSample));
            samples.push_back(values[k]);
        }
        const ToneFit fit = ToneFit::of(positions, samples);
        for (std::size_t k = 0; k < values.size(); ++k) {
            if (k % factor != 0) {
                EXPECT_NEAR(values[k], fit.at(first + (static_cast<double>(k) / perSample)), 1e-3)
                    << "point " << k << " of " << values.size();
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Rates, SingleTriggerDeepZoom,
                         ::testing::Values(kSampleRates.front(), kSampleRates.back()),
                         [](const ::testing::TestParamInfo<double>& info) {
                             return rateName(info.param);
                         });

class RawCrossing : public ::testing::TestWithParam<DeepZoomCase>
{
};

// Where zoom crosses from min/max columns to raw samples, about a sample a column, a raw frame
// still draws the wave between its samples: drawn straight, a crest between two samples would
// stand lower than in the columns either side of the crossing (2.4 dB at 10 kHz at 44.1 kHz).
// Views from a little wider than the columns to well within them, at the span's start, middle and
// end, where a view is clamped to the window and can show as many samples as it is wide. A view
// at least two samples narrower than the columns is raw: it shows at most two samples more.
TEST_P(RawCrossing, NeverDrawsARawFrameStraight)
{
    const DeepZoomCase c = GetParam();
    Session session(c.rate, c.channels);
    session.join({.frequency = kOwnTone * c.rate});
    session.join({.frequency = kOtherTone * c.rate});
    configure(session.own(), c.mode, c.timeBase);
    double played = 0.0;
    if (c.timeBase == TimeBase::kBeats) {
        const auto span = protocol::kBeatSpanCount - 1;
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(span));
        played = static_cast<double>(beatSpanSamples(c.rate, 120.0, span)) / c.rate;
    } else {
        const protocol::SettingDescriptor& spans = setting(spanSetting(c.timeBase));
        setSpan(session.own(), c.timeBase, spans.maximum);
        played = spans.maximum;
    }
    session.play(played + 0.5, protocol::kReadoutIntervalSeconds / 2.0);
    auto frame = session.produce();
    if (!frame) {
        session.play(static_cast<double>(kBlock) / c.rate);
        frame = session.produce();
    }
    ASSERT_TRUE(frame) << "no frame";
    if (c.frozen) {
        session.analyzer().setFrozen(true);
        session.play(static_cast<double>(kBlock) / c.rate);
        frame = session.produce();
        ASSERT_TRUE(frame) << "no frozen frame";
    }
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const auto columns = static_cast<double>(kColumns);
    std::size_t raw = 0;
    for (int step = 0; step <= 58; ++step) {
        const double samples = columns - 40.0 + (0.73 * step);
        SCOPED_TRACE(std::to_string(samples) + " samples");
        const double width = samples / spanSamples;
        const double start = c.place == Place::kStart ? 0.0
                             : c.place == Place::kEnd ? 1.0 - width
                                                      : 0.5 - (width / 2.0);
        session.analyzer().setView(start, start + width);
        if (!c.frozen) {
            session.play(static_cast<double>(kBlock) / c.rate);
        }
        frame = session.produce();
        ASSERT_TRUE(frame) << "no frame";
        if (frame->mode == FrameMode::kRaw) {
            ++raw;
            EXPECT_GE(frame->pointsPerSample, protocol::kMinPointsPerSample)
                << "a raw frame draws the wave between its samples";
        } else {
            EXPECT_GT(samples, columns - 2.0) << "a view this narrow is raw";
        }
    }
    EXPECT_GT(raw, 0U);
}

std::vector<DeepZoomCase> rawCrossingCases()
{
    std::vector<DeepZoomCase> cases;
    int alternate = 0;
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        for (const TimeBase timeBase : {TimeBase::kBeats, TimeBase::kTrigger, TimeBase::kFree}) {
            for (const Display mode : everyMode()) {
                for (const bool frozen : {false, true}) {
                    for (const Place place : {Place::kStart, Place::kMiddle, Place::kEnd}) {
                        cases.push_back({.rate = rate,
                                         .timeBase = timeBase,
                                         .mode = mode,
                                         .frozen = frozen,
                                         .longest = true,
                                         .place = place,
                                         .channels = (alternate++ % 2) == 0 ? 2 : 1});
                    }
                }
            }
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, RawCrossing, ::testing::ValuesIn(rawCrossingCases()),
                         [](const ::testing::TestParamInfo<DeepZoomCase>& info) {
                             return nameOf(info.param);
                         });

struct FlatToneCase
{
    double rate = 44100.0;
    double cycles = 10000.0 / 44100.0; // The tone, as a share of the rate.
    TimeBase timeBase = TimeBase::kBeats;
    Display mode = Display::kSingle;
    bool frozen = false;
    int channels = 2;
    Place place = Place::kMiddle;
};

std::string nameOf(const FlatToneCase& c)
{
    constexpr std::array kPlaces{"Start", "Middle", "End"};
    return rateName(c.rate) + "_" + std::to_string(std::lround(c.cycles * c.rate)) + "Hz_" +
           timeBaseName(c.timeBase) + "_" + modeName(c.mode) + "_" +
           (c.frozen ? "Frozen" : "Live") + "_" + channelsName(c.channels) + "_" +
           kPlaces.at(static_cast<std::size_t>(c.place));
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const FlatToneCase& c, std::ostream* out)
{
    *out << nameOf(c);
}

class FlatTone : public ::testing::TestWithParam<FlatToneCase>
{
};

// A steady tone draws a flat top in every trace (each view, each lane and Compare's sum), in every
// display mode and time base, live and frozen, mono and stereo, at the span's start, middle and
// end: its min/max columns take their height from the wave
// between the samples, so they do not ripple as the samples fall nearer or further from each crest
// (10 kHz at 44.1 kHz rippled by 2.4 dB, a quarter of the rate at 45 degrees read 3 dB low in every
// column). Zoomed on to the raw samples, just past the crossing, the crests stand within the lines'
// own under-read of the columns' height.
TEST_P(FlatTone, DrawsAFlatTopAtEveryZoom)
{
    const FlatToneCase c = GetParam();
    Session session(c.rate, c.channels);
    const double frequency = c.cycles * c.rate;
    session.join({.frequency = frequency});
    session.join({.frequency = frequency});
    configure(session.own(), c.mode, c.timeBase);
    double played = 0.0;
    if (c.timeBase == TimeBase::kBeats) {
        session.own().settings().set(SettingId::kBeatSpan, 0.0);
        played = static_cast<double>(beatSpanSamples(c.rate, 120.0, 0)) / c.rate;
    } else {
        setSpan(session.own(), c.timeBase, 0.1);
        played = 0.1;
    }
    session.play(played + 0.5, protocol::kReadoutIntervalSeconds / 2.0);
    auto frame = session.produce();
    if (!frame) {
        session.play(static_cast<double>(kBlock) / c.rate);
        frame = session.produce();
    }
    ASSERT_TRUE(frame) << "no frame";
    if (c.frozen) {
        session.analyzer().setFrozen(true);
        session.play(static_cast<double>(kBlock) / c.rate);
        frame = session.produce();
        ASSERT_TRUE(frame) << "no frozen frame";
    }
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const auto columns = static_cast<double>(kColumns);
    // The tops of a frame's traces: per trace, the highest and lowest column top away from the
    // view's ends, or the highest raw point.
    const auto viewAt = [&](double samples) {
        const double width = samples / spanSamples;
        const double start = c.place == Place::kStart ? 0.0
                             : c.place == Place::kEnd ? 1.0 - width
                                                      : 0.5 - (width / 2.0);
        session.analyzer().setView(start, start + width);
        if (!c.frozen) {
            session.play(static_cast<double>(kBlock) / c.rate);
        }
        auto shown = session.produce();
        if (!shown && !c.frozen) {
            session.play(static_cast<double>(kBlock) / c.rate);
            shown = session.produce();
        }
        return shown;
    };
    const auto columnFrame = viewAt(8.0 * columns);
    ASSERT_TRUE(columnFrame);
    ASSERT_EQ(columnFrame->mode, FrameMode::kMinMax);
    const auto rawFrame = viewAt(0.9 * columns);
    ASSERT_TRUE(rawFrame);
    ASSERT_EQ(rawFrame->mode, FrameMode::kRaw);
    ASSERT_EQ(tracesIn(*columnFrame), tracesIn(*rawFrame));
    // The step's own error at this tone and the 4-point lines' (DESIGN.md, AD-32).
    const double ripple = 1.0 - std::pow(10.0, -0.05 / 20.0);
    const double lines = 1.0 - std::cos(std::numbers::pi * c.cycles / 4.0);
    for (std::size_t index = 0; index < tracesIn(*columnFrame); ++index) {
        SCOPED_TRACE("trace " + std::to_string(index));
        const std::span<const float> values = trace(*columnFrame, index);
        double highest = -std::numeric_limits<double>::infinity();
        double lowest = std::numeric_limits<double>::infinity();
        double floorHighest = -std::numeric_limits<double>::infinity();
        // A sweep's playhead joins the newest pass to the one before: no wave across it, so the
        // columns beside it read their samples (and are held to the bounds below only).
        const auto count = static_cast<double>(values.size()) / 2.0;
        const double playhead = columnFrame->cursor >= 0.0f
                                    ? (columnFrame->cursor - columnFrame->viewStart) /
                                          (columnFrame->viewEnd - columnFrame->viewStart) * count
                                    : -100.0;
        for (std::size_t column = 4; column + 4 < values.size() / 2; ++column) {
            if (std::abs(static_cast<double>(column) - playhead) < 3.0) {
                continue;
            }
            highest = std::max(highest, static_cast<double>(values[column * 2 + 1]));
            lowest = std::min(lowest, static_cast<double>(values[column * 2 + 1]));
            floorHighest = std::max(floorHighest, static_cast<double>(values[column * 2]));
        }
        // The tone's swing in this trace, from the columns: top to bottom.
        double bottom = std::numeric_limits<double>::infinity();
        for (std::size_t column = 4; column + 4 < values.size() / 2; ++column) {
            bottom = std::min(bottom, static_cast<double>(values[column * 2]));
        }
        const double amplitude = (highest - bottom) / 2.0;
        if (!(amplitude > 1e-3)) {
            continue; // A sum that cancels, or a silent lane.
        }
        EXPECT_LE(highest - lowest, 2.0 * amplitude * ripple) << "the columns' top ripples";
        // Every column holds a crest (eight samples, at most 4.4 a cycle) and reads it from the
        // wave, at the view's and the span's ends too: its neighbours past the span's ends come
        // from the audio around it, a sweep's from round its wrap. Only beside the playhead, and
        // where no audio exists past an end (live, the newest sample, where Free and Auto drawing
        // untriggered end, this session not being paced and the tone riding above the trigger
        // level), the gaps are straight: a column there reads no higher than the wave and no
        // lower than its nearest sample to a crest.
        const bool endHasNoAudio =
            c.timeBase != TimeBase::kBeats && !c.frozen && c.place == Place::kEnd;
        const std::size_t columnCount = values.size() / 2;
        const auto straightThere = [&](std::size_t column) {
            return (endHasNoAudio && column + 2 >= columnCount) ||
                   std::abs(static_cast<double>(column) - playhead) < 3.0;
        };
        const double straight = 1.0 - std::cos(std::numbers::pi * c.cycles);
        for (std::size_t column = 0; column < columnCount; ++column) {
            const double top = values[column * 2 + 1];
            EXPECT_LE(top, highest + (2.0 * amplitude * ripple)) << "column " << column;
            const double under = straightThere(column) ? amplitude * straight : 0.0;
            EXPECT_GE(top, highest - under - (2.0 * amplitude * ripple))
                << "column " << column << (straightThere(column) ? "" : ", read from the wave");
        }
        const std::span<const float> points = trace(*rawFrame, index);
        double rawTop = -std::numeric_limits<double>::infinity();
        for (const float point : points) {
            if (std::isfinite(point)) {
                rawTop = std::max(rawTop, static_cast<double>(point));
            }
        }
        EXPECT_NEAR(rawTop, highest, amplitude * (lines + 2.0 * ripple))
            << "raw crests and columns meet at the crossing";
    }
}

std::vector<FlatToneCase> flatToneCases()
{
    std::vector<FlatToneCase> cases;
    for (const auto& [rate, cycles] : {std::pair{44100.0, 10000.0 / 44100.0},
                                       std::pair{48000.0, 0.25}, std::pair{384000.0, 1.0 / 3.0}}) {
        for (const TimeBase timeBase : {TimeBase::kBeats, TimeBase::kTrigger, TimeBase::kFree}) {
            for (const Display mode : everyMode()) {
                for (const bool frozen : {false, true}) {
                    for (const int channels : {1, 2}) {
                        for (const Place place : {Place::kStart, Place::kMiddle, Place::kEnd}) {
                            cases.push_back({.rate = rate,
                                             .cycles = cycles,
                                             .timeBase = timeBase,
                                             .mode = mode,
                                             .frozen = frozen,
                                             .channels = channels,
                                             .place = place});
                        }
                    }
                }
            }
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(Tones, FlatTone, ::testing::ValuesIn(flatToneCases()),
                         [](const ::testing::TestParamInfo<FlatToneCase>& info) {
                             return nameOf(info.param);
                         });

// Live in Beats, the playhead joins the newest pass to the previous one: zoomed in on it, the
// points between samples within a reach of it are drawn straight, so neither side rings into the
// other, while farther away the trace is the wave. In every trace of every display mode: views,
// lanes and Compare's sum.
// Live, and frozen on a shorter beat span cropped from the held bar where the playhead is in its
// second half (the part shown starts part way into the held sweep).
struct SeamCase
{
    Display mode = Display::kSingle;
    bool croppedFreeze = false;
};

// Whether the gap after sample n of a raw trace of `factor` points a sample is drawn as a line
// between its two samples rather than as the wave.
bool drawnStraight(std::span<const float> values, std::size_t factor, std::size_t n)
{
    const float from = values[n * factor];
    const float to = values[(n + 1) * factor];
    for (std::size_t phase = 1; phase < factor; ++phase) {
        const float line =
            from + ((to - from) * static_cast<float>(phase) / static_cast<float>(factor));
        if (std::abs(values[(n * factor) + phase] - line) > 1e-5f) {
            return false;
        }
    }
    return true;
}

// The gaps of each raw trace of `frame` within a reach of sample `join` (a position in samples
// from its first drawn sample) are drawn straight, and most gaps further away are the wave.
void expectStraightOnlyAround(const DecodedFrame& frame, double join)
{
    const std::size_t factor = frame.pointsPerSample;
    ASSERT_GT(factor, 1U);
    for (std::size_t index = 0; index < tracesIn(frame); ++index) {
        SCOPED_TRACE("trace " + std::to_string(index));
        const std::span<const float> values = trace(frame, index);
        const std::size_t samples = ((values.size() - 1) / factor) + 1;
        std::size_t curvedFar = 0;
        std::size_t far = 0;
        for (std::size_t n = 0; n + 1 < samples; ++n) {
            const double distance = std::abs((static_cast<double>(n) + 0.5) - join);
            if (distance < static_cast<double>(kReconstructionReach) - 2.0) {
                EXPECT_TRUE(drawnStraight(values, factor, n))
                    << "gap " << n << ", " << distance << " from the join";
            } else if (distance > static_cast<double>(kReconstructionReach) + 2.0) {
                ++far;
                curvedFar += drawnStraight(values, factor, n) ? 0 : 1;
            }
        }
        EXPECT_GT(curvedFar, far / 2) << "away from the join, the wave";
    }
}

class DeepZoomFrozenPart : public ::testing::TestWithParam<Display>
{
};

// Frozen while a bar's first pass is still filling in, a shorter beat span is shown rebuilt from
// the held audio (a part), beside the image's own samples, which are of another layout: no wave
// across either of the part's edges, at the shorter span's start and end. In every display mode,
// with a half and a quarter bar frozen at three quarters of the bar, where the part ends at the
// image's end, which meets the join where it began, and a quarter frozen at three eighths: the
// second quarter's part then ends where nothing else joins, beside image samples not yet written.
// (A half bar frozen there holds no audio near its end: that half's previous pass was never
// played.)
TEST_P(DeepZoomFrozenPart, DrawsStraightAtThePartsEdges)
{
    constexpr double kRate = 48000.0;
    for (const auto [shorter, played] : {std::pair{BeatSpan::kHalfBar, 1.5},
                                         {BeatSpan::kQuarterBar, 1.5},
                                         {BeatSpan::kQuarterBar, 0.75}}) {
        SCOPED_TRACE(::testing::Message()
                     << (shorter == BeatSpan::kHalfBar ? "half a bar" : "a quarter of a bar")
                     << ", frozen after " << played << " s of a bar of 2 s");
        Session session(kRate, 2);
        session.join({.frequency = kOwnTone * kRate});
        session.join({.frequency = kOtherTone * kRate});
        configure(session.own(), GetParam(), TimeBase::kBeats);
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
        session.play(played, protocol::kReadoutIntervalSeconds / 2.0);
        session.play(0.01);
        ASSERT_TRUE(session.produce());
        session.analyzer().setFrozen(true);
        ASSERT_TRUE(session.produce());
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(shorter));
        auto frame = session.produce();
        ASSERT_TRUE(frame) << "no frame at the shorter span";
        const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
        const double width = 4.0 * protocol::kMinimumViewSamples / spanSamples;
        for (const bool atStart : {true, false}) {
            SCOPED_TRACE(atStart ? "the part's start" : "the part's end");
            session.analyzer().setView(atStart ? 0.0 : 1.0 - width, atStart ? width : 1.0);
            frame = session.produce();
            ASSERT_TRUE(frame);
            ASSERT_EQ(frame->mode, FrameMode::kRaw);
            const double offset = frame->startFraction;
            expectStraightOnlyAround(*frame, atStart ? offset : frame->visibleSamples + offset);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(EveryMode, DeepZoomFrozenPart, ::testing::ValuesIn(everyMode()),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

class DeepZoomSeam : public ::testing::TestWithParam<SeamCase>
{
};

TEST_P(DeepZoomSeam, DrawsStraightAcrossThePlayhead)
{
    const SeamCase c = GetParam();
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.join({.frequency = kOwnTone * kRate});
    session.join({.frequency = kOtherTone * kRate});
    configure(session.own(), c.mode, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
    // Three quarters into the second bar at 120 BPM.
    session.play(3.5, protocol::kReadoutIntervalSeconds / 2.0);
    session.play(static_cast<double>(kBlock) / kRate);
    auto frame = session.produce();
    ASSERT_TRUE(frame);
    if (c.croppedFreeze) {
        session.analyzer().setFrozen(true);
        ASSERT_TRUE(session.produce());
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kHalfBar));
        frame = session.produce();
        ASSERT_TRUE(frame) << "no frame at the shorter span";
    }
    ASSERT_GE(frame->cursor, 0.0f);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const double width = 4.0 * protocol::kMinimumViewSamples / spanSamples;
    const double start =
        std::clamp(static_cast<double>(frame->cursor) - (width / 2.0), 0.0, 1.0 - width);
    session.analyzer().setView(start, start + width);
    frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    const std::size_t factor = frame->pointsPerSample;
    ASSERT_GT(factor, 1U);
    const double visible = frame->visibleSamples;
    const double playhead =
        ((frame->cursor - frame->viewStart) / (frame->viewEnd - frame->viewStart) * visible) +
        frame->startFraction;
    ASSERT_GT(playhead, 2.0 * kReconstructionReach) << "the playhead is in the view";
    for (std::size_t index = 0; index < tracesIn(*frame); ++index) {
        SCOPED_TRACE("trace " + std::to_string(index));
        const std::span<const float> values = trace(*frame, index);
        const std::size_t samples = ((values.size() - 1) / factor) + 1;
        ASSERT_LT(playhead, static_cast<double>(samples) - (2.0 * kReconstructionReach));
        const auto straight = [&](std::size_t n) { return drawnStraight(values, factor, n); };
        std::size_t curvedFar = 0;
        for (std::size_t n = 0; n + 1 < samples; ++n) {
            const double distance = std::abs((static_cast<double>(n) + 0.5) - playhead);
            if (distance < static_cast<double>(kReconstructionReach) - 2.0) {
                EXPECT_TRUE(straight(n)) << "gap " << n << ", " << distance << " from the playhead";
            } else if (distance > static_cast<double>(kReconstructionReach) + 2.0 && !straight(n)) {
                ++curvedFar;
            }
        }
        EXPECT_GT(curvedFar, samples / 4) << "away from the playhead, the wave";
    }
}

std::vector<SeamCase> seamCases()
{
    std::vector<SeamCase> cases;
    for (const bool croppedFreeze : {false, true}) {
        for (const Display mode : everyMode()) {
            cases.push_back({.mode = mode, .croppedFreeze = croppedFreeze});
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EveryMode, DeepZoomSeam, ::testing::ValuesIn(seamCases()),
                         [](const ::testing::TestParamInfo<SeamCase>& info) {
                             return modeName(info.param.mode) +
                                    (info.param.croppedFreeze ? "CroppedFreeze" : "Live");
                         });

// Live in Free, other tracks are sure to hold their audio only up to the paced edge (AD-29): a
// track whose blocks come a block later than this one's has none past it yet. Zoomed in on the
// newest samples in Multi and Compare, no lane reads past the edge as silence, so none bends
// towards zero at its end.
class DeepZoomLaggingLanes : public ::testing::TestWithParam<Display>
{
};

TEST_P(DeepZoomLaggingLanes, DoNotBendTowardsSilence)
{
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.paced = true;
    session.join({.frequency = kOwnTone * kRate});
    session.join({.frequency = kOtherTone * kRate});
    configure(session.own(), GetParam(), TimeBase::kFree);
    setSpan(session.own(), TimeBase::kFree, 0.5);
    // Small blocks (64 samples, 1.3 ms), the others' a millisecond later in each cycle: the paced
    // edge trails this track's newest audio by little more than a block.
    constexpr int kBlockSamples = 64;
    constexpr int kLateMillis = 1;
    session.playPaced(3.0, kLateMillis, false, kBlockSamples); // The clock locks.
    const double width = protocol::kMinimumViewSamples / (0.5 * kRate);
    session.analyzer().setView(1.0 - width, 1.0);
    // This track's newest block in, the others' not yet.
    session.playPaced(0.05, kLateMillis, true, kBlockSamples);
    const auto frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_GT(frame->pointsPerSample, 1U);
    // Where a lane holds no audio past the edge, its neighbours there are none: nothing reads as
    // invalid for it.
    EXPECT_EQ(frame->invalidLanes, 0U);
    EXPECT_EQ(frame->invalidSamples, 0U);
    // A lane's Mid rides between 0.1875 and 0.5625 (three quarters of the left channel's 0.5 offset
    // and 0.25 swing; the sum, of two, twice that): a point far below is silence read in.
    for (std::size_t index = 0; index < tracesIn(*frame); ++index) {
        SCOPED_TRACE("lane " + std::to_string(index));
        const std::span<const float> values = trace(*frame, index);
        EXPECT_GT(*std::ranges::min_element(values), 0.15f) << "no silence read past the edge";
    }
}

INSTANTIATE_TEST_SUITE_P(Lanes, DeepZoomLaggingLanes,
                         ::testing::Values(Display::kMulti, Display::kCompare),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

// Where a deep-zoom test runs: a display mode and a time base, frozen there.
struct DeepPath
{
    Display mode = Display::kSingle;
    TimeBase timeBase = TimeBase::kFree;
};

std::string nameOf(const DeepPath& path)
{
    return modeName(path.mode) + "_" + timeBaseName(path.timeBase);
}

// Plays a session of `path` (Beats: one bar; Free: 100 ms) and freezes it.
std::optional<DecodedFrame> frozenSession(Session& session, const DeepPath& path)
{
    configure(session.own(), path.mode, path.timeBase);
    if (path.timeBase == TimeBase::kBeats) {
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
    } else {
        setSpan(session.own(), path.timeBase, 0.1);
    }
    session.play(3.0, protocol::kReadoutIntervalSeconds / 2.0);
    session.analyzer().setFrozen(true);
    session.play(static_cast<double>(kBlock) / session.rate);
    auto frame = session.produce();
    if (!frame) {
        session.analyzer().setColumns(kColumns - 1); // Forces a frame.
        frame = session.produce();
        session.analyzer().setColumns(kColumns);
    }
    return frame;
}

// Which channel of this track holds the broken samples.
enum class BrokenSide : std::uint8_t
{
    kBoth,
    kLeft,
    kRight,
};

struct InvalidCase
{
    DeepPath path;
    BrokenSide channel = BrokenSide::kBoth;
};

std::string nameOf(const InvalidCase& c)
{
    constexpr std::array kChannels{"Both", "Left", "Right"};
    return nameOf(c.path) + "_" + kChannels.at(static_cast<std::size_t>(c.channel));
}

// A sample that is not finite just past either edge of the view is read only as a neighbour: the
// trace is drawn around it and the track is not flagged as holding one, as without the
// reconstruction; just inside either edge it is flagged. On every path that flags: Single's window
// and sweep, Multi's and Compare's lanes in Free; in Single, with the sample broken in one drawn
// view only (L or R) as well as both. A Beats lane flags by 10 ms blocks of its levels (a NaN that
// close to the view flags it already without the reconstruction), so there only the flag inside
// is checked.
class DeepZoomInvalid : public ::testing::TestWithParam<InvalidCase>
{
};

bool hasBroken(std::span<const float> values)
{
    return std::ranges::any_of(values, [](float v) { return std::isnan(v); });
}

bool noneBroken(const DecodedFrame& shown)
{
    for (std::size_t index = 0; index < tracesIn(shown); ++index) {
        if (hasBroken(trace(shown, index))) {
            return false;
        }
    }
    return true;
}

// The place in a frozen span (in samples from its start) of a broken sample in its middle: a raw
// view of 300 samples there, in whichever trace holds one. None if none is found.
std::optional<double> brokenSampleAt(Session& session, double spanSamples)
{
    const double wide = 300.0 / spanSamples;
    session.analyzer().setView(0.5 - (wide / 2.0), 0.5 + (wide / 2.0));
    const auto frame = session.produce();
    if (!frame || frame->mode != FrameMode::kRaw) {
        return std::nullopt;
    }
    std::size_t brokenTrace = 0;
    while (brokenTrace < tracesIn(*frame) && !hasBroken(trace(*frame, brokenTrace))) {
        ++brokenTrace;
    }
    if (brokenTrace == tracesIn(*frame)) {
        return std::nullopt;
    }
    // The broken sample: every pointsPerSample-th point is a sample (the points between it and its
    // neighbours are not finite either).
    const std::span<const float> values = trace(*frame, brokenTrace);
    const std::size_t factor = frame->pointsPerSample;
    std::size_t broken = 0;
    while (broken < values.size() && !std::isnan(values[broken])) {
        ++broken;
    }
    broken = (broken + factor - 1) / factor * factor;
    if (broken >= values.size() || !std::isnan(values[broken])) {
        return std::nullopt;
    }
    return (frame->viewStart * spanSamples) - frame->startFraction +
           (static_cast<double>(broken) / static_cast<double>(factor));
}

TEST_P(DeepZoomInvalid, FlagsOnlyWhatTheViewShows)
{
    const InvalidCase c = GetParam();
    const DeepPath path = c.path;
    constexpr double kRate = 48000.0;
    constexpr std::int64_t kBrokenEvery = 251; // One in every 300-sample view, none near another.
    Session session(kRate, 2);
    session.join({.frequency = kOwnTone * kRate,
                  .brokenEvery = kBrokenEvery,
                  .brokenRightOnly = c.channel == BrokenSide::kRight,
                  .brokenLeftOnly = c.channel == BrokenSide::kLeft});
    session.join({.frequency = kOtherTone * kRate});
    auto frame = frozenSession(session, path);
    ASSERT_TRUE(frame);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const std::optional<double> brokenAt = brokenSampleAt(session, spanSamples);
    ASSERT_TRUE(brokenAt);
    const double at = *brokenAt;
    const double width = protocol::kMinimumViewSamples / spanSamples;
    const auto viewStartingAt = [&](double start) {
        session.analyzer().setView(start / spanSamples, (start / spanSamples) + width);
        auto shown = session.produce();
        EXPECT_TRUE(shown && shown->pointsPerSample > 1U);
        return shown;
    };
    const double samples = protocol::kMinimumViewSamples;

    const bool byBlocks = isMulti(path.mode) && path.timeBase == TimeBase::kBeats;
    if (!byBlocks) {
        frame = viewStartingAt(at - samples - 4.0);
        ASSERT_TRUE(frame);
        EXPECT_TRUE(noneBroken(*frame));
        EXPECT_EQ(frame->invalidLanes & 1U, 0U) << "4 samples past the view's right edge";
        frame = viewStartingAt(at + 4.0);
        ASSERT_TRUE(frame);
        EXPECT_TRUE(noneBroken(*frame));
        EXPECT_EQ(frame->invalidLanes & 1U, 0U) << "4 samples before the view's left edge";
    }

    frame = viewStartingAt(at - samples + 4.0);
    ASSERT_TRUE(frame);
    EXPECT_NE(frame->invalidLanes & 1U, 0U) << "on screen, at its right edge";
    frame = viewStartingAt(at - 4.0);
    ASSERT_TRUE(frame);
    EXPECT_NE(frame->invalidLanes & 1U, 0U) << "on screen, at its left edge";
}

std::vector<InvalidCase> invalidCases()
{
    std::vector<InvalidCase> cases;
    for (const DeepPath path :
         {DeepPath{.mode = Display::kSingle, .timeBase = TimeBase::kFree},
          DeepPath{.mode = Display::kSingle, .timeBase = TimeBase::kBeats},
          DeepPath{.mode = Display::kMulti, .timeBase = TimeBase::kFree},
          DeepPath{.mode = Display::kCompare, .timeBase = TimeBase::kFree},
          DeepPath{.mode = Display::kMulti, .timeBase = TimeBase::kBeats},
          DeepPath{.mode = Display::kCompare, .timeBase = TimeBase::kBeats}}) {
        cases.push_back({.path = path});
        if (!isMulti(path.mode)) {
            cases.push_back({.path = path, .channel = BrokenSide::kLeft});
            cases.push_back({.path = path, .channel = BrokenSide::kRight});
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(Paths, DeepZoomInvalid, ::testing::ValuesIn(invalidCases()),
                         [](const ::testing::TestParamInfo<InvalidCase>& info) {
                             return nameOf(info.param);
                         });

// The frozen Trigger and Free paths of every display mode.
std::vector<DeepPath> frozenWindowPaths()
{
    std::vector<DeepPath> paths;
    for (const TimeBase timeBase : {TimeBase::kTrigger, TimeBase::kFree}) {
        for (const Display mode : everyMode()) {
            paths.push_back({.mode = mode, .timeBase = timeBase});
        }
    }
    return paths;
}

class DeepZoomInvalidPastSpan : public ::testing::TestWithParam<DeepPath>
{
};

// A sample that is not finite a few samples past a frozen span's end, or before its start, is read
// only as a neighbour of the wave at that end: the trace is drawn up to it, not through it, and
// nothing is flagged, neither the Invalid samples chip's count nor any lane, since the screen does
// not show it.
TEST_P(DeepZoomInvalidPastSpan, LeavesItOutOfTheTraceAndTheChip)
{
    const DeepPath path = GetParam();
    constexpr double kRate = 48000.0;
    constexpr std::int64_t kBrokenEvery = 251;
    Session session(kRate, 2);
    session.join({.frequency = kOwnTone * kRate, .brokenEvery = kBrokenEvery});
    session.join({.frequency = kOtherTone * kRate});
    auto frame = frozenSession(session, path);
    ASSERT_TRUE(frame);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const std::optional<double> at = brokenSampleAt(session, spanSamples);
    ASSERT_TRUE(at);
    const double width = protocol::kMinimumViewSamples / spanSamples;
    // Panning back by p samples moves every sample p further into the span: a pan that puts one of
    // the broken samples (one every kBrokenEvery) at `target`, a few samples past an end.
    const auto panTo = [&](double target) {
        const auto every = static_cast<double>(kBrokenEvery);
        const double pan = std::fmod(std::fmod(target - *at, every) + every, every) + every;
        session.analyzer().setPan(pan / kRate);
    };
    for (const bool pastEnd : {true, false}) {
        SCOPED_TRACE(pastEnd ? "past the span's end" : "before the span's start");
        panTo(pastEnd ? spanSamples + 4.5 : -5.5);
        session.analyzer().setView(pastEnd ? 1.0 - width : 0.0, pastEnd ? 1.0 : width);
        frame = session.produce();
        ASSERT_TRUE(frame);
        ASSERT_EQ(frame->mode, FrameMode::kRaw);
        ASSERT_GT(frame->pointsPerSample, 1U);
        EXPECT_TRUE(noneBroken(*frame)) << "drawn up to it, not through it";
        EXPECT_EQ(frame->invalidLanes, 0U);
        EXPECT_EQ(frame->invalidSamples, 0U);
    }
}

INSTANTIATE_TEST_SUITE_P(Paths, DeepZoomInvalidPastSpan, ::testing::ValuesIn(frozenWindowPaths()),
                         [](const ::testing::TestParamInfo<DeepPath>& info) {
                             return nameOf(info.param);
                         });

class DeepZoomAtTheHoldStart : public ::testing::TestWithParam<DeepPath>
{
};

// A frozen view panned back to the oldest audio held, which began with the session: no audio
// exists before it, so the trace starts straight, not ringing towards silence that was never
// played, and nothing reads as invalid.
TEST_P(DeepZoomAtTheHoldStart, StartsStraightWhereNoAudioWasHeld)
{
    const DeepPath path = GetParam();
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.join({.frequency = kOwnTone * kRate});
    session.join({.frequency = kOtherTone * kRate});
    auto frame = frozenSession(session, path);
    ASSERT_TRUE(frame);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    session.analyzer().setPan(10.0); // Further than the 3 s held: as far as the hold reaches.
    session.analyzer().setView(0.0, protocol::kMinimumViewSamples / spanSamples);
    frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    const std::size_t factor = frame->pointsPerSample;
    ASSERT_GT(factor, 1U);
    EXPECT_TRUE(noneBroken(*frame));
    EXPECT_EQ(frame->invalidLanes, 0U);
    EXPECT_EQ(frame->invalidSamples, 0U);
    for (std::size_t index = 0; index < tracesIn(*frame); ++index) {
        SCOPED_TRACE("trace " + std::to_string(index));
        const std::span<const float> values = trace(*frame, index);
        ASSERT_GT(values.size(), factor);
        // The first gap: its points on the line between its two samples.
        for (std::size_t phase = 1; phase < factor; ++phase) {
            const float line = values[0] + ((values[factor] - values[0]) *
                                            static_cast<float>(phase) / static_cast<float>(factor));
            EXPECT_NEAR(values[phase], line, 1e-5f) << "point " << phase;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Paths, DeepZoomAtTheHoldStart, ::testing::ValuesIn(frozenWindowPaths()),
                         [](const ::testing::TestParamInfo<DeepPath>& info) {
                             return nameOf(info.param);
                         });

class DeepZoomNearTheEnds : public ::testing::TestWithParam<double>
{
};

// A view a few samples inside a frozen Trig span's ends, closer than the reach to them: its
// neighbours lie partly inside the span and partly past it, and the wave is drawn at both of the
// view's edges, read from both.
TEST_P(DeepZoomNearTheEnds, DrawsTheWaveFromNeighboursInsideAndPastTheSpan)
{
    const double rate = GetParam();
    Session session(rate, 2);
    session.join({.frequency = kOwnTone * rate});
    auto frame = frozenSession(session, {.mode = Display::kSingle, .timeBase = TimeBase::kTrigger});
    ASSERT_TRUE(frame);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const double width = protocol::kMinimumViewSamples / spanSamples;
    // Samples from the ends: fewer than the reach's 16, so that some neighbours lie past the span
    // and some inside it, and more than the reach but fewer than twice it.
    for (const auto [nearStart, inside] :
         {std::pair{true, 5.0}, {true, 20.0}, {false, 5.0}, {false, 20.0}}) {
        SCOPED_TRACE(::testing::Message()
                     << (nearStart ? "from the span's start: " : "from its end: ") << inside);
        const double start =
            nearStart ? inside / spanSamples : 1.0 - width - (inside / spanSamples);
        session.analyzer().setView(start, start + width);
        frame = session.produce();
        ASSERT_TRUE(frame);
        ASSERT_EQ(frame->mode, FrameMode::kRaw);
        const std::size_t factor = frame->pointsPerSample;
        ASSERT_GT(factor, 1U);
        const auto perSample = static_cast<double>(factor);
        const double first = -frame->startFraction;
        for (std::size_t index = 0; index < tracesIn(*frame); ++index) {
            SCOPED_TRACE("trace " + std::to_string(index));
            const std::span<const float> values = trace(*frame, index);
            std::vector<double> positions;
            std::vector<double> samples;
            for (std::size_t k = 0; k < values.size(); k += factor) {
                positions.push_back(first + (static_cast<double>(k) / perSample));
                samples.push_back(values[k]);
            }
            const ToneFit fit = ToneFit::of(positions, samples);
            for (std::size_t k = 0; k < values.size(); ++k) {
                if (k % factor != 0) {
                    EXPECT_NEAR(values[k], fit.at(first + (static_cast<double>(k) / perSample)),
                                1e-3)
                        << "point " << k << " of " << values.size();
                }
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Rates, DeepZoomNearTheEnds,
                         ::testing::Values(kSampleRates.front(), kSampleRates.back()),
                         [](const ::testing::TestParamInfo<double>& info) {
                             return rateName(info.param);
                         });

// A deep view starting a few samples into a Beats sweep, where fewer neighbours than the reach lie
// before it: a lane holding broken samples on screen is flagged, in Multi and Compare.
class DeepZoomInvalidAtSweepStart : public ::testing::TestWithParam<Display>
{
};

TEST_P(DeepZoomInvalidAtSweepStart, FlagsWhatTheViewShows)
{
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    // Broken every 7 samples: some on screen in any view of a few samples.
    session.join({.frequency = kOwnTone * kRate, .brokenEvery = 7});
    session.join({.frequency = kOtherTone * kRate});
    auto frame = frozenSession(session, {.mode = GetParam(), .timeBase = TimeBase::kBeats});
    ASSERT_TRUE(frame);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const double width = protocol::kMinimumViewSamples / spanSamples;
    for (const double start : {1.0, 10.0, 31.0}) {
        SCOPED_TRACE(start);
        session.analyzer().setView(start / spanSamples, (start / spanSamples) + width);
        frame = session.produce();
        ASSERT_TRUE(frame);
        ASSERT_GT(frame->pointsPerSample, 1U);
        EXPECT_NE(frame->invalidLanes & 1U, 0U) << "broken samples on screen";
    }
}

// A lane flags by its 10 ms blocks of levels: a broken sample in the block before the view's, the
// reconstruction's neighbours reaching back into it, does not flag the lane. At 120 BPM and
// 48 kHz the held bar starts at host sample 96,000, so its block of 480 samples from there holds
// the broken sample at 470, and a view from 500 starts in the next block.
TEST_P(DeepZoomInvalidAtSweepStart, LeavesThePreviousBlockOut)
{
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.join({.frequency = kOwnTone * kRate, .brokenAt = 96000 + 470});
    session.join({.frequency = kOtherTone * kRate});
    auto frame = frozenSession(session, {.mode = GetParam(), .timeBase = TimeBase::kBeats});
    ASSERT_TRUE(frame);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    ASSERT_EQ(spanSamples, 96000.0) << "a bar";
    const double width = protocol::kMinimumViewSamples / spanSamples;
    // Its block, on screen: flagged.
    session.analyzer().setView(450.0 / spanSamples, (450.0 / spanSamples) + width);
    frame = session.produce();
    ASSERT_TRUE(frame);
    EXPECT_NE(frame->invalidLanes & 1U, 0U) << "the broken sample's block on screen";
    // The next block only: not.
    session.analyzer().setView(500.0 / spanSamples, (500.0 / spanSamples) + width);
    frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_GT(frame->pointsPerSample, 1U);
    EXPECT_EQ(frame->invalidLanes & 1U, 0U) << "the block before the view's";
}

// The same at the next block: a view ending a few samples before the block of 480 from 960, which
// holds the broken sample at 1000, does not flag the lane, though its reconstruction's neighbours
// are read past the view; a view over it does.
TEST_P(DeepZoomInvalidAtSweepStart, LeavesTheNextBlockOut)
{
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.join({.frequency = kOwnTone * kRate, .brokenAt = 96000 + 1000});
    session.join({.frequency = kOtherTone * kRate});
    auto frame = frozenSession(session, {.mode = GetParam(), .timeBase = TimeBase::kBeats});
    ASSERT_TRUE(frame);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    ASSERT_EQ(spanSamples, 96000.0) << "a bar";
    const double width = protocol::kMinimumViewSamples / spanSamples;
    // Ending 8 samples before the next block.
    session.analyzer().setView(920.0 / spanSamples, (920.0 / spanSamples) + width);
    frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_GT(frame->pointsPerSample, 1U);
    EXPECT_EQ(frame->invalidLanes & 1U, 0U) << "the block after the view's";
    // Over it: flagged.
    session.analyzer().setView(985.0 / spanSamples, (985.0 / spanSamples) + width);
    frame = session.produce();
    ASSERT_TRUE(frame);
    EXPECT_NE(frame->invalidLanes & 1U, 0U) << "the broken sample's block on screen";
}

INSTANTIATE_TEST_SUITE_P(Lanes, DeepZoomInvalidAtSweepStart,
                         ::testing::Values(Display::kMulti, Display::kCompare),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

// A sweep's neighbours at its start are read round its wrap, from its last positions: a sample
// that is not finite 5 samples before the held bar's end (host sample 96,000 starts it, the bar
// before ends at 95,999 on screen) is read only as a neighbour of a view at the bar's start. The
// trace is drawn up to it, not through it, and Single's sweep, which flags what the view shows
// (lanes flag by their 10 ms blocks of levels: DeepZoomInvalidAtSweepStart), is not flagged.
TEST(DeepZoomInvalidAcrossTheWrap, LeavesItOutOfTheTraceAndTheChip)
{
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.join({.frequency = kOwnTone * kRate, .brokenAt = 96000 - 5});
    auto frame = frozenSession(session, {.mode = Display::kSingle, .timeBase = TimeBase::kBeats});
    ASSERT_TRUE(frame);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    ASSERT_EQ(spanSamples, 96000.0) << "a bar";
    session.analyzer().setView(0.0, protocol::kMinimumViewSamples / spanSamples);
    frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    const std::size_t factor = frame->pointsPerSample;
    ASSERT_GT(factor, 1U);
    EXPECT_TRUE(noneBroken(*frame)) << "drawn up to it, not through it";
    EXPECT_EQ(frame->invalidLanes, 0U);
    EXPECT_EQ(frame->invalidSamples, 0U);
    for (std::size_t index = 0; index < tracesIn(*frame); ++index) {
        EXPECT_TRUE(drawnStraight(trace(*frame, index), factor, 0))
            << "trace " << index << ": its first gap reads the broken neighbour";
    }
}

// The traces of `kept`, a frame built from the columns kept frame to frame, equal those of the
// same audio reduced afresh: other columns and back, nothing played between.
void expectAsAFreshReduction(Session& session, const DecodedFrame& kept)
{
    session.analyzer().setColumns(kColumns - 1);
    ASSERT_TRUE(session.produce());
    session.analyzer().setColumns(kColumns);
    const auto fresh = session.produce();
    ASSERT_TRUE(fresh);
    ASSERT_EQ(tracesIn(*fresh), tracesIn(kept));
    for (std::size_t index = 0; index < tracesIn(kept); ++index) {
        const std::span<const float> was = trace(kept, index);
        const std::span<const float> now = trace(*fresh, index);
        ASSERT_EQ(now.size(), was.size()) << "trace " << index;
        for (std::size_t i = 0; i < was.size(); ++i) {
            ASSERT_EQ(was[i], now[i])
                << "trace " << index << ", point " << i << " of " << was.size();
        }
    }
}

class SweepEdgeAcrossTheWrap : public ::testing::TestWithParam<Display>
{
};

// Live in Beats, zoomed in at a bar's end, the trace's last gaps read the next pass's first
// samples round the wrap. When the playhead crosses into the next pass, those samples change, and
// the frames built from the columns kept frame to frame draw what a fresh reduction of the same
// audio draws: not the previous pass's neighbours. On noise, which differs from bar to bar, in
// every display mode.
TEST_P(SweepEdgeAcrossTheWrap, FollowsTheNextPassAsAFreshReductionDoes)
{
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 1});
    session.join({.kind = Signal::Kind::kNoise, .seed = 2});
    configure(session.own(), GetParam(), TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
    session.play(1.9, protocol::kReadoutIntervalSeconds / 2.0); // Near the first bar's end.
    session.play(static_cast<double>(kBlock) / kRate);
    auto frame = session.produce();
    ASSERT_TRUE(frame);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const double width = 4.0 * protocol::kMinimumViewSamples / spanSamples;
    session.analyzer().setView(1.0 - width, 1.0);
    ASSERT_TRUE(session.produce());
    // Into the next bar, a frame every 10 ms: the view's samples are the first pass's, its
    // neighbours past the bar's end the second pass's first samples, placed as it begins.
    session.play(0.3, 0.01);
    frame = session.produce();
    if (!frame) {
        session.play(static_cast<double>(kBlock) / kRate);
        frame = session.produce();
    }
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    expectAsAFreshReduction(session, *frame);
}

// The same at the bar's start: its first gaps read the end of the bar round the wrap, which the
// playhead writes just before it wraps; the frames built from the columns kept since draw what a
// fresh reduction of the same audio draws.
TEST_P(SweepEdgeAcrossTheWrap, FollowsThePreviousPassAsAFreshReductionDoes)
{
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 1});
    session.join({.kind = Signal::Kind::kNoise, .seed = 2});
    configure(session.own(), GetParam(), TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
    session.play(2.3, protocol::kReadoutIntervalSeconds / 2.0); // Into the second bar.
    session.play(static_cast<double>(kBlock) / kRate);
    auto frame = session.produce();
    ASSERT_TRUE(frame);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const double width = 4.0 * protocol::kMinimumViewSamples / spanSamples;
    session.analyzer().setView(0.0, width);
    ASSERT_TRUE(session.produce());
    // To the second bar's end and just past it, a frame every 10 ms: the bar's last positions,
    // the view's neighbours before its start, are written as the playhead reaches them.
    session.play(1.75, 0.01);
    frame = session.produce();
    if (!frame) {
        session.play(static_cast<double>(kBlock) / kRate);
        frame = session.produce();
    }
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    expectAsAFreshReduction(session, *frame);
}

INSTANTIATE_TEST_SUITE_P(EveryMode, SweepEdgeAcrossTheWrap, ::testing::ValuesIn(everyMode()),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

// Frozen, a shorter beat span shows part of the held sweep: zoomed in on it, every display mode
// draws the wave as Single does, a point per column for the samples the view shows.
class DeepZoomCroppedFreeze : public ::testing::TestWithParam<Display>
{
};

TEST_P(DeepZoomCroppedFreeze, DrawsTheWaveAsAWholeSweepDoes)
{
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.join({.frequency = kOwnTone * kRate});
    session.join({.frequency = kOtherTone * kRate});
    auto frame = frozenSession(session, {.mode = GetParam(), .timeBase = TimeBase::kBeats});
    ASSERT_TRUE(frame);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kHalfBar));
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frame at the shorter span";
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const double width = protocol::kMinimumViewSamples / spanSamples;
    session.analyzer().setView(0.5 - (width / 2.0), 0.5 + (width / 2.0));
    frame = session.produce();
    ASSERT_TRUE(frame);
    EXPECT_NEAR(frame->visibleSamples, protocol::kMinimumViewSamples, 0.01);
    EXPECT_EQ(frame->pointsPerSample,
              Reconstruction::pointsPerSample(frame->visibleSamples, kColumns));
}

INSTANTIATE_TEST_SUITE_P(EveryMode, DeepZoomCroppedFreeze, ::testing::ValuesIn(everyMode()),
                         [](const ::testing::TestParamInfo<Display>& info) {
                             return modeName(info.param);
                         });

// Compare's sum lane follows the view: panned at the same width, it is the sum of the lanes
// shown there, point for point, not the place it was drawn before. Frozen, and live with the
// host stopped (a Beats sweep that no new audio redraws).
struct SumPannedCase
{
    TimeBase timeBase = TimeBase::kFree;
    bool stopped = false; // Live, the host's transport stopped; else frozen.
};

class DeepZoomSumPanned : public ::testing::TestWithParam<SumPannedCase>
{
};

TEST_P(DeepZoomSumPanned, SumsTheLanesShown)
{
    const SumPannedCase c = GetParam();
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.join({.frequency = kOwnTone * kRate});
    session.join({.frequency = kOtherTone * kRate});
    std::optional<DecodedFrame> frame;
    if (c.stopped) {
        configure(session.own(), Display::kCompare, c.timeBase);
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
        session.own().settings().set(SettingId::kAutoFreeze, 0.0);
        session.play(3.0, protocol::kReadoutIntervalSeconds / 2.0);
        session.playing = false;
        session.play(0.1, protocol::kReadoutIntervalSeconds / 2.0);
        frame = session.produce();
        if (!frame) {
            session.analyzer().setColumns(kColumns - 1); // Forces a frame.
            frame = session.produce();
            session.analyzer().setColumns(kColumns);
        }
    } else {
        frame = frozenSession(session, {.mode = Display::kCompare, .timeBase = c.timeBase});
    }
    ASSERT_TRUE(frame);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const double width = protocol::kMinimumViewSamples / spanSamples;
    // Panned by a whole number of samples, so the part read is as long as before: only where it
    // is changes. 101, so that neither tone repeats over it (both do over 100).
    for (const double start : {0.3, 0.3 + (101.0 / spanSamples)}) {
        SCOPED_TRACE(start);
        session.analyzer().setView(start, start + width);
        frame = session.produce();
        ASSERT_TRUE(frame);
        ASSERT_EQ(frame->laneCount, 3U);
        ASSERT_GT(frame->pointsPerSample, 1U);
        const std::span<const float> own = trace(*frame, 0);
        const std::span<const float> other = trace(*frame, 1);
        const std::span<const float> sum = trace(*frame, 2);
        for (std::size_t k = 0; k < sum.size(); ++k) {
            ASSERT_NEAR(sum[k], own[k] + other[k], 1e-4) << "point " << k;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Cases, DeepZoomSumPanned,
                         ::testing::Values(SumPannedCase{.timeBase = TimeBase::kFree},
                                           SumPannedCase{.timeBase = TimeBase::kBeats},
                                           SumPannedCase{.timeBase = TimeBase::kBeats,
                                                         .stopped = true}),
                         [](const ::testing::TestParamInfo<SumPannedCase>& info) {
                             return timeBaseName(info.param.timeBase) +
                                    (info.param.stopped ? "Stopped" : "Frozen");
                         });

// Compare's matches and summing gains measure the visible samples: reconstructing the wave
// between them (columns enough for a raw frame) changes neither from a narrow editor's min/max
// columns. In Free (renderCompare) and Beats (appendComparison).
class DeepZoomCompare : public ::testing::TestWithParam<TimeBase>
{
};

TEST_P(DeepZoomCompare, MeasuresTheSameAsWithoutTheWave)
{
    constexpr double kRate = 48000.0;
    const auto measure = [&](std::size_t columns) {
        Session session(kRate, 2);
        session.join({.frequency = kOwnTone * kRate});
        session.join({.frequency = kOtherTone * kRate});
        session.analyzer().setColumns(columns);
        auto frame = frozenSession(session, {.mode = Display::kCompare, .timeBase = GetParam()});
        EXPECT_TRUE(frame);
        session.analyzer().setColumns(columns);
        const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
        const double width = protocol::kMinimumViewSamples / spanSamples;
        session.analyzer().setView(0.5 - (width / 2.0), 0.5 + (width / 2.0));
        // The gains are measured over frames: a few more to finish.
        for (int step = 0; step < 32; ++step) {
            if (auto next = session.produce()) {
                frame = std::move(next);
            }
        }
        return frame;
    };
    const auto wave = measure(kColumns);
    const auto lines = measure(static_cast<std::size_t>(protocol::kMinimumViewSamples) / 2);
    ASSERT_TRUE(wave && lines);
    ASSERT_GT(wave->pointsPerSample, 1U);
    ASSERT_EQ(lines->mode, FrameMode::kMinMax);
    const auto matchesOf = [](const DecodedFrame& frame) {
        const std::size_t length = traceLength(frame) * frame.laneCount;
        return std::vector<float>(frame.data.begin() + static_cast<std::ptrdiff_t>(length),
                                  frame.data.end());
    };
    EXPECT_TRUE(sameValues(matchesOf(*wave), matchesOf(*lines)));
    EXPECT_TRUE(sameValues({wave->sumGain, wave->fixedGain}, {lines->sumGain, lines->fixedGain}));
}

INSTANTIATE_TEST_SUITE_P(TimeBases, DeepZoomCompare,
                         ::testing::Values(TimeBase::kFree, TimeBase::kBeats),
                         [](const ::testing::TestParamInfo<TimeBase>& info) {
                             return timeBaseName(info.param);
                         });

// --- Work per frame -------------------------------------------------------------------------
//
// A frame's cost follows what changed, not the size of what is shown (DESIGN.md, AD-17): a sweep
// or a roll reduces only the columns its new audio landed in or that scrolled in, a frozen view
// with nothing new costs nothing, a Compare sum whose parts change is redone over several frames
// rather than in one, and compared tracks are searched one per frame. Work is counted in samples
// (AnalysisWork), so these tests do not depend on the machine's speed: a change that made every
// frame redo the whole span would pass every other test, and only show as a slow editor.

// The other tracks of the group, by lane id.
std::vector<std::uint64_t> othersOf(Session& session)
{
    std::vector<std::uint64_t> others;
    for (const Member& member : session.registry.members()) {
        if (member.engine.get() != &session.own()) {
            others.push_back(member.serial);
        }
    }
    return others;
}

// A session of `mode` with one track in Single and three otherwise, unrelated, each compared.
std::unique_ptr<Session> workSession(const ModeCase& c)
{
    auto session = std::make_unique<Session>(c.rate, c.channels);
    const std::size_t tracks = !isMulti(c.mode) ? 1 : 3;
    for (std::size_t track = 0; track < tracks; ++track) {
        session->join({.kind = Signal::Kind::kNoise, .seed = track + 1});
    }
    configure(session->own(), c.mode, c.timeBase);
    if (c.mode == Display::kCompare) {
        session->analyzer().setShown(othersOf(*session));
    }
    return session;
}

AnalysisWork workSince(const AnalysisWork& before, const AnalysisWork& now)
{
    return {.samplesRead = now.samplesRead - before.samplesRead,
            .samplesReduced = now.samplesReduced - before.samplesReduced,
            .samplesSummed = now.samplesSummed - before.samplesSummed,
            .samplesSearched = now.samplesSearched - before.samplesSearched,
            .matchSearches = now.matchSearches - before.matchSearches,
            .searchWork = now.searchWork - before.searchWork,
            .gainWork = now.gainWork - before.gainWork,
            .sweepUpdates = now.sweepUpdates - before.sweepUpdates};
}

// The work a session does per second of audio, once what it shows is full: played for `fill`
// seconds with a frame each second, then for a second with 60 frames a second, as a page asks for
// them, so that no frame still has the fill's audio to place; then measured over two seconds, 60
// frames a second.
struct WorkPerSecond
{
    double reduced = 0.0;
    double read = 0.0; // Not for match searches, which read up to their window whatever the span.
    double summed = 0.0;
};

WorkPerSecond workPerSecond(Session& session, double fill)
{
    session.play(fill, 1.0);
    constexpr double kFrame = 1.0 / 60.0;
    session.play(1.0, kFrame);
    const AnalysisWork before = session.analyzer().work();
    constexpr double kMeasured = 2.0;
    session.play(kMeasured, kFrame);
    const AnalysisWork spent = workSince(before, session.analyzer().work());
    return {.reduced = static_cast<double>(spent.samplesReduced) / kMeasured,
            .read = static_cast<double>(spent.samplesRead - spent.samplesSearched) / kMeasured,
            .summed = static_cast<double>(spent.samplesSummed) / kMeasured};
}

// Every mode, mono and stereo, at the lowest and highest rates, in `timeBase`.
std::vector<ModeCase> workCases(TimeBase timeBase)
{
    std::vector<ModeCase> cases;
    for (const double rate : {kSampleRates.front(), kSampleRates.back()}) {
        for (const Display mode : everyMode()) {
            for (const int channels : {1, 2}) {
                cases.push_back(
                    {.rate = rate, .mode = mode, .channels = channels, .timeBase = timeBase});
            }
        }
    }
    return cases;
}

// Two shows of the same audio that differ only in size: the longer span in as many more
// columns, so that a column holds as many samples in both. Reducing only what changed costs both
// the same, as each frame reduces the columns its new audio landed in; reducing the whole span
// costs the longer one as many times more as it is longer (four times for the sweeps, twice for
// the rolls). They measure the same exactly; the allowance is for a frame whose new audio
// would straddle a column boundary in one show and not in the other.
constexpr double kSameCost = 1.1;

class SweepWork : public ::testing::TestWithParam<ModeCase>
{
};

// A sweep's frames reduce, measure (its levels per block) and, in Compare, sum only where new
// audio landed, and its readouts measure the newest second: none of them costs more for a longer
// sweep. Match searches are left out of the reads: each reads the newest kMatchWindowSeconds of
// two tracks, or all of a shorter sweep, such as the shorter one here.
TEST_P(SweepWork, CostsNoMoreForALongerSweep)
{
    const auto work = [&](BeatSpan span, std::size_t columns) {
        const auto session = workSession(GetParam());
        session->own().settings().set(SettingId::kBeatSpan, static_cast<double>(span));
        session->analyzer().setColumns(columns);
        // One and four bars at 120 beats a minute: 2 and 8 seconds.
        const double seconds = beatSpanQuarterNotes(span, 4.0) * 60.0 / 120.0;
        return workPerSecond(*session, seconds + 1.0);
    };
    const WorkPerSecond shorter = work(BeatSpan::kOneBar, kColumns);
    const WorkPerSecond longer = work(BeatSpan::kFourBars, 4 * kColumns);
    ASSERT_GT(shorter.reduced, 0.0) << "the sweep reduces its new audio";
    EXPECT_LE(longer.reduced, shorter.reduced * kSameCost)
        << "a longer sweep reduces more than its new audio: " << longer.reduced << " against "
        << shorter.reduced << " samples a second";
    EXPECT_LE(longer.read, shorter.read * kSameCost)
        << "a longer sweep reads more than its new audio: " << longer.read << " against "
        << shorter.read << " samples a second";
    EXPECT_LE(longer.summed, shorter.summed * kSameCost)
        << "a longer sweep sums more than its new audio: " << longer.summed << " against "
        << shorter.summed << " samples a second";
}

INSTANTIATE_TEST_SUITE_P(EveryMode, SweepWork, ::testing::ValuesIn(workCases(TimeBase::kBeats)),
                         modeTestName);

class RollWork : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(RollWork, ReducesNoMoreForALongerRoll)
{
    const double longest = setting(SettingId::kFreeSpan).maximum;
    // Reductions alone: a window's readouts read its whole span, every 100 ms of audio.
    const auto reduced = [&](double span, std::size_t columns) {
        const auto session = workSession(GetParam());
        setSpan(session->own(), GetParam().timeBase, span);
        session->analyzer().setColumns(columns);
        return workPerSecond(*session, span + 1.0).reduced;
    };
    // Both free-running: the cases are in the Free time base, which offers half the longest span.
    static_assert(isFreeRunning(TimeBase::kFree));
    const double shorter = reduced(longest / 2.0, kColumns);
    const double longer = reduced(longest, 2 * kColumns);
    ASSERT_GT(shorter, 0.0) << "the roll reduces the columns that scrolled in";
    EXPECT_LE(longer, shorter * kSameCost)
        << "a longer roll reduces more than what scrolled in: " << longer << " against " << shorter
        << " samples a second";
}

INSTANTIATE_TEST_SUITE_P(EveryMode, RollWork, ::testing::ValuesIn(workCases(TimeBase::kFree)),
                         modeTestName);

// Every mode in every time base, stereo.
std::vector<ModeCase> everyModeAndTimeBase()
{
    std::vector<ModeCase> cases;
    for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
        for (const Display mode : everyMode()) {
            cases.push_back({.mode = mode, .timeBase = static_cast<TimeBase>(timeBase)});
        }
    }
    return cases;
}

class FrozenWork : public ::testing::TestWithParam<ModeCase>
{
};

TEST_P(FrozenWork, ReadsNothingWhileNothingChanges)
{
    const auto session = workSession(GetParam());
    session->own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
    setSpan(session->own(), GetParam().timeBase, 0.5);
    session->play(3.0, 1.0);
    // Live frames read their audio: a count that never moved would pass below for nothing.
    ASSERT_GT(session->analyzer().work().samplesRead, 0U) << "live frames read no audio";
    session->analyzer().setFrozen(true);
    // The frozen frame, and any that finish what it started (a Compare sum, the matches).
    ASSERT_TRUE(session->produce()) << "no frozen frame";
    for (int frame = 0; frame < 64 && session->produce(); ++frame) {
    }
    const AnalysisWork before = session->analyzer().work();
    // The host plays on: new audio arrives, but nothing on screen changes.
    session->play(1.0, static_cast<double>(kBlock) / session->rate);
    const AnalysisWork spent = workSince(before, session->analyzer().work());
    EXPECT_EQ(spent.samplesRead, 0U) << "an unchanged frozen view read audio again";
    EXPECT_EQ(spent.matchSearches, 0U) << "an unchanged frozen view searched again";
}

INSTANTIATE_TEST_SUITE_P(EveryMode, FrozenWork, ::testing::ValuesIn(everyModeAndTimeBase()),
                         modeTestName);

// Checking another track changes every column and level of a Compare sweep's sum. A long sweep of
// several tracks is redone over the next frames, so that no frame stalls the display; it is
// redone in full, so that the sum is right once it has.
TEST(CompareSumWork, IsRedoneOverSeveralFramesWhenATrackIsChecked)
{
    // The worst case's sweep: sixteen bars at 60 beats a minute and 96 kHz, 64 s of each track.
    constexpr double kRate = 96000.0;
    constexpr double kTempo = 60.0;
    Session session(kRate, 2);
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        session.join({.kind = Signal::Kind::kNoise, .seed = seed}, kTempo);
    }
    configure(session.own(), Display::kCompare, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kSixteenBars));
    const std::vector<std::uint64_t> others = othersOf(session);
    session.analyzer().setShown({others.front()});
    const double sweepSeconds = beatSpanQuarterNotes(BeatSpan::kSixteenBars, 4.0) * 60.0 / kTempo;
    session.play(sweepSeconds + 1.0, 1.0);
    for (int frame = 0; frame < 64 && session.produce(); ++frame) {
    }

    // Both others checked: this track and two more, each over the whole sweep.
    session.analyzer().setShown(others);
    const auto whole = 3 * static_cast<std::uint64_t>(std::llround(sweepSeconds * kRate));
    std::uint64_t most = 0;
    std::uint64_t total = 0;
    for (int frame = 0; frame < 64; ++frame) {
        const AnalysisWork before = session.analyzer().work();
        session.play(static_cast<double>(kBlock) / kRate, static_cast<double>(kBlock) / kRate);
        const std::uint64_t summed = workSince(before, session.analyzer().work()).samplesSummed;
        most = std::max(most, summed);
        total += summed;
    }
    EXPECT_LT(most, whole) << "one frame summed the whole sweep of every compared track";
    EXPECT_GE(total, whole) << "no recomputation summed the whole sweep";
}

// Every mode at the lowest and highest rates, mono and stereo, in every time base.
std::vector<ModeCase> everyWorkCase()
{
    std::vector<ModeCase> cases;
    for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
        std::ranges::copy(workCases(static_cast<TimeBase>(timeBase)), std::back_inserter(cases));
    }
    return cases;
}

class AdvancedFrame : public ::testing::TestWithParam<ModeCase>
{
};

// While the page has not asked for a frame, the analysis places new audio in the sweeps as it
// arrives (advance), also advancing in between when nothing new has. The frame asked for next
// shows that audio at once: it is not left waiting for more audio to arrive, which delayed it by
// up to a host block, every frame.
TEST_P(AdvancedFrame, DrawsTheAudioPlacedSinceTheLastFrame)
{
    const auto session = workSession(GetParam());
    session->own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
    setSpan(session->own(), GetParam().timeBase, 0.5);
    session->play(3.0, 1.0 / 60.0);
    session->play(1.0 / 60.0);
    session->analyzer().advance();
    // The worker advances more often than the host's blocks arrive: one that finds nothing new.
    session->analyzer().advance();
    std::vector<std::byte> frame;
    EXPECT_TRUE(session->analyzer().produce(frame)) << "the new audio was not drawn";
}

INSTANTIATE_TEST_SUITE_P(EveryMode, AdvancedFrame, ::testing::ValuesIn(everyWorkCase()),
                         modeTestName);

// Compare at the lowest and highest rates, mono and stereo, in every time base.
std::vector<ModeCase> compareWorkCases()
{
    std::vector<ModeCase> cases;
    for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
        for (const ModeCase& c : workCases(static_cast<TimeBase>(timeBase))) {
            if (c.mode == Display::kCompare) {
                cases.push_back(c);
            }
        }
    }
    return cases;
}

class CompareTrackWork : public ::testing::TestWithParam<ModeCase>
{
};

// All tracks' new audio lands at the same positions of the sweep; each frame sums those positions
// once for each compared track, not once more for every other track compared, so checking many
// tracks costs each of them no more than checking few.
TEST_P(CompareTrackWork, SumsEachTracksNewAudioOnce)
{
    const ModeCase c = GetParam();
    const auto summedPerTrack = [&](std::size_t tracks) {
        Session session(c.rate, c.channels);
        for (std::size_t track = 0; track <= tracks; ++track) {
            session.join({.kind = Signal::Kind::kNoise, .seed = track + 1});
        }
        configure(session.own(), Display::kCompare, c.timeBase);
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
        setSpan(session.own(), c.timeBase, 0.5);
        session.analyzer().setShown(othersOf(session));
        return workPerSecond(session, 3.0).summed / static_cast<double>(tracks);
    };
    const double few = summedPerTrack(2);
    const double many = summedPerTrack(6);
    ASSERT_GT(few, 0.0) << "nothing summed";
    EXPECT_LE(many, few * kSameCost)
        << "each compared track summed " << many << " samples a second with six compared, " << few
        << " with two";
}

INSTANTIATE_TEST_SUITE_P(EveryRate, CompareTrackWork, ::testing::ValuesIn(compareWorkCases()),
                         modeTestName);

// The most a frame sums over `seconds` of audio, a frame at display rate.
std::uint64_t mostSummedPerFrame(Session& session, double seconds)
{
    constexpr double kFrame = 1.0 / 60.0;
    std::uint64_t most = 0;
    const auto frames = static_cast<int>(std::lround(seconds / kFrame));
    for (int index = 0; index < frames; ++index) {
        const AnalysisWork before = session.analyzer().work();
        session.play(kFrame, kFrame);
        most = std::max(most, workSince(before, session.analyzer().work()).samplesSummed);
    }
    return most;
}

class MovingMatchWork : public ::testing::TestWithParam<double>
{
};

// When every compared track's match moves, as when a session's routing changes, the fixed sum's
// levels, each shifted track's levels and the drawn sum's columns are all redone over the next
// frames. Together they take at most one frame's share of that work (kSumRefreshSamples), so that
// frames keep their pace however many matches moved, and at a high rate, where each takes longer.
TEST_P(MovingMatchWork, RedoesTheSumsWithinOneFramesShare)
{
    const double rate = GetParam();
    Session session(rate, 2);
    constexpr std::int64_t kTracks = 5;
    for (std::int64_t track = 0; track < kTracks; ++track) {
        session.join({.kind = Signal::Kind::kNoise, .seed = 1, .delay = track * 8});
    }
    configure(session.own(), Display::kCompare, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kFourBars));
    session.analyzer().setShown(othersOf(session));
    session.analyzer().setPreviewFix(true);
    const double sweepSeconds = beatSpanQuarterNotes(BeatSpan::kFourBars, 4.0) * 60.0 / 120.0;
    // The sweep filled, every match found and every sum measured with it.
    session.play(sweepSeconds + 1.0, 1.0);
    session.play(protocol::kMatchWindowSeconds, 1.0 / 60.0);
    // What a frame sums with nothing to redo: its own new audio.
    const std::uint64_t steady = mostSummedPerFrame(session, 1.0);

    for (std::int64_t track = 1; track < kTracks; ++track) {
        session.setSignal(static_cast<std::size_t>(track),
                          {.kind = Signal::Kind::kNoise, .seed = 1, .delay = (track * 8) + 40});
    }
    // Each match moves once the new routing holds most of what it is searched over.
    const std::uint64_t moving = mostSummedPerFrame(session, protocol::kMatchWindowSeconds);
    ASSERT_GT(moving, steady) << "the moved matches' sums were not redone";
    // Beside the share, such a frame sums its own new audio, as a steady one does, and measures
    // the gains again, as they follow the recomputation: at most a steady frame's work again.
    EXPECT_LE(moving, kSumRefreshSamples + (2 * steady))
        << "the sums redone after the matches moved took more than one frame's share: " << moving
        << " samples summed in a frame, " << steady << " in a steady one";
}

INSTANTIATE_TEST_SUITE_P(EveryRate, MovingMatchWork, ::testing::ValuesIn(kSampleRates),
                         rateTestName);

class FirstShiftWork : public ::testing::TestWithParam<double>
{
};

// Four bars at 60 beats a minute: 16 s of each track.
constexpr double kFirstShiftTempo = 60.0;
double firstShiftSweepSeconds()
{
    return beatSpanQuarterNotes(BeatSpan::kFourBars, 4.0) * 60.0 / kFirstShiftTempo;
}

// The rates at which that sweep holds more of a track than one frame's share, which a measurement
// at once would exceed.
std::vector<double> firstShiftRates()
{
    std::vector<double> rates;
    std::ranges::copy_if(kSampleRates, std::back_inserter(rates), [](double rate) {
        return firstShiftSweepSeconds() * rate > static_cast<double>(kSumRefreshSamples);
    });
    return rates;
}

// A compared track the fix shifts for the first time needs its levels as shifted over the whole
// sweep. Measured in the frame its match was found, a long sweep at a high rate stalled that frame;
// they are redone over the frames that follow, as when a match moves, within one frame's share.
TEST_P(FirstShiftWork, MeasuresTheShiftedTrackOverFrames)
{
    const double rate = GetParam();
    Session session(rate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 1}, kFirstShiftTempo);
    session.join({.kind = Signal::Kind::kNoise, .seed = 1}, kFirstShiftTempo);
    configure(session.own(), Display::kCompare, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kFourBars));
    session.analyzer().setShown(othersOf(session));
    session.analyzer().setPreviewFix(true);
    const double sweepSeconds = firstShiftSweepSeconds();
    ASSERT_GT(sweepSeconds * rate, static_cast<double>(kSumRefreshSamples))
        << "the sweep fits in one frame's share";
    session.play(sweepSeconds + 1.0, 1.0);
    session.play(protocol::kMatchWindowSeconds, 1.0 / 60.0);
    // In time with this track: matched, but not shifted.
    const std::uint64_t steady = mostSummedPerFrame(session, 1.0);

    // Its routing changes: 40 samples late, which the fix shifts.
    session.setSignal(1, {.kind = Signal::Kind::kNoise, .seed = 1, .delay = 40});
    const std::uint64_t shifted = mostSummedPerFrame(session, protocol::kMatchWindowSeconds);
    ASSERT_GT(shifted, steady) << "the shifted track's levels were not measured";
    EXPECT_LE(shifted, kSumRefreshSamples + (2 * steady))
        << "a frame measured the shifted track's levels over the whole sweep: " << shifted
        << " samples summed in a frame, " << steady << " in a steady one";
}

INSTANTIATE_TEST_SUITE_P(LongSweeps, FirstShiftWork, ::testing::ValuesIn(firstShiftRates()),
                         rateTestName);

class MatchWork : public ::testing::TestWithParam<ModeCase>
{
};

// Every compared track is searched for its match, but one per frame, so that many tracks share
// the analysis thread.
TEST_P(MatchWork, SearchesOneComparedTrackPerFrame)
{
    const ModeCase c = GetParam();
    Session session(c.rate, c.channels);
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        session.join({.kind = Signal::Kind::kNoise, .seed = seed});
    }
    configure(session.own(), Display::kCompare, c.timeBase);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
    session.own().settings().set(SettingId::kFreeSpan, 0.5);
    const std::vector<std::uint64_t> others = othersOf(session);
    session.analyzer().setShown(others);
    std::uint64_t most = 0;
    std::uint64_t total = 0;
    // Three seconds of audio, a block and a frame at a time.
    const double block = static_cast<double>(kBlock) / c.rate;
    const auto frames = static_cast<int>(std::lround(3.0 / block));
    for (int frame = 0; frame < frames; ++frame) {
        const AnalysisWork before = session.analyzer().work();
        session.play(block, block);
        const std::uint64_t searches = workSince(before, session.analyzer().work()).matchSearches;
        most = std::max(most, searches);
        total += searches;
    }
    EXPECT_LE(most, 1U) << "a frame searched more than one compared track";
    // With fewer searches than compared tracks, some were never searched (and a bound on each
    // frame's searches would hold for nothing).
    EXPECT_GE(total, others.size()) << "fewer searches than compared tracks";
}

// Compare in every time base, stereo.
std::vector<ModeCase> compareCases()
{
    std::vector<ModeCase> cases;
    cases.reserve(protocol::kTimeBaseCount);
    for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
        cases.push_back({.mode = Display::kCompare, .timeBase = static_cast<TimeBase>(timeBase)});
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EveryTimeBase, MatchWork, ::testing::ValuesIn(compareCases()),
                         modeTestName);

// A match search over a long window at a high rate (here 8 s, or the longest span, at 96 kHz) is
// spread over frames, a budget of work in each (kMatchSearchWork), so that no frame stalls on it:
// in every time base.
TEST_P(MatchWork, SpreadsALongSearchOverFrames)
{
    const ModeCase c = GetParam();
    constexpr double kRate = 96000.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 1});
    session.join({.kind = Signal::Kind::kNoise, .seed = 1, .delay = 40});
    configure(session.own(), Display::kCompare, c.timeBase);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kEightBars));
    setSpan(session.own(), c.timeBase, setting(spanSetting(c.timeBase)).maximum);
    session.analyzer().setShown(othersOf(session));
    session.play(17.0, 1.0);
    std::uint64_t most = 0;
    std::uint64_t total = 0;
    const double block = static_cast<double>(kBlock) / kRate;
    const auto frames = static_cast<int>(std::lround(2.0 / block));
    for (int frame = 0; frame < frames; ++frame) {
        const AnalysisWork before = session.analyzer().work();
        session.play(block, block);
        const std::uint64_t work = workSince(before, session.analyzer().work()).searchWork;
        most = std::max(most, work);
        total += work;
    }
    ASSERT_GT(total, kMatchSearchWork) << "the searches were not long enough to spread";
    // A frame stops once its budget is spent, finishing the block of the window it is passing
    // over: at most a pass over the window beyond it, and never a transform it cannot pay for.
    const auto window = static_cast<std::uint64_t>(kMatchWindowSeconds * kRate);
    EXPECT_LE(most, kMatchSearchWork + window) << "a frame did a whole search's work at once";
}

// On a lane channel other than Mid, a search spread over frames still ends, and its match shows
// and stays while the channel does: the channel is compared with the one last measured, not
// taken as a new choice in every frame.
TEST_P(MatchWork, ShowsAMatchSearchedOverFramesOnAnotherLaneChannel)
{
    const ModeCase c = GetParam();
    constexpr double kRate = 96000.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 1});
    session.join({.kind = Signal::Kind::kNoise, .seed = 1, .delay = 40});
    configure(session.own(), Display::kCompare, c.timeBase);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kEightBars));
    setSpan(session.own(), c.timeBase, setting(spanSetting(c.timeBase)).maximum);
    session.own().settings().set(SettingId::kLaneChannel,
                                 static_cast<double>(protocol::ChannelView::kLeft));
    session.analyzer().setShown(othersOf(session));
    session.play(17.0);
    const auto correlationOf = [](const DecodedFrame& frame) {
        return frame.data.at(std::size_t{frame.laneCount} * traceLength(frame) +
                             std::size_t{1} * protocol::kMatchValues);
    };
    std::optional<DecodedFrame> frame;
    for (int step = 0; step < 20 && !(frame && std::isfinite(correlationOf(*frame))); ++step) {
        session.play(0.01);
        frame = session.produce();
    }
    ASSERT_TRUE(frame);
    EXPECT_TRUE(std::isfinite(correlationOf(*frame))) << "the search ended on Left";
    for (int step = 0; step < 10; ++step) {
        session.play(0.01);
        frame = session.produce();
        ASSERT_TRUE(frame);
        EXPECT_TRUE(std::isfinite(correlationOf(*frame))) << "the match stays, frame " << step;
    }
}

// Frozen, the audio shown is held and does not change: once its matches are found, a frozen view
// has nothing more to search or measure while the host plays on: it reads nothing and draws no
// more frames, and redrawn at another width, it searches no match again. Nine
// tracks over a long window at 96 kHz, whose searches take several frames each, in every time
// base.
TEST_P(MatchWork, LeavesAFrozenViewAloneOnceItsMatchesAreFound)
{
    const ModeCase c = GetParam();
    constexpr double kRate = 96000.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 1});
    for (std::int64_t copy = 1; copy <= 8; ++copy) {
        session.join({.kind = Signal::Kind::kNoise, .seed = 1, .delay = 10 * copy});
    }
    configure(session.own(), Display::kCompare, c.timeBase);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kFourBars));
    setSpan(session.own(), c.timeBase, setting(spanSetting(c.timeBase)).maximum);
    session.analyzer().setShown(othersOf(session));
    // The view filled: four bars at 120 beats a minute (8 s), or the longest span of the time base.
    session.play(c.timeBase == TimeBase::kBeats ? 9.0 : 5.0, 1.0);
    session.analyzer().setFrozen(true);
    const double frame = 1.0 / 60.0;
    for (int tick = 0; tick < 240; ++tick) { // Four seconds to find them all.
        session.play(frame);
        (void)session.produce();
    }
    int drawn = 0;
    const AnalysisWork quietFrom = session.analyzer().work();
    for (int tick = 0; tick < 60; ++tick) {
        session.play(frame);
        drawn += session.produce().has_value() ? 1 : 0;
    }
    EXPECT_EQ(drawn, 0) << "frames drawn in a second of a frozen view";
    EXPECT_EQ(workSince(quietFrom, session.analyzer().work()).samplesRead, 0U)
        << "the held audio read again, to search or measure it";
    // Redrawn with nothing it shows changed (another window width), it has no match to find.
    const AnalysisWork redrawFrom = session.analyzer().work();
    session.analyzer().setColumns(kColumns / 2);
    for (int tick = 0; tick < 240 && session.produce(); ++tick) {
    }
    EXPECT_EQ(workSince(redrawFrom, session.analyzer().work()).matchSearches, 0U)
        << "matches searched again over the same held audio";
}

// Panned while a match search and the gains are still being found, a frozen view ends both: they
// would describe audio no longer shown, and are work for nothing. This track and seven copies of
// it, which start just as the frozen 1 s view does, at 384 kHz (more than a frame to search, and
// to read for the gains): one frame in, panned back to before the copies sound, nothing sounds
// together, so every match and both gains are no value.
TEST(MatchWork, EndsTheSearchAndGainsOfAViewPannedAway)
{
    constexpr double kRate = 384000.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 3});
    const auto copiesStart = static_cast<std::int64_t>(1.52 * kRate);
    for (std::int64_t copy = 0; copy < 7; ++copy) {
        session.join(
            {.kind = Signal::Kind::kNoise, .seed = 3, .delay = 10 * copy, .startsAt = copiesStart});
    }
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    session.own().settings().set(SettingId::kFreeSpan, 1.0);
    session.analyzer().setShown(othersOf(session));
    session.play(2.5);
    session.analyzer().setFrozen(true);
    const AnalysisWork before = session.analyzer().work();
    ASSERT_TRUE(session.produce());
    const AnalysisWork first = workSince(before, session.analyzer().work());
    // A frame's search stops short of a transform its budget cannot pay for: most of the budget
    // spent says it is still going.
    ASSERT_GE(first.searchWork, kMatchSearchWork / 2) << "the search ended in its first frame";
    ASSERT_TRUE(std::isnan(session.field(FrameHeader::kSumGainOffset)))
        << "the gains were measured in their first frame: nothing to end";
    session.analyzer().setPan(1.0); // Back to before the copies sound.
    const AnalysisWork panned = session.analyzer().work();
    std::optional<DecodedFrame> frame = session.produce();
    ASSERT_TRUE(frame) << "no frame for the pan";
    const AnalysisWork onPan = workSince(panned, session.analyzer().work());
    EXPECT_EQ(onPan.searchWork, 0U) << "the search of the view panned away went on";
    EXPECT_EQ(onPan.gainWork, 0U) << "the gains of the view panned away went on";
    for (int step = 0; step < 400; ++step) {
        auto produced = session.produce();
        if (!produced) {
            break;
        }
        frame = std::move(produced);
    }
    ASSERT_TRUE(frame);
    for (std::size_t track = 1; track <= 7; ++track) {
        const std::size_t at =
            std::size_t{frame->laneCount} * traceLength(*frame) + track * protocol::kMatchValues;
        EXPECT_TRUE(std::isnan(frame->data.at(at))) << "track " << track << " still matched";
    }
    EXPECT_TRUE(std::isnan(session.field(FrameHeader::kSumGainOffset)))
        << "gains of the view panned away";
    EXPECT_TRUE(std::isnan(session.field(FrameHeader::kFixedGainOffset)));
}

// Live with the transport stopped and no audio arriving, panning a zoomed roll shows other audio,
// and its matches are searched again over what it shows: this track and two copies sound only in
// the newest second of a 4 s roll; zoomed onto that second the copies match, and panned onto the
// oldest, where nothing sounds, every match and the summing gain are no value.
TEST(MatchWork, FollowsAZoomedRollPannedWithTheTransportStopped)
{
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    const auto sounds = static_cast<std::int64_t>(3.0 * kRate);
    session.join({.kind = Signal::Kind::kNoise, .seed = 5, .startsAt = sounds});
    session.join({.kind = Signal::Kind::kNoise, .seed = 5, .startsAt = sounds});
    session.join({.kind = Signal::Kind::kNoise, .seed = 5, .startsAt = sounds});
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    session.own().settings().set(SettingId::kFreeSpan, 4.0);
    session.analyzer().setShown(othersOf(session));
    session.playing = false;
    session.play(4.0);
    const auto settle = [&] {
        std::optional<DecodedFrame> last;
        for (int frame = 0; frame < 200; ++frame) {
            auto produced = session.produce();
            if (!produced) {
                break;
            }
            last = std::move(produced);
        }
        return last;
    };
    const auto matched = [](const DecodedFrame& frame, std::size_t track) {
        const std::size_t at =
            std::size_t{frame.laneCount} * traceLength(frame) + track * protocol::kMatchValues;
        return frame.data.at(at);
    };
    session.analyzer().setView(0.75, 1.0);
    auto frame = settle();
    ASSERT_TRUE(frame);
    ASSERT_TRUE(std::isfinite(matched(*frame, 1))) << "the copies match where they sound";
    session.analyzer().setView(0.0, 0.25);
    frame = settle();
    ASSERT_TRUE(frame) << "no frame for the pan";
    for (const std::size_t track : {1U, 2U}) {
        EXPECT_TRUE(std::isnan(matched(*frame, track))) << "track " << track << " still matched";
    }
    EXPECT_TRUE(std::isnan(session.field(FrameHeader::kSumGainOffset)))
        << "the gain of the second shown before";
}

// A compared track that cannot be read yet (it joined mid-play, so it holds less than the view)
// takes its turn and is tried again soon, but does not take every turn: the other tracks' matches
// still follow a change meanwhile. A copy's delay changes from 10 to 30 samples while a track
// that joined just before holds too little to search; within two seconds, the copy reads 30.
TEST(MatchWork, RefreshesTheOthersWhileATrackCannotBeRead)
{
    constexpr double kRate = 48000.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 6});
    session.join({.kind = Signal::Kind::kNoise, .seed = 6, .delay = 10});
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    session.own().settings().set(SettingId::kFreeSpan, setting(SettingId::kFreeSpan).maximum);
    session.play(5.0, 1.0 / 60.0);
    session.join({.kind = Signal::Kind::kNoise, .seed = 7}); // Holds nothing of the view yet.
    session.analyzer().setShown(othersOf(session));
    session.setSignal(1, {.kind = Signal::Kind::kNoise, .seed = 6, .delay = 30});
    double offset = 0.0;
    for (int frame = 0; frame < 120; ++frame) {
        session.play(1.0 / 60.0);
        if (const auto drawn = session.produce()) {
            ASSERT_EQ(drawn->laneCount, 4) << "three tracks and the sum";
            offset = drawn->data.at(std::size_t{drawn->laneCount} * traceLength(*drawn) +
                                    (1 * protocol::kMatchValues) + 1);
        }
    }
    EXPECT_EQ(offset, 30.0) << "the copy's match never refreshed";
}

// Frozen, the searches and gains of a zoomed view run over many frames, but what it shows changes
// only when one ends: the frame is built only then, not at every step, and never sent again
// unchanged. Eight tracks over a long window at 96 kHz, frozen and zoomed onto its second half,
// in Beats and Free.
TEST_P(MatchWork, BuildsAFrozenFrameOnlyWhenItsWorkEnds)
{
    const ModeCase c = GetParam();
    if (c.timeBase == TimeBase::kTrigger) {
        // Its longest span, a second, is searched in a step a track: every step ends a search.
        GTEST_SKIP() << "Trigger's spans are too short for a search to run over frames";
    }
    constexpr double kRate = 96000.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 11});
    for (std::int64_t copy = 1; copy <= 7; ++copy) {
        session.join({.kind = Signal::Kind::kNoise, .seed = 11, .delay = 10 * copy});
    }
    configure(session.own(), Display::kCompare, c.timeBase);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kFourBars));
    setSpan(session.own(), c.timeBase, setting(spanSetting(c.timeBase)).maximum);
    session.analyzer().setShown(othersOf(session));
    session.play(c.timeBase == TimeBase::kBeats ? 9.0 : 5.0, 1.0);
    session.analyzer().setFrozen(true);
    session.analyzer().setView(0.5, 1.0);
    std::vector<std::byte> last;
    std::vector<std::byte> bytes;
    int sent = 0;
    int repeated = 0;
    int working = 0; // Ticks that stepped a search or gains.
    // Ticks that built a frame, sent or not: in Trigger and Free its columns reduced, in Beats
    // (whose frozen lanes keep their columns, and which has no gains to read) its readouts read.
    int built = 0;
    for (int tick = 0; tick < 600; ++tick) {
        const AnalysisWork before = session.analyzer().work();
        const bool drawn = session.analyzer().produce(bytes);
        const AnalysisWork spent = workSince(before, session.analyzer().work());
        working += spent.searchWork > 0 || spent.gainWork > 0 ? 1 : 0;
        const bool build = c.timeBase == TimeBase::kBeats
                               ? spent.samplesRead > spent.samplesSearched
                               : spent.samplesReduced > 0;
        built += build ? 1 : 0;
        if (drawn) {
            ++sent;
            repeated += bytes == last ? 1 : 0;
            last = bytes;
        }
    }
    EXPECT_GT(sent, 1) << "the matches were never shown";
    EXPECT_EQ(repeated, 0) << "frames sent again unchanged, of " << sent;
    EXPECT_LT(built, working) << "a frame built at every step of the work";
}

// Frozen, a match whose audio could not be read (a held copy can be caught short, and is captured
// again on the next frame) is tried again on the frames that follow, not left as no value until
// the view changes, also with no audio arriving and nothing else to do; and tracks that can never
// be read are not tried on and on. Tracks that joined just before the freeze hold too little of
// the view: each is tried more than once, and within a second no more.
TEST(MatchWork, TriesAFailedReadAgainWhileFrozen)
{
    constexpr double kRate = 48000.0;
    constexpr double kSpan = 4.0;
    // This track alone with one late track, so nothing else keeps the frames going; and with a
    // copy and two late tracks, each of which would keep the other waiting.
    for (const auto& [copies, late] : {std::pair{0, 1}, std::pair{1, 2}}) {
        SCOPED_TRACE(testing::Message() << copies << " copies, " << late << " late tracks");
        Session session(kRate, 2);
        session.join({.kind = Signal::Kind::kNoise, .seed = 8});
        for (int copy = 0; copy < copies; ++copy) {
            session.join({.kind = Signal::Kind::kNoise, .seed = 8, .delay = 10});
        }
        configure(session.own(), Display::kCompare, TimeBase::kFree);
        session.own().settings().set(SettingId::kFreeSpan, kSpan);
        session.play(5.0, 1.0);
        for (int track = 0; track < late; ++track) { // Holding nothing of the view yet.
            session.join({.kind = Signal::Kind::kNoise, .seed = 9U + static_cast<unsigned>(track)});
        }
        session.analyzer().setShown(othersOf(session));
        session.play(0.2);
        const AnalysisWork before = session.analyzer().work();
        session.analyzer().setFrozen(true);
        std::vector<std::byte> bytes;
        int triedAtEnd = 0; // Ticks of the second half that still read to search.
        for (int tick = 0; tick < 120; ++tick) { // No audio arrives.
            const AnalysisWork tickBefore = session.analyzer().work();
            (void)session.analyzer().produce(bytes);
            const bool tried = workSince(tickBefore, session.analyzer().work()).samplesSearched > 0;
            triedAtEnd += tick >= 60 && tried ? 1 : 0;
        }
        // Each try reads the newest part of the view, of this track and the other: the copies'
        // searches, then the late tracks' tries.
        const auto window =
            static_cast<std::uint64_t>(std::min(kMatchWindowSeconds, kSpan) * kRate) * 2;
        const std::uint64_t tries =
            workSince(before, session.analyzer().work()).samplesSearched / window -
            static_cast<std::uint64_t>(copies);
        EXPECT_GE(tries, 2U * static_cast<std::uint64_t>(late))
            << "the late tracks' failed reads were not tried again";
        EXPECT_EQ(triedAtEnd, 0) << "a read that keeps failing was tried on and on";
    }
}

// Dragged, a frozen Free view moves a little each frame: a search or measurement begun in one
// frame would be ended by the next frame's move, so none begins until the view stays where it is,
// and then they do. Nine tracks over a 1 s view at 96 kHz, panned back through the audio held
// for a second.
TEST(MatchWork, WaitsForAFrozenViewToStopBeingDragged)
{
    constexpr double kRate = 96000.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 1});
    for (std::int64_t copy = 1; copy <= 8; ++copy) {
        session.join({.kind = Signal::Kind::kNoise, .seed = 1, .delay = 10 * copy});
    }
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    session.own().settings().set(SettingId::kFreeSpan, 1.0);
    session.analyzer().setShown(othersOf(session));
    session.play(5.0, 1.0);
    session.analyzer().setFrozen(true);
    for (int frame = 0; frame < 400 && session.produce(); ++frame) {
    }
    const AnalysisWork before = session.analyzer().work();
    for (int frame = 1; frame <= 60; ++frame) {
        session.analyzer().setPan(0.01 * frame);
        ASSERT_TRUE(session.produce()) << "no frame for the move";
    }
    const AnalysisWork dragged = workSince(before, session.analyzer().work());
    EXPECT_EQ(dragged.searchWork, 0U) << "searches begun while dragged";
    EXPECT_EQ(dragged.gainWork, 0U) << "gains measured while dragged";
    const AnalysisWork stopped = session.analyzer().work();
    for (int frame = 0; frame < 400 && session.produce(); ++frame) {
    }
    const AnalysisWork after = workSince(stopped, session.analyzer().work());
    EXPECT_GT(after.searchWork, 0U) << "nothing searched once it stopped";
    EXPECT_GT(after.gainWork, 0U) << "nothing measured once it stopped";
}

// A Compare roll's gains measured over several frames are the ones measured at once would be:
// this track, a copy and a copy 40 samples late, over the longest span at 384 kHz (more than a
// frame's budget to read), sum as recorded to 10 log10(5/3) dB (two in phase, one unrelated)
// and, fixed, to 10 log10(9/3) dB (all three in phase), live and then frozen. Until the first
// measurement ends, both are no value, not 0.
TEST(CompareGainWork, MeasuresALongRollsGainsOverFramesAsAtOnce)
{
    constexpr double kRate = 384000.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 7});
    session.join({.kind = Signal::Kind::kNoise, .seed = 7});
    session.join({.kind = Signal::Kind::kNoise, .seed = 7, .delay = 40});
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    const double span = setting(SettingId::kFreeSpan).maximum;
    session.own().settings().set(SettingId::kFreeSpan, span);
    session.play(span + 0.5); // No frames yet.
    session.analyzer().setShown(othersOf(session));
    ASSERT_TRUE(session.produce());
    EXPECT_TRUE(std::isnan(session.field(FrameHeader::kSumGainOffset)))
        << "a gain before it was measured";
    EXPECT_TRUE(std::isnan(session.field(FrameHeader::kFixedGainOffset)));

    const auto expectGains = [&] {
        // The frames that follow with no more audio, until the searches and gains have ended.
        for (int frame = 0; frame < 200 && session.produce(); ++frame) {
        }
        EXPECT_NEAR(session.field(FrameHeader::kSumGainOffset), 10.0 * std::log10(5.0 / 3.0), 0.15);
        EXPECT_NEAR(session.field(FrameHeader::kFixedGainOffset), 10.0 * std::log10(9.0 / 3.0),
                    0.15);
    };
    {
        SCOPED_TRACE("live");
        expectGains();
    }
    {
        SCOPED_TRACE("frozen");
        session.analyzer().setFrozen(true);
        expectGains();
    }
}

// The gains read every compared track's view, stereo tracks as their Mid, which needs room for a
// second channel: room enough for a view longer than any read before, though no match search
// (with no other track compared, there is none) has made room for it. A playing stereo roll of
// this track alone, lengthened from 2 s to the longest span, is drawn and measured.
TEST(CompareGainWork, ReadsALongerViewThanAnyBefore)
{
    constexpr double kRate = 96000.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 21});
    session.join({.kind = Signal::Kind::kNoise, .seed = 22});
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    session.own().settings().set(SettingId::kFreeSpan, 2.0);
    session.play(5.0, 1.0 / 60.0);
    session.own().settings().set(SettingId::kFreeSpan, setting(SettingId::kFreeSpan).maximum);
    session.play(0.5, 1.0 / 60.0);
    EXPECT_TRUE(session.produce()) << "the longer roll was not drawn";
}

// Frozen, the audio shown is held: its gains, once measured, stay while the host plays on, rather
// than coming due again every 100 ms of live audio. A measurement that takes longer than that (four
// tracks over the longest span at 384 kHz) would otherwise end in a frame that begins the next,
// for as long as the view stays frozen. Within two seconds of the freeze, it reads nothing more.
TEST(CompareGainWork, MeasuresAFrozenRollsGainsOnce)
{
    constexpr double kRate = 384000.0;
    Session session(kRate, 2);
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        session.join({.kind = Signal::Kind::kNoise, .seed = seed});
    }
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    const double span = setting(SettingId::kFreeSpan).maximum;
    session.own().settings().set(SettingId::kFreeSpan, span);
    session.analyzer().setShown(othersOf(session));
    session.play(span + 0.5);
    session.analyzer().setFrozen(true);
    std::vector<std::byte> bytes;
    for (int tick = 0; tick < 120; ++tick) { // The host plays on.
        session.play(1.0 / 60.0);
        (void)session.analyzer().produce(bytes);
    }
    ASSERT_FALSE(bytes.empty()) << "no frame drawn";
    ASSERT_TRUE(std::isfinite(get<float>(bytes, FrameHeader::kSumGainOffset))) << "never measured";
    const AnalysisWork before = session.analyzer().work();
    for (int tick = 0; tick < 60; ++tick) {
        session.play(1.0 / 60.0);
        (void)session.analyzer().produce(bytes);
    }
    const AnalysisWork spent = workSince(before, session.analyzer().work());
    EXPECT_EQ(spent.gainWork, 0U) << "the held audio's gains measured again";
    EXPECT_EQ(spent.samplesRead, 0U) << "the held audio read again";
}

// Playing, a long roll's window moves on between the frames over which its gains are measured:
// each track is read where the window was when they began, so that the tracks the fix aligns stay
// aligned. This track, a copy and a copy 40 samples late, at 384 kHz over the longest span with a
// block of audio between frames: once the gains are measured with the matches found, every frame's
// gains are the ones measured at once.
TEST(CompareGainWork, KeepsALongRollsGainsWhilePlaying)
{
    constexpr double kRate = 384000.0;
    const double sumGain = 10.0 * std::log10(5.0 / 3.0);
    const double fixedGain = 10.0 * std::log10(9.0 / 3.0);
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 7});
    session.join({.kind = Signal::Kind::kNoise, .seed = 7});
    session.join({.kind = Signal::Kind::kNoise, .seed = 7, .delay = 40});
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    const double span = setting(SettingId::kFreeSpan).maximum;
    session.own().settings().set(SettingId::kFreeSpan, span);
    session.play(span + 0.5); // No frames yet.
    session.analyzer().setShown(othersOf(session));
    int fixedFrames = 0;
    int wrong = 0;
    for (int frame = 0; frame < 300; ++frame) {
        session.play(static_cast<double>(kBlock) / kRate);
        if (!session.produce()) {
            continue;
        }
        const double sum = session.field(FrameHeader::kSumGainOffset);
        const double fixed = session.field(FrameHeader::kFixedGainOffset);
        // Until the matches are found, there is nothing to fix: the fixed gain is the sum's.
        if (fixedFrames == 0 && !(std::abs(fixed - fixedGain) <= 0.15)) {
            continue;
        }
        ++fixedFrames;
        if (!(std::abs(sum - sumGain) <= 0.15 && std::abs(fixed - fixedGain) <= 0.15)) {
            ++wrong;
        }
    }
    EXPECT_GT(fixedFrames, 100) << "the fix was not measured, or late";
    EXPECT_EQ(wrong, 0) << "of " << fixedFrames << " frames measured with the fix";
}

// A page that is hidden stops asking for frames, and the gains being measured wait: shown again
// after the capture has moved past the audio they began on, they are measured afresh, not from
// silence where that audio was. This track, a copy and a copy 40 samples late, at 384 kHz over the
// longest span, hidden for six seconds partway through a measurement: every gain shown afterwards
// is the one measured at once.
TEST(CompareGainWork, MeasuresAfreshAfterThePageWasHidden)
{
    constexpr double kRate = 384000.0;
    const double sumGain = 10.0 * std::log10(5.0 / 3.0);
    const double fixedGain = 10.0 * std::log10(9.0 / 3.0);
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 7});
    session.join({.kind = Signal::Kind::kNoise, .seed = 7});
    session.join({.kind = Signal::Kind::kNoise, .seed = 7, .delay = 40});
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    const double span = setting(SettingId::kFreeSpan).maximum;
    session.own().settings().set(SettingId::kFreeSpan, span);
    session.play(span + 0.5);
    session.analyzer().setShown(othersOf(session));
    const double block = static_cast<double>(kBlock) / kRate;
    // Until the gains are measured with the matches found.
    bool fixed = false;
    for (int frame = 0; frame < 300 && !fixed; ++frame) {
        session.play(block);
        fixed = session.produce().has_value() &&
                std::abs(session.field(FrameHeader::kFixedGainOffset) - fixedGain) <= 0.15;
    }
    ASSERT_TRUE(fixed);
    // Hidden just after a frame that began the next measurement, which takes several frames at
    // this rate: one that read a frame's budget of samples.
    bool begun = false;
    for (int frame = 0; frame < 300 && !begun; ++frame) {
        const AnalysisWork before = session.analyzer().work();
        session.play(block);
        (void)session.produce();
        begun = workSince(before, session.analyzer().work()).samplesRead > kGainReadWork / 2;
    }
    ASSERT_TRUE(begun);
    session.play(6.0);
    int wrong = 0;
    for (int frame = 0; frame < 120; ++frame) {
        session.play(block);
        if (!session.produce()) {
            continue;
        }
        const double sum = session.field(FrameHeader::kSumGainOffset);
        const double fixedNow = session.field(FrameHeader::kFixedGainOffset);
        if (!(std::abs(sum - sumGain) <= 0.15 && std::abs(fixedNow - fixedGain) <= 0.15)) {
            ++wrong;
        }
    }
    EXPECT_EQ(wrong, 0) << "frames with gains measured from audio since overwritten";
}

// Frozen, every match is searched again, one a frame, while the gains take more than a frame to
// measure: a match found while they are being measured is in the fixed gain all the same, though
// no audio arrives to make them due again. This track, six unrelated tracks, then two copies at
// different delays, checked last so that their matches are the last found: fixed, the three
// copies sum in phase, to 10 log10((9 + 6) / 9) dB; as recorded, none do, to about 0 dB.
TEST(CompareGainWork, FixesTheGainWithEveryMatchFoundWhileMeasuring)
{
    constexpr double kRate = 48000.0;
    constexpr double kSpan = 4.0;
    Session session(kRate, 2);
    session.join({.kind = Signal::Kind::kNoise, .seed = 9});
    for (std::uint64_t seed = 101; seed <= 106; ++seed) {
        session.join({.kind = Signal::Kind::kNoise, .seed = seed});
    }
    for (std::int64_t copy = 1; copy <= 2; ++copy) {
        session.join({.kind = Signal::Kind::kNoise, .seed = 9, .delay = 10 * copy});
    }
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    session.own().settings().set(SettingId::kFreeSpan, kSpan);
    // No frame until frozen, so every match is first found while frozen.
    session.play(kSpan + 1.0);
    session.analyzer().setShown(othersOf(session));
    session.analyzer().setFrozen(true);
    for (int frame = 0; frame < 300 && session.produce(); ++frame) {
    }
    EXPECT_NEAR(session.field(FrameHeader::kFixedGainOffset), 10.0 * std::log10(15.0 / 9.0), 0.3);
    EXPECT_NEAR(session.field(FrameHeader::kSumGainOffset), 0.0, 0.3);
}

// A triggered view starts a fraction of a sample after a sample, a different fraction each frame:
// zoomed in, the samples shown begin one sample later in some frames than in others. That is the
// same view, so the gains measured over several frames still end. A tone of 384.5 samples a cycle
// triggers half a sample apart from frame to frame, and two zooms half a sample apart make sure
// that one of them moves by a sample: fifteen tracks over the longest Trigger span at 384 kHz
// have their summing gains measured within a second of frames.
TEST(CompareGainWork, MeasuresAZoomedTriggeredViewsGains)
{
    constexpr double kRate = 384000.0;
    const double span = setting(SettingId::kTriggerSpan).maximum;
    const double count = span * kRate;
    for (const double halfSamples : {0.0, 1.0}) {
        SCOPED_TRACE(halfSamples);
        Session session(kRate, 2);
        session.join({.frequency = kRate / 384.5, .amplitude = 0.5, .offset = 0.0});
        for (std::uint64_t seed = 1; seed < 15; ++seed) {
            session.join({.kind = Signal::Kind::kNoise, .seed = seed});
        }
        configure(session.own(), Display::kCompare, TimeBase::kTrigger);
        session.own().settings().set(SettingId::kTriggerMode,
                                     static_cast<double>(protocol::TriggerMode::kNormal));
        session.own().settings().set(SettingId::kTriggerSpan, span);
        session.analyzer().setShown(othersOf(session));
        const double start = (std::floor(0.3 * count) + 0.25 + (0.5 * halfSamples)) / count;
        session.analyzer().setView(start, 0.9);
        session.play(span + 0.5);
        bool measured = false;
        for (int frame = 0; frame < 60 && !measured; ++frame) {
            session.play(1.0 / 60.0);
            measured = session.produce().has_value() &&
                       std::isfinite(session.field(FrameHeader::kSumGainOffset));
        }
        EXPECT_TRUE(measured) << "the gains were begun again every few frames";
    }
}

// Other tracks compared, the gains are no value until they are measured for them, rather than the
// last tracks' gains under the new tracks' names: eight tracks over the longest span at 96 kHz
// (more than a frame to read), one unchecked once their gains are measured.
TEST(CompareGainWork, ShowsNoGainsForOtherTracksUntilMeasured)
{
    constexpr double kRate = 96000.0;
    Session session(kRate, 2);
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        session.join({.kind = Signal::Kind::kNoise, .seed = seed});
    }
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    session.own().settings().set(SettingId::kFreeSpan, setting(SettingId::kFreeSpan).maximum);
    std::vector<std::uint64_t> others = othersOf(session);
    session.analyzer().setShown(others);
    session.play(5.0);
    session.analyzer().setFrozen(true);
    for (int frame = 0; frame < 400 && session.produce(); ++frame) {
    }
    ASSERT_TRUE(std::isfinite(session.field(FrameHeader::kSumGainOffset)));
    others.pop_back();
    session.analyzer().setShown(others);
    ASSERT_TRUE(session.produce()) << "no frame for the change";
    EXPECT_TRUE(std::isnan(session.field(FrameHeader::kSumGainOffset)))
        << "the last tracks' gain shown for other tracks";
    EXPECT_TRUE(std::isnan(session.field(FrameHeader::kFixedGainOffset)));
    for (int frame = 0; frame < 400 && session.produce(); ++frame) {
    }
    EXPECT_TRUE(std::isfinite(session.field(FrameHeader::kSumGainOffset))) << "never measured";
}

// Zoomed or panned, a long roll's gains are measured again over several frames, and meanwhile the
// last ones stay on show, as they do between refreshes: a value, not "no value", in every frame of
// a drag. Eight tracks over the longest span at 96 kHz (more than a frame to read), live and
// frozen.
TEST(CompareGainWork, KeepsTheLastGainsOnShowWhileAViewIsMeasured)
{
    constexpr double kRate = 96000.0;
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        Session session(kRate, 2);
        for (std::uint64_t seed = 1; seed <= 8; ++seed) {
            session.join({.kind = Signal::Kind::kNoise, .seed = seed});
        }
        configure(session.own(), Display::kCompare, TimeBase::kFree);
        const double span = setting(SettingId::kFreeSpan).maximum;
        session.own().settings().set(SettingId::kFreeSpan, span);
        session.analyzer().setShown(othersOf(session));
        session.play(span + 1.0);
        session.analyzer().setFrozen(frozen);
        for (int frame = 0; frame < 200 && session.produce(); ++frame) {
        }
        ASSERT_TRUE(std::isfinite(session.field(FrameHeader::kSumGainOffset)));
        for (int step = 1; step <= 8; ++step) {
            session.analyzer().setView(0.0, 1.0 - (0.05 * step));
            ASSERT_TRUE(session.produce()) << "no frame for the zoom";
            EXPECT_TRUE(std::isfinite(session.field(FrameHeader::kSumGainOffset)))
                << "no value while zooming, step " << step;
            EXPECT_TRUE(std::isfinite(session.field(FrameHeader::kFixedGainOffset)));
        }
    }
}

// Compare left for Single while its matches and gains are still being found, over frames, leaves
// nothing to do: with no new audio, live or frozen, Single draws the frame for the change and then
// none, as it does after any other change.
TEST(CompareGainWork, LeavesNothingToRedrawInSingle)
{
    constexpr double kRate = 96000.0;
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        Session session(kRate, 2);
        for (std::uint64_t seed = 1; seed <= 8; ++seed) {
            session.join({.kind = Signal::Kind::kNoise, .seed = seed});
        }
        configure(session.own(), Display::kCompare, TimeBase::kFree);
        const double span = setting(SettingId::kFreeSpan).maximum;
        session.own().settings().set(SettingId::kFreeSpan, span);
        session.analyzer().setShown(othersOf(session));
        session.play(span + 1.0);
        session.analyzer().setFrozen(frozen);
        ASSERT_TRUE(session.produce());
        ASSERT_TRUE(session.produce()) << "the gains took a single frame: nothing in progress";
        show(session.own().settings(), Display::kSingle);
        ASSERT_TRUE(session.produce()) << "no frame for the change";
        EXPECT_FALSE(session.produce()) << "Single drew the same frame again";
    }
}

// A Compare roll measures its summing gains over the whole view of every compared track, as
// recorded and as fixed. Eight tracks over the longest span are read, and passed over to
// measure them, over several frames, so no frame reads every track's view at once, nor measures
// a gain at once. At 96 kHz and at the highest rate.
TEST(CompareGainWork, ReadsARollsTracksOverSeveralFrames)
{
    constexpr std::size_t kTracks = 8;
    for (const double rate : {96000.0, kSampleRates.back()}) {
        SCOPED_TRACE(rateName(rate));
        Session session(rate, 2);
        for (std::uint64_t seed = 1; seed <= kTracks; ++seed) {
            session.join({.kind = Signal::Kind::kNoise, .seed = seed});
        }
        configure(session.own(), Display::kCompare, TimeBase::kFree);
        const double span = setting(SettingId::kFreeSpan).maximum;
        session.own().settings().set(SettingId::kFreeSpan, span);
        session.analyzer().setShown(othersOf(session));
        session.play(span + 1.0);
        (void)session.produce(); // The roll drawn whole, once, as on opening the view.
        std::uint64_t mostRead = 0;
        std::uint64_t mostPassed = 0;
        bool measured = false;
        const double block = static_cast<double>(kBlock) / rate;
        for (int frame = 0; frame < 40; ++frame) {
            const AnalysisWork before = session.analyzer().work();
            session.play(block);
            const bool drawn = session.produce().has_value();
            const AnalysisWork spent = workSince(before, session.analyzer().work());
            mostRead = std::max(mostRead, spent.samplesRead);
            mostPassed = std::max(mostPassed, spent.gainWork);
            measured =
                measured || (drawn && std::isfinite(session.field(FrameHeader::kSumGainOffset)));
        }
        // As recorded and as fixed: what measuring them all at once reads.
        const auto everyView = static_cast<std::uint64_t>(kTracks * span * rate);
        EXPECT_LT(mostRead, 2 * everyView) << "one frame read every compared track's view, twice";
        EXPECT_GT(mostPassed, 0U) << "nothing measured";
        // A gain measured at once passes over every view; spread, when that is well beyond what a
        // frame may pass over, no frame does.
        if (everyView > 2 * kGainReadWork) {
            EXPECT_LT(mostPassed, everyView) << "one frame measured a gain at once";
        }
        // And they are measured: unrelated noise sums to a gain near 0 dB, a number.
        EXPECT_TRUE(measured);
    }
}

// ----- A sum that frames left out -----

// What a kept sum sat out: Compare switched off, or its last lane showing the other of the sum and
// the difference.
enum class Away : std::uint8_t
{
    kCompareOff,
    kOtherLane,
};

struct ReturningCase
{
    double rate = 0.0;
    bool frozen = false;
    TimeBase timeBase = TimeBase::kBeats;
    protocol::CompareLane lane = protocol::CompareLane::kSum;
    Away away = Away::kCompareOff;
    bool align = false; // The difference with Align on: the other track lined up with this one.
};

class ReturningSum : public ::testing::TestWithParam<ReturningCase>
{
};

// Every frame of `analyzer` until none comes: the last one, with whatever was being redone done.
std::optional<DecodedFrame> settled(Analyzer& analyzer)
{
    std::optional<DecodedFrame> last;
    std::vector<std::byte> bytes;
    int quiet = 0;
    for (int tick = 0; tick < 4096 && quiet < 64; ++tick) {
        if (analyzer.produce(bytes)) {
            last = decode(bytes);
            quiet = 0;
        } else {
            ++quiet;
        }
    }
    return last;
}

// The same figure: the same number, or no value in both.
bool sameFigure(float one, float other)
{
    return (std::isnan(one) && std::isnan(other)) || one == other;
}

void showLane(Engine& engine, protocol::CompareLane lane)
{
    engine.settings().set(SettingId::kCompareLane, static_cast<double>(lane));
}

void setAlign(Engine& engine, bool on)
{
    engine.settings().set(SettingId::kAlign,
                          static_cast<double>(on ? protocol::Toggle::kOn : protocol::Toggle::kOff));
}

// Compare's last lane left and come back to while the music plays (Compare switched off and on,
// or the lane showing the difference in place of the sum, or the reverse), live or frozen
// meanwhile: what it shows on coming back is what a session that showed it all along shows, as
// soon as its sums are redone, not only once the sweep has passed again (which frozen it never
// does). Here the other track turns into this one inverted while the lane is away: back, the sum
// cancels exactly (it did read +2 dB) and the difference is twice this track; with Align on, the
// other track is flipped back and taken out of this one, and its own lane is drawn as lined up:
// a lane kept as recorded beside it would not be a fresh session's.
TEST_P(ReturningSum, MatchesAFreshSum)
{
    const ReturningCase c = GetParam();
    const bool difference = c.lane == protocol::CompareLane::kDifference;
    const protocol::CompareLane other =
        difference ? protocol::CompareLane::kSum : protocol::CompareLane::kDifference;
    Session session(c.rate, 2);
    session.join({.kind = Signal::Kind::kNoise, .offset = 0.0, .seed = 3});
    session.join({.kind = Signal::Kind::kNoise, .offset = 0.0, .seed = 5});
    configure(session.own(), Display::kCompare, c.timeBase);
    showLane(session.own(), c.lane);
    setAlign(session.own(), c.align);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
    setSpan(session.own(), c.timeBase, 2.0);
    session.analyzer().setShown(othersOf(session));
    session.play(3.0, 0.05);
    session.play(0.01); // Audio no frame has shown yet, so that one comes.
    ASSERT_TRUE(session.produce());
    if (difference) {
        ASSERT_NEAR(session.field(FrameHeader::kNullDepthOffset), 3.0, 1.0)
            << "unrelated: the difference is as loud as the two together";
    } else {
        ASSERT_NEAR(session.field(FrameHeader::kSumGainOffset), 0.0, 1.0) << "unrelated: harmless";
    }

    if (c.away == Away::kCompareOff) {
        show(session.own().settings(), Display::kMulti);
    } else {
        showLane(session.own(), other);
    }
    session.setSignal(1,
                      {.kind = Signal::Kind::kNoise, .amplitude = -0.25, .offset = 0.0, .seed = 3});
    session.play(2.5, 0.05); // More than the bar: every position of the other track is new.
    session.play(0.01);
    if (c.frozen) {
        // A freeze holds the window last shown: one at the newest audio, here and afresh below.
        ASSERT_TRUE(session.produce());
        session.analyzer().setFrozen(true);
    }
    show(session.own().settings(), Display::kCompare);
    showLane(session.own(), c.lane);
    // No audio from here on: what follows comes of the sums being redone, not of the sweep
    // passing over them again.
    const std::optional<DecodedFrame> returned = settled(session.analyzer());
    ASSERT_TRUE(returned);

    Analyzer fresh(session.own(), session.registry);
    fresh.setColumns(kColumns);
    if (c.frozen) {
        std::vector<std::byte> live;
        ASSERT_TRUE(fresh.produce(live));
        fresh.setFrozen(true);
    }
    const std::optional<DecodedFrame> expected = settled(fresh);
    ASSERT_TRUE(expected);
    ASSERT_TRUE(expected->has(FrameFlag::kCompared));
    ASSERT_EQ(expected->compareLane, c.lane);
    if (difference && c.align) {
        ASSERT_LE(expected->nullDepth, protocol::kGainFloorDb)
            << "the other track is this one inverted: lined up, it is taken out of this track";
    } else if (difference) {
        ASSERT_NEAR(expected->nullDepth, 20.0 * std::log10(2.0), 1e-3)
            << "the other track is this one inverted: the difference is twice this track";
    } else {
        ASSERT_EQ(expected->sumGain, -std::numeric_limits<float>::infinity())
            << "the other track is this one inverted: the sum cancels exactly";
    }
    EXPECT_EQ(returned->compareLane, expected->compareLane);
    EXPECT_TRUE(sameFigure(returned->sumGain, expected->sumGain))
        << returned->sumGain << " for " << expected->sumGain;
    EXPECT_TRUE(sameFigure(returned->fixedGain, expected->fixedGain))
        << returned->fixedGain << " for " << expected->fixedGain;
    EXPECT_TRUE(sameFigure(returned->nullDepth, expected->nullDepth))
        << returned->nullDepth << " for " << expected->nullDepth;
    ASSERT_EQ(tracesIn(*returned), tracesIn(*expected));
    ASSERT_EQ(returned->pointCount, expected->pointCount);
    for (std::size_t index = 0; index < tracesIn(*expected); ++index) {
        EXPECT_TRUE(std::ranges::equal(trace(*returned, index), trace(*expected, index)))
            << (index + 1 == tracesIn(*expected) ? "the last lane" : "a track's lane");
    }
}

std::vector<ReturningCase> returningCases()
{
    std::vector<ReturningCase> cases;
    for (const double rate : kSampleRates) {
        for (const bool frozen : {false, true}) {
            cases.push_back({.rate = rate, .frozen = frozen});
            // The sum while the difference shows, and the difference while the sum does.
            for (const protocol::CompareLane lane :
                 {protocol::CompareLane::kSum, protocol::CompareLane::kDifference}) {
                cases.push_back(
                    {.rate = rate, .frozen = frozen, .lane = lane, .away = Away::kOtherLane});
            }
            // The difference with Align on, away for the sum and with Compare off.
            for (const Away away : {Away::kOtherLane, Away::kCompareOff}) {
                cases.push_back({.rate = rate,
                                 .frozen = frozen,
                                 .lane = protocol::CompareLane::kDifference,
                                 .away = away,
                                 .align = true});
            }
        }
    }
    // Free, rolling, keeps its columns by the audio's own positions, which nothing rewrites: the
    // same steps, right before and after.
    for (const bool frozen : {false, true}) {
        cases.push_back({.rate = 48000.0, .frozen = frozen, .timeBase = TimeBase::kFree});
        for (const bool align : {false, true}) {
            cases.push_back({.rate = 48000.0,
                             .frozen = frozen,
                             .timeBase = TimeBase::kFree,
                             .lane = protocol::CompareLane::kDifference,
                             .away = Away::kOtherLane,
                             .align = align});
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EveryRate, ReturningSum, ::testing::ValuesIn(returningCases()),
                         [](const ::testing::TestParamInfo<ReturningCase>& info) {
                             const ReturningCase& c = info.param;
                             const bool difference = c.lane == protocol::CompareLane::kDifference;
                             return rateName(c.rate) + "_" + timeBaseName(c.timeBase) +
                                    (difference ? "_Difference" : "_Sum") +
                                    (c.align ? "Aligned" : "") +
                                    (c.away == Away::kCompareOff ? "_CompareOff" : "_OtherLane") +
                                    (c.frozen ? "_Frozen" : "_Live");
                         });

// ----- The difference -----

struct DifferenceCase
{
    double rate = 0.0;
    int channels = 2;
    TimeBase timeBase = TimeBase::kBeats;
    bool frozen = false;
};

class Difference : public ::testing::TestWithParam<DifferenceCase>
{
};

// Compare's last lane showing the difference, with Align off: this track minus the other checked
// tracks, sample for sample. Here the others are this track at a half and at a quarter of its
// level, so the difference is this track at a quarter: its lane is this track's lane, a quarter as
// tall, and it nulls 12 dB below this track. The frame says the lane is the difference, and carries
// no summing gains, which are the sum's.
TEST_P(Difference, IsThisTrackMinusTheOthers)
{
    const DifferenceCase c = GetParam();
    Session session(c.rate, c.channels);
    session.join({.kind = Signal::Kind::kNoise, .amplitude = 0.5, .offset = 0.0, .seed = 3});
    session.join({.kind = Signal::Kind::kNoise, .amplitude = 0.25, .offset = 0.0, .seed = 3});
    session.join({.kind = Signal::Kind::kNoise, .amplitude = 0.125, .offset = 0.0, .seed = 3});
    configure(session.own(), Display::kCompare, c.timeBase);
    showLane(session.own(), protocol::CompareLane::kDifference);
    setAlign(session.own(), false);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
    setSpan(session.own(), c.timeBase, c.timeBase == TimeBase::kFree ? 2.0 : 0.05);
    session.analyzer().setShown(othersOf(session));
    session.analyzer().setPreviewFix(true); // The sum's, and nothing to the difference.
    session.play(3.0, 0.05);
    session.play(0.01);
    std::optional<DecodedFrame> frame = session.produce();
    ASSERT_TRUE(frame);
    if (c.frozen) {
        session.analyzer().setFrozen(true);
    }
    // And whatever follows with no more audio: the depth, where it is measured over frames.
    if (auto later = settled(session.analyzer())) {
        frame = std::move(later);
    }
    ASSERT_TRUE(frame->has(FrameFlag::kCompared));
    ASSERT_EQ(frame->laneCount, 4);
    EXPECT_EQ(frame->compareLane, protocol::CompareLane::kDifference);
    EXPECT_TRUE(std::isnan(frame->sumGain)) << frame->sumGain;
    EXPECT_TRUE(std::isnan(frame->fixedGain)) << frame->fixedGain;
    EXPECT_NEAR(frame->nullDepth, 20.0 * std::log10(0.25), 1e-3);
    const std::span<const float> own = trace(*frame, 0);
    const std::span<const float> difference = trace(*frame, 3);
    ASSERT_FALSE(own.empty());
    bool sounded = false;
    for (std::size_t index = 0; index < own.size(); ++index) {
        ASSERT_EQ(difference[index], 0.25f * own[index]) << "at point " << index;
        sounded = sounded || own[index] != 0.0f;
    }
    EXPECT_TRUE(sounded) << "nothing drawn to compare";
}

std::vector<DifferenceCase> differenceCases()
{
    std::vector<DifferenceCase> cases;
    for (std::size_t rate = 0; rate < kSampleRates.size(); ++rate) {
        for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
            for (const bool frozen : {false, true}) {
                cases.push_back({.rate = kSampleRates.at(rate),
                                 .channels = (rate + timeBase) % 2 == 0 ? 2 : 1,
                                 .timeBase = static_cast<TimeBase>(timeBase),
                                 .frozen = frozen});
            }
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EveryMode, Difference, ::testing::ValuesIn(differenceCases()),
                         [](const ::testing::TestParamInfo<DifferenceCase>& info) {
                             const DifferenceCase& c = info.param;
                             return rateName(c.rate) + "_" + timeBaseName(c.timeBase) + "_" +
                                    channelsName(c.channels) + (c.frozen ? "_Frozen" : "_Live");
                         });

// The difference costs no more to keep than the sum: with steady audio, a frame adds up no more
// samples for it, and passes over none for summing gains it does not show.
TEST(DifferenceWork, SumsNoMoreAFrameThanTheSum)
{
    const auto summedPerFrame = [](protocol::CompareLane lane, TimeBase timeBase) {
        Session session(48000.0, 2);
        session.join({.kind = Signal::Kind::kNoise, .offset = 0.0, .seed = 3});
        session.join({.kind = Signal::Kind::kNoise, .offset = 0.0, .seed = 5});
        configure(session.own(), Display::kCompare, timeBase);
        showLane(session.own(), lane);
        setSpan(session.own(), timeBase, 2.0);
        session.analyzer().setShown(othersOf(session));
        session.play(6.0, 0.05);
        const AnalysisWork before = session.analyzer().work();
        session.play(2.0, 0.05);
        return workSince(before, session.analyzer().work());
    };
    for (const TimeBase timeBase : {TimeBase::kBeats, TimeBase::kFree}) {
        SCOPED_TRACE(timeBaseName(timeBase));
        const AnalysisWork sum = summedPerFrame(protocol::CompareLane::kSum, timeBase);
        const AnalysisWork difference =
            summedPerFrame(protocol::CompareLane::kDifference, timeBase);
        EXPECT_GT(difference.samplesSummed, 0U);
        EXPECT_LE(difference.samplesSummed, sum.samplesSummed);
        EXPECT_LE(difference.gainWork, sum.gainWork);
    }
}

// The difference's depth over a long roll at the highest rate, zoomed into its newer half, takes
// several frames to measure. Until it is, frames carry no value for it, never a number that was
// not measured; and it is measured while the roll plays on, though each frame's new audio moves
// what the captures hold: eight unrelated tracks at one level leave a difference 9 dB above this
// track.
TEST(DifferenceWork, ShowsNoDepthUntilItIsMeasuredOverAZoomedRoll)
{
    constexpr std::size_t kTracks = 8;
    const double rate = kSampleRates.back();
    Session session(rate, 2);
    for (std::uint64_t seed = 1; seed <= kTracks; ++seed) {
        session.join({.kind = Signal::Kind::kNoise, .offset = 0.0, .seed = seed});
    }
    configure(session.own(), Display::kCompare, TimeBase::kFree);
    showLane(session.own(), protocol::CompareLane::kDifference);
    setAlign(session.own(), false);
    const double span = setting(SettingId::kFreeSpan).maximum;
    session.own().settings().set(SettingId::kFreeSpan, span);
    session.analyzer().setShown(othersOf(session));
    // Long enough for the captures to be full: from here on each block replaces their oldest.
    session.play((3.0 * span) + 1.0);
    ASSERT_TRUE(session.produce());
    ASSERT_TRUE(std::isnan(session.field(FrameHeader::kNullDepthOffset)))
        << "a depth before any was measured";
    session.analyzer().setView(0.5, 1.0);
    const double block = static_cast<double>(kBlock) / rate;
    bool measured = false;
    for (int frame = 0; frame < 40; ++frame) {
        session.play(block);
        if (!session.produce()) {
            continue;
        }
        const float depth = session.field(FrameHeader::kNullDepthOffset);
        if (!std::isnan(depth)) {
            ASSERT_NEAR(depth, 10.0 * std::log10(static_cast<double>(kTracks)), 1.0)
                << "frame " << frame;
            measured = true;
        }
    }
    EXPECT_TRUE(measured);
}

// ----- Align -----

// A compared track's match in a frame: its correlation, its delay in samples and its polarity
// (-1 flipped, 0 not related).
struct LaneMatch
{
    float correlation = 0.0f;
    float delay = 0.0f;
    float polarity = 0.0f;
};

LaneMatch matchOf(const DecodedFrame& frame, std::size_t track)
{
    const std::size_t at =
        std::size_t{frame.laneCount} * traceLength(frame) + track * protocol::kMatchValues;
    return {.correlation = frame.data.at(at),
            .delay = frame.data.at(at + 1),
            .polarity = frame.data.at(at + 2)};
}

// What an effect did to the copy of a track on another one.
struct Effect
{
    double delaySeconds = 0.046; // Beyond the sum's reach and within Align's.
    double fraction = 0.0;       // Of a sample, beyond the whole samples of the delay.
    double gain = 1.0;           // The copy's level against the track's.
    bool flipped = true;
    double lowPass = 0.0; // Both tracks' noise, limited (Signal::lowPass).
    // Viewed from the track after the effect: this track is the late one, and the other, as
    // recorded before the effect, is what Align moves.
    bool fromCopy = false;

    [[nodiscard]] std::int64_t delaySamples(double rate) const
    {
        return std::llround(delaySeconds * rate);
    }
};

// The track before `effect` (noise), and its copy after it.
Signal beforeEffect(const Effect& effect)
{
    return {.kind = Signal::Kind::kNoise,
            .amplitude = 0.2,
            .offset = 0.0,
            .seed = 3,
            .lowPass = effect.lowPass};
}

Signal afterEffect(double rate, const Effect& effect)
{
    Signal signal = beforeEffect(effect);
    signal.amplitude *= effect.flipped ? -effect.gain : effect.gain;
    signal.delay = effect.delaySamples(rate);
    signal.fraction = effect.fraction;
    return signal;
}

// This track and a second one: a track before `effect` and its copy after it.
void joinCopy(Session& session, double rate, const Effect& effect)
{
    session.join(effect.fromCopy ? afterEffect(rate, effect) : beforeEffect(effect));
    session.join(effect.fromCopy ? beforeEffect(effect) : afterEffect(rate, effect));
}

struct AlignedCase
{
    double rate = 0.0;
    int channels = 2;
    TimeBase timeBase = TimeBase::kBeats;
    bool frozen = false;
};

// The spans the Align tests use: one bar (2 s) in Beats, 2 s rolling in Free, and a quarter of a
// second in Trigger, each several times the delay.
void showDifference(Session& session, TimeBase timeBase)
{
    configure(session.own(), Display::kCompare, timeBase);
    showLane(session.own(), protocol::CompareLane::kDifference);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBar));
    setSpan(session.own(), timeBase, timeBase == TimeBase::kFree ? 2.0 : 0.25);
    session.analyzer().setShown(othersOf(session));
}

// Plays long enough for the alignment to be found and drawn, then gives the last frame: live, or
// frozen and settled.
std::optional<DecodedFrame> playAndShow(Session& session, bool frozen)
{
    session.play(4.0, 0.05);
    session.play(0.01);
    std::optional<DecodedFrame> frame = session.produce();
    if (!frame) {
        return frame;
    }
    if (frozen) {
        session.analyzer().setFrozen(true);
    }
    if (auto later = settled(session.analyzer())) {
        frame = std::move(later);
    }
    return frame;
}

// How many of a lane's columns are empty (no audio: drawn silent), and how many are neither
// empty nor `expected`'s within rounding: the edges of an empty stretch, where a column holds
// both.
struct LaneFit
{
    std::size_t same = 0;
    std::size_t empty = 0;
    std::size_t other = 0;
};

LaneFit fitOf(std::span<const float> lane, std::span<const float> expected)
{
    LaneFit fit;
    for (std::size_t point = 0; point + 1 < lane.size(); point += 2) {
        const bool same = std::abs(lane[point] - expected[point]) <= 1e-5f &&
                          std::abs(lane[point + 1] - expected[point + 1]) <= 1e-5f;
        const bool empty = lane[point] == 0.0f && lane[point + 1] == 0.0f;
        ++(same ? fit.same : empty ? fit.empty : fit.other);
    }
    return fit;
}

class Aligned : public ::testing::TestWithParam<AlignedCase>
{
};

// A track compared with itself after an effect that makes it 46 ms late (more than the sum's
// reach, as a look-ahead limiter or a linear-phase equaliser does) and flipped: in the difference
// Align takes both out, so the difference is silent and nulls below what rounding leaves; the
// other track's lane is drawn lined up, the same as this track's but where its audio has not
// arrived, which is empty; and the frame says what was taken out, for the label.
TEST_P(Aligned, NullsALateFlippedCopy)
{
    const AlignedCase c = GetParam();
    const Effect effect;
    Session session(c.rate, c.channels);
    joinCopy(session, c.rate, effect);
    showDifference(session, c.timeBase);
    const std::optional<DecodedFrame> frame = playAndShow(session, c.frozen);
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->has(FrameFlag::kCompared));
    ASSERT_EQ(frame->compareLane, protocol::CompareLane::kDifference);

    const LaneMatch match = matchOf(*frame, 1);
    EXPECT_EQ(match.delay, static_cast<float>(effect.delaySamples(c.rate)));
    EXPECT_EQ(match.polarity, -1.0f);
    EXPECT_LE(frame->nullDepth, protocol::kGainFloorDb);

    ASSERT_EQ(frame->mode, FrameMode::kMinMax);
    const std::span<const float> own = trace(*frame, 0);
    const LaneFit lane = fitOf(trace(*frame, 1), own);
    // Empty over the delay at the newest audio and, in a sweep, at its right edge; a column at
    // each end of such a stretch holds both.
    const double spanSeconds = c.timeBase == TimeBase::kTrigger ? 0.25 : 2.0;
    const auto delayColumns = static_cast<std::size_t>(
        std::ceil(effect.delaySeconds / spanSeconds * static_cast<double>(kColumns)));
    EXPECT_GT(lane.same, (own.size() / 2) / 2) << "the lane is not this track's, lined up";
    EXPECT_LE(lane.empty, 2 * delayColumns);
    EXPECT_LE(lane.other, 4U);
    for (const float value : trace(*frame, 2)) {
        ASSERT_LE(std::abs(value), 1e-5f) << "the difference is not silent";
    }
    // And with no more audio the frames stop, lined up as the tracks are.
    EXPECT_FALSE(session.produce()) << "frames go on with nothing new to draw";
}

// A change of level is not taken out: it is part of what an effect did. A copy 2.3 dB louder,
// late and flipped is lined up in time and polarity only, so its lane is this track's 2.3 dB
// taller, and the difference is what the level change left: this track times (1 - g), which
// nulls 20·log10|1 - g| below it.
TEST_P(Aligned, LeavesALevelChangeInTheDifference)
{
    const AlignedCase c = GetParam();
    const Effect effect{.gain = std::pow(10.0, 2.3 / 20.0)};
    Session session(c.rate, c.channels);
    joinCopy(session, c.rate, effect);
    showDifference(session, c.timeBase);
    const std::optional<DecodedFrame> frame = playAndShow(session, c.frozen);
    ASSERT_TRUE(frame);
    const LaneMatch match = matchOf(*frame, 1);
    EXPECT_EQ(match.delay, static_cast<float>(effect.delaySamples(c.rate)));
    EXPECT_EQ(match.polarity, -1.0f);
    EXPECT_NEAR(frame->nullDepth, 20.0 * std::log10(effect.gain - 1.0), 0.01);

    ASSERT_EQ(frame->mode, FrameMode::kMinMax);
    const std::span<const float> own = trace(*frame, 0);
    std::vector<float> taller(own.begin(), own.end());
    std::vector<float> left(own.begin(), own.end());
    for (std::size_t point = 0; point + 1 < own.size(); point += 2) {
        taller[point] = static_cast<float>(effect.gain) * own[point];
        taller[point + 1] = static_cast<float>(effect.gain) * own[point + 1];
        // This track less the louder copy: turned over, so a column's lowest is its highest.
        left[point] = static_cast<float>(1.0 - effect.gain) * own[point + 1];
        left[point + 1] = static_cast<float>(1.0 - effect.gain) * own[point];
    }
    const LaneFit lane = fitOf(trace(*frame, 1), taller);
    EXPECT_GT(lane.same, (own.size() / 2) / 2) << "the lane is not the copy at its own level";
    EXPECT_LE(lane.other, 4U);
    const LaneFit difference = fitOf(trace(*frame, 2), left);
    EXPECT_GT(difference.same, (own.size() / 2) / 2) << "the difference is not the level change";
    EXPECT_LE(difference.other, 4U);
}

// With Align off the difference is of the tracks as recorded, and the other track's lane is its
// audio as recorded, as the sum's view draws it; the label still says what Align would take out.
TEST_P(Aligned, ShowsTheTracksAsRecordedWithAlignOff)
{
    const AlignedCase c = GetParam();
    const Effect effect;
    const auto show = [&](Session& session, protocol::CompareLane lane) {
        joinCopy(session, c.rate, effect);
        showDifference(session, c.timeBase);
        showLane(session.own(), lane);
        setAlign(session.own(), false);
        return playAndShow(session, c.frozen);
    };
    Session session(c.rate, c.channels);
    const std::optional<DecodedFrame> frame = show(session, protocol::CompareLane::kDifference);
    Session summed(c.rate, c.channels);
    const std::optional<DecodedFrame> sum = show(summed, protocol::CompareLane::kSum);
    ASSERT_TRUE(frame);
    ASSERT_TRUE(sum);

    const LaneMatch match = matchOf(*frame, 1);
    EXPECT_EQ(match.delay, static_cast<float>(effect.delaySamples(c.rate)));
    EXPECT_EQ(match.polarity, -1.0f);
    // Noise and its late copy are unrelated as they are: their energies add.
    EXPECT_NEAR(frame->nullDepth, 10.0 * std::log10(1.0 + effect.gain * effect.gain), 0.5);
    if (c.timeBase != TimeBase::kTrigger) { // A triggered window is another in each session.
        EXPECT_TRUE(std::ranges::equal(trace(*frame, 1), trace(*sum, 1)))
            << "the other track's lane is not its audio as recorded";
    }
}

// A track that has nothing to do with this one is not lined up with it: its lane and the
// difference are of the audio as recorded, and the frame has no delay or gain for it.
TEST_P(Aligned, LeavesAnUnrelatedTrackAsRecorded)
{
    const AlignedCase c = GetParam();
    const auto show = [&](Session& session, protocol::CompareLane lane) {
        session.join({.kind = Signal::Kind::kNoise, .amplitude = 0.2, .offset = 0.0, .seed = 3});
        session.join({.kind = Signal::Kind::kNoise, .amplitude = 0.2, .offset = 0.0, .seed = 5});
        showDifference(session, c.timeBase);
        showLane(session.own(), lane);
        return playAndShow(session, c.frozen);
    };
    Session session(c.rate, c.channels);
    const std::optional<DecodedFrame> frame = show(session, protocol::CompareLane::kDifference);
    Session summed(c.rate, c.channels);
    const std::optional<DecodedFrame> sum = show(summed, protocol::CompareLane::kSum);
    ASSERT_TRUE(frame);
    ASSERT_TRUE(sum);

    const LaneMatch match = matchOf(*frame, 1);
    EXPECT_EQ(match.polarity, 0.0f);
    EXPECT_TRUE(std::isnan(match.delay)) << match.delay;
    EXPECT_NEAR(frame->nullDepth, 10.0 * std::log10(2.0), 0.5);
    if (c.timeBase != TimeBase::kTrigger) {
        EXPECT_TRUE(std::ranges::equal(trace(*frame, 1), trace(*sum, 1)))
            << "the other track's lane is not its audio as recorded";
    }
}

// Instances before, in the middle of and after a chain: this track and two copies of it, each late
// by its own delay, one of them flipped, at a half and at a quarter of its level. Each is lined up
// by its own delay, so the difference is the quarter this track has left over, 12 dB below it,
// and it is empty wherever either copy's audio has not arrived: not this track less one copy
// alone there, which would read as a large difference. Whichever copy is the later one.
TEST_P(Aligned, LinesUpEachOfTwoTracksByItsOwnDelay)
{
    const AlignedCase c = GetParam();
    const Effect longer{.gain = 0.5};
    const Effect shorter{.delaySeconds = 0.012, .gain = 0.25, .flipped = false};
    for (const bool longerFirst : {true, false}) {
        SCOPED_TRACE(longerFirst ? "the later copy first" : "the later copy last");
        const std::array<Effect, 2> effects{longerFirst ? longer : shorter,
                                            longerFirst ? shorter : longer};
        Session session(c.rate, c.channels);
        session.join(beforeEffect({}));
        session.join(afterEffect(c.rate, effects[0]));
        session.join(afterEffect(c.rate, effects[1]));
        showDifference(session, c.timeBase);
        const std::optional<DecodedFrame> frame = playAndShow(session, c.frozen);
        ASSERT_TRUE(frame);
        ASSERT_EQ(frame->laneCount, 4);
        for (std::size_t track = 1; track <= effects.size(); ++track) {
            const Effect& effect = effects.at(track - 1);
            const LaneMatch match = matchOf(*frame, track);
            EXPECT_EQ(match.delay, static_cast<float>(effect.delaySamples(c.rate)))
                << "track " << track;
            EXPECT_EQ(match.polarity, effect.flipped ? -1.0f : 1.0f) << "track " << track;
        }
        EXPECT_NEAR(frame->nullDepth, 20.0 * std::log10(0.25), 0.01);

        ASSERT_EQ(frame->mode, FrameMode::kMinMax);
        const std::span<const float> own = trace(*frame, 0);
        std::vector<float> left(own.begin(), own.end());
        for (float& value : left) {
            value *= 0.25f;
        }
        const double spanSeconds = c.timeBase == TimeBase::kTrigger ? 0.25 : 2.0;
        const auto delayColumns = static_cast<std::size_t>(
            std::ceil(longer.delaySeconds / spanSeconds * static_cast<double>(kColumns)));
        const LaneFit difference = fitOf(trace(*frame, 3), left);
        EXPECT_GT(difference.same, (own.size() / 2) / 2)
            << "the difference is not what the two copies leave";
        EXPECT_LE(difference.empty, 2 * delayColumns);
        EXPECT_LE(difference.other, 4U) << "a difference from one copy alone";
    }
}

std::vector<AlignedCase> alignedCases(double highestRate)
{
    std::vector<AlignedCase> cases;
    for (std::size_t rate = 0; rate < kSampleRates.size(); ++rate) {
        if (kSampleRates.at(rate) > highestRate) {
            continue;
        }
        for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
            for (const bool frozen : {false, true}) {
                cases.push_back({.rate = kSampleRates.at(rate),
                                 .channels = (rate + timeBase) % 2 == 0 ? 2 : 1,
                                 .timeBase = static_cast<TimeBase>(timeBase),
                                 .frozen = frozen});
            }
        }
    }
    return cases;
}

std::string alignedName(const ::testing::TestParamInfo<AlignedCase>& info)
{
    const AlignedCase& c = info.param;
    return rateName(c.rate) + "_" + timeBaseName(c.timeBase) +
           (c.channels == 1 ? "_Mono" : "_Stereo") + (c.frozen ? "_Frozen" : "_Live");
}

INSTANTIATE_TEST_SUITE_P(EveryMode, Aligned, ::testing::ValuesIn(alignedCases(kSampleRates.back())),
                         alignedName);

class AlignedBetweenSamples : public ::testing::TestWithParam<AlignedCase>
{
};

// An effect's delay is seldom a whole number of samples (a filter's, or an oversampled
// limiter's). Late by 0.37 of a sample beyond the 46 ms, the copy is read between its samples:
// the frame's delay says so, and the difference still nulls far below anything a whole-sample
// shift leaves (-9 dB here, a third of a sample off across the band), for audio below a third of
// the sample rate, which the filter that reads between the samples passes.
TEST_P(AlignedBetweenSamples, NullsACopyLateByAFractionOfASample)
{
    const AlignedCase c = GetParam();
    const Effect effect{.fraction = 0.37, .lowPass = 1.0 / 3.0};
    Session session(c.rate, c.channels);
    joinCopy(session, c.rate, effect);
    showDifference(session, c.timeBase);
    const std::optional<DecodedFrame> frame = playAndShow(session, c.frozen);
    ASSERT_TRUE(frame);
    const LaneMatch match = matchOf(*frame, 1);
    EXPECT_NEAR(match.delay, static_cast<double>(effect.delaySamples(c.rate)) + effect.fraction,
                0.01);
    EXPECT_EQ(match.polarity, -1.0f);
    EXPECT_LT(frame->nullDepth, -60.0);
}

INSTANTIATE_TEST_SUITE_P(EveryMode, AlignedBetweenSamples,
                         ::testing::ValuesIn(alignedCases(kSampleRates.back())), alignedName);

// What a new analyzer shows of a session's audio and settings as they stand: frozen at the newest
// audio if so, and with whatever it measures over frames measured.
std::optional<DecodedFrame> freshView(Session& session, bool frozen)
{
    Analyzer fresh(session.own(), session.registry);
    fresh.setColumns(kColumns);
    if (frozen) {
        std::vector<std::byte> live;
        if (!fresh.produce(live)) {
            return std::nullopt;
        }
        fresh.setFrozen(true);
    }
    return settled(fresh);
}

// The same lanes, value for value, with the same null depth and the same match for track 1.
void expectSameView(const DecodedFrame& shown, const DecodedFrame& expected)
{
    EXPECT_TRUE(sameFigure(shown.nullDepth, expected.nullDepth))
        << shown.nullDepth << " for " << expected.nullDepth;
    const LaneMatch match = matchOf(shown, 1);
    const LaneMatch fresh = matchOf(expected, 1);
    EXPECT_TRUE(sameFigure(match.delay, fresh.delay)) << match.delay << " for " << fresh.delay;
    EXPECT_EQ(match.polarity, fresh.polarity);
    ASSERT_EQ(tracesIn(shown), tracesIn(expected));
    ASSERT_EQ(shown.pointCount, expected.pointCount);
    for (std::size_t index = 0; index < tracesIn(expected); ++index) {
        EXPECT_TRUE(std::ranges::equal(trace(shown, index), trace(expected, index)))
            << (index + 1 == tracesIn(expected) ? "the difference" : "a track's lane") << " "
            << index;
    }
}

// Whether a trace is silent to within rounding.
bool silent(std::span<const float> values)
{
    return std::ranges::all_of(values, [](float value) { return std::abs(value) <= 1e-5f; });
}

// The difference and its depth are this track's wherever its lane is. Opened on a track the host
// lists after the other one (or on the second of two instances on one track), this track is the
// second lane. The other is this track at half its level, so the difference is the half left,
// 6 dB below this track; taken against the other track it would read level with it, 0 dB.
TEST(DifferenceView, IsThisTracksOnALaterLane)
{
    for (std::size_t index = 0; index < protocol::kTimeBaseCount; ++index) {
        for (const bool align : {false, true}) {
            const auto timeBase = static_cast<TimeBase>(index);
            SCOPED_TRACE(timeBaseName(timeBase) + (align ? ", Align on" : ", Align off"));
            Session session(48000.0, 2);
            session.join(
                {.kind = Signal::Kind::kNoise, .amplitude = 0.5, .offset = 0.0, .seed = 3});
            const auto other = session.join(
                {.kind = Signal::Kind::kNoise, .amplitude = 0.25, .offset = 0.0, .seed = 3});
            other->setTrackInfo({.name = "Before", .colour = std::nullopt, .index = 0});
            session.own().setTrackInfo({.name = "After", .colour = std::nullopt, .index = 1});
            showDifference(session, timeBase);
            setAlign(session.own(), align);
            const std::optional<DecodedFrame> frame = playAndShow(session, false);
            ASSERT_TRUE(frame);
            const LaneSet lanes = session.analyzer().laneSet();
            ASSERT_EQ(lanes.lanes.size(), 2U);
            ASSERT_TRUE(lanes.lanes[1].own) << "this track is not the second lane";
            EXPECT_NEAR(frame->nullDepth, 20.0 * std::log10(0.5), 1e-3);
            const std::span<const float> own = trace(*frame, 1);
            const std::span<const float> difference = trace(*frame, 2);
            EXPECT_FALSE(silent(own)) << "nothing drawn to compare";
            for (std::size_t point = 0; point < own.size(); ++point) {
                ASSERT_EQ(difference[point], 0.5f * own[point]) << "at point " << point;
            }
        }
    }
}

// Shapes are compared zoomed to a few samples, where the view shows far less than the delay: the
// alignment is the routing's, found over audio the view does not choose, so it stays, and the few
// samples shown are this track's in both lanes with nothing left between them. Frozen, the zoom
// makes no search due.
TEST(AlignedView, KeepsItsAlignmentZoomedToAFewSamples)
{
    constexpr double kRate = 48000.0;
    const Effect effect;
    for (std::size_t index = 0; index < protocol::kTimeBaseCount; ++index) {
        for (const bool frozen : {false, true}) {
            const auto timeBase = static_cast<TimeBase>(index);
            SCOPED_TRACE(timeBaseName(timeBase) + (frozen ? " frozen" : " live"));
            Session session(kRate, 2);
            joinCopy(session, kRate, effect);
            showDifference(session, timeBase);
            ASSERT_TRUE(playAndShow(session, frozen));
            const double spanSamples = (timeBase == TimeBase::kTrigger ? 0.25 : 2.0) * kRate;
            const double width = protocol::kMinimumViewSamples / spanSamples * (1.0 - 1e-12);
            const AnalysisWork before = session.analyzer().work();
            // A quarter into the span: away from the newest audio, which the late track lacks.
            session.analyzer().setView(0.25, 0.25 + width);
            std::optional<DecodedFrame> frame = session.produce();
            ASSERT_TRUE(frame);
            if (auto later = settled(session.analyzer())) {
                frame = std::move(later);
            }
            if (frozen) {
                EXPECT_EQ(workSince(before, session.analyzer().work()).matchSearches, 0U)
                    << "the zoom made a search due";
            } else {
                session.play(1.0, 0.05);
                session.play(0.01);
                frame = session.produce();
                ASSERT_TRUE(frame);
            }
            EXPECT_NEAR(frame->visibleSamples, protocol::kMinimumViewSamples, 0.01);
            const LaneMatch match = matchOf(*frame, 1);
            EXPECT_EQ(match.delay, static_cast<float>(effect.delaySamples(kRate)));
            EXPECT_EQ(match.polarity, -1.0f);
            EXPECT_LE(frame->nullDepth, protocol::kGainFloorDb);
            ASSERT_EQ(frame->mode, FrameMode::kRaw);
            const std::span<const float> own = trace(*frame, 0);
            const std::span<const float> lane = trace(*frame, 1);
            EXPECT_FALSE(silent(own)) << "nothing drawn to compare";
            for (std::size_t point = 0; point < own.size(); ++point) {
                ASSERT_NEAR(lane[point], own[point], 1e-5f) << "at point " << point;
            }
            EXPECT_TRUE(silent(trace(*frame, 2))) << "the difference is not silent";
        }
    }
}

// Frozen in Trigger at its shortest span, half a millisecond, and panned back to the oldest audio
// held: what is shown is seconds from the audio the alignment was found over. It still is the
// routing's: no search is made due, the tracks stay lined up and the difference silent.
TEST(AlignedView, KeepsItsAlignmentWhilePanned)
{
    constexpr double kRate = 48000.0;
    const Effect effect;
    Session session(kRate, 2);
    joinCopy(session, kRate, effect);
    showDifference(session, TimeBase::kTrigger);
    setSpan(session.own(), TimeBase::kTrigger, setting(SettingId::kTriggerSpan).minimum);
    session.play(protocol::kFrozenHoldSeconds + 0.5, 0.05);
    session.play(0.01);
    ASSERT_TRUE(session.produce());
    session.analyzer().setFrozen(true);
    ASSERT_TRUE(session.produce());
    const float reach = session.field(FrameHeader::kPanReachOffset);
    ASSERT_GT(reach, 1.0f);
    (void)settled(session.analyzer()); // Whatever the freeze left to measure.
    const AnalysisWork before = session.analyzer().work();
    session.analyzer().setPan(reach);
    std::optional<DecodedFrame> frame = session.produce();
    ASSERT_TRUE(frame);
    if (auto later = settled(session.analyzer())) {
        frame = std::move(later);
    }
    EXPECT_EQ(workSince(before, session.analyzer().work()).matchSearches, 0U)
        << "the pan made a search due";
    const LaneMatch match = matchOf(*frame, 1);
    EXPECT_EQ(match.delay, static_cast<float>(effect.delaySamples(kRate)));
    EXPECT_LE(frame->nullDepth, protocol::kGainFloorDb);
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    const std::span<const float> own = trace(*frame, 0);
    const std::span<const float> lane = trace(*frame, 1);
    EXPECT_FALSE(silent(own)) << "nothing drawn to compare";
    for (std::size_t point = 0; point < own.size(); ++point) {
        ASSERT_NEAR(lane[point], own[point], 1e-5f) << "at point " << point;
    }
    EXPECT_TRUE(silent(trace(*frame, 2))) << "the difference is not silent";
}

// Align switched off, and on again, with no audio in between: each time the lanes, the
// difference and its depth are what a session that opened that way shows, once redrawn.
TEST(AlignedView, TurningAlignOffAndOnMatchesAFreshSession)
{
    constexpr double kRate = 48000.0;
    for (const TimeBase timeBase : {TimeBase::kBeats, TimeBase::kFree}) {
        for (const bool frozen : {false, true}) {
            SCOPED_TRACE(timeBaseName(timeBase) + (frozen ? " frozen" : " live"));
            Session session(kRate, 2);
            joinCopy(session, kRate, {});
            showDifference(session, timeBase);
            const std::optional<DecodedFrame> aligned = playAndShow(session, frozen);
            ASSERT_TRUE(aligned);
            ASSERT_LE(aligned->nullDepth, protocol::kGainFloorDb);
            for (const bool on : {false, true}) {
                SCOPED_TRACE(on ? "on again" : "off");
                setAlign(session.own(), on);
                const std::optional<DecodedFrame> shown = settled(session.analyzer());
                const std::optional<DecodedFrame> expected = freshView(session, frozen);
                ASSERT_TRUE(shown);
                ASSERT_TRUE(expected);
                if (on) {
                    ASSERT_LE(expected->nullDepth, protocol::kGainFloorDb);
                } else {
                    ASSERT_GT(expected->nullDepth, 0.0f);
                }
                expectSameView(*shown, *expected);
            }
        }
    }
}

// The routing changes while it plays: the effect's latency (another setting of its look-ahead),
// or its polarity. Once the searches see only the new routing, the track is drawn lined up by it,
// every column of its lane and of the difference, as a session opened then draws them: none left
// from the alignment before.
TEST(AlignedView, DrawsTheLanesAgainWhenTheAlignmentMoves)
{
    constexpr double kRate = 48000.0;
    const Effect first;
    const std::array<Effect, 2> seconds{Effect{.delaySeconds = 0.031}, Effect{.flipped = false}};
    for (const TimeBase timeBase : {TimeBase::kBeats, TimeBase::kFree}) {
        for (const Effect& second : seconds) {
            SCOPED_TRACE(timeBaseName(timeBase) + (second.flipped == first.flipped
                                                       ? ", the delay moves"
                                                       : ", the polarity turns"));
            Session session(kRate, 2);
            joinCopy(session, kRate, first);
            showDifference(session, timeBase);
            const std::optional<DecodedFrame> before = playAndShow(session, false);
            ASSERT_TRUE(before);
            ASSERT_EQ(matchOf(*before, 1).delay, static_cast<float>(first.delaySamples(kRate)));
            session.setSignal(1, afterEffect(kRate, second));
            // More than the search's window: its audio is all of the new routing.
            session.play(kMatchWindowSeconds + 2.0, 0.05);
            session.play(0.01);
            std::optional<DecodedFrame> shown = session.produce();
            if (auto later = settled(session.analyzer())) {
                shown = std::move(later);
            }
            const std::optional<DecodedFrame> expected = freshView(session, false);
            ASSERT_TRUE(shown);
            ASSERT_TRUE(expected);
            const LaneMatch match = matchOf(*expected, 1);
            ASSERT_EQ(match.delay, static_cast<float>(second.delaySamples(kRate)));
            ASSERT_EQ(match.polarity, second.flipped ? -1.0f : 1.0f);
            ASSERT_LE(expected->nullDepth, protocol::kGainFloorDb);
            expectSameView(*shown, *expected);
        }
    }
}

// A track lined up between its samples is filtered once a frame, over what that frame's new audio
// reaches, for everything drawn and measured from it. Every column drawn from that is the
// lined-up track's: its lane is this track's or empty, and the difference stays 40 dB or more
// below the tracks in every column, seen from either track. None is this track alone (which
// reads as a large difference), nor audio from beside what was filtered: neither once the lanes
// have been drawn again after Align comes on while it plays, nor in any frame after. And while
// they are drawn again, over several frames, the lane is on screen in every one of them. Align
// comes on mid-bar at 48 kHz and, at 96 kHz, where a bar is more than a frame filters at once,
// in the frame before the sweep starts over, so that it does while the lanes are being drawn
// again far ahead of the playhead.
TEST(AlignedView, NullsInEveryColumnWhileATrackIsLinedUpBetweenSamples)
{
    struct Case
    {
        double rate;
        double alignOnAt; // Seconds played, of 2 s bars.
    };
    constexpr float kLeft = 0.002f; // 40 dB below the noise's 0.2.
    const auto expectNulled = [&](const DecodedFrame& frame) {
        ASSERT_EQ(frame.mode, FrameMode::kMinMax);
        const std::span<const float> own = trace(frame, 0);
        const std::span<const float> lane = trace(frame, 1);
        std::size_t neither = 0;
        for (std::size_t point = 0; point + 1 < own.size(); point += 2) {
            const bool same = std::abs(lane[point] - own[point]) <= kLeft &&
                              std::abs(lane[point + 1] - own[point + 1]) <= kLeft;
            const bool empty = lane[point] == 0.0f && lane[point + 1] == 0.0f;
            neither += same || empty ? 0 : 1;
        }
        // A column at each end of an empty stretch holds both.
        ASSERT_LE(neither, 4U) << "the lane is not this track's";
        for (const float value : trace(frame, 2)) {
            ASSERT_LE(std::abs(value), kLeft) << "not nulled";
        }
    };
    for (const Case& c :
         {Case{.rate = 48000.0, .alignOnAt = 3.0}, Case{.rate = 96000.0, .alignOnAt = 1.96}}) {
        for (const bool fromCopy : {false, true}) {
            SCOPED_TRACE(rateName(c.rate) +
                         (fromCopy ? " from the late track" : " from the early track"));
            const Effect effect{.fraction = 0.37, .lowPass = 1.0 / 3.0, .fromCopy = fromCopy};
            Session session(c.rate, 2);
            joinCopy(session, c.rate, effect);
            showDifference(session, TimeBase::kBeats);
            // Align comes on while it plays, with both tracks on screen as recorded: the lanes
            // are drawn again over the next frames, beside each frame's new audio; then the rest
            // of them, with no more audio.
            setAlign(session.own(), false);
            session.play(c.alignOnAt, 0.05);
            setAlign(session.own(), true);
            std::optional<DecodedFrame> redrawn;
            for (int frames = 0; frames < 10; ++frames) {
                session.play(0.05);
                redrawn = session.produce();
                ASSERT_TRUE(redrawn);
                ASSERT_FALSE(silent(trace(*redrawn, 1)))
                    << "the lane went empty while it was drawn again, frame " << frames;
            }
            if (auto later = settled(session.analyzer())) {
                redrawn = std::move(later);
            }
            ASSERT_TRUE(redrawn);
            {
                SCOPED_TRACE("drawn again after Align came on");
                expectNulled(*redrawn);
            }
            // On through a pass of the bar, a frame every few blocks: each drawn from what the
            // frames before it left.
            for (int frames = 0; frames < 60; ++frames) {
                SCOPED_TRACE(testing::Message() << "frame " << frames);
                session.play(0.05);
                const std::optional<DecodedFrame> frame = session.produce();
                ASSERT_TRUE(frame);
                expectNulled(*frame);
                if (::testing::Test::HasFatalFailure()) {
                    return;
                }
            }
        }
    }
}

// The effect's delay moves by less than a sample (another setting of its oversampling), the whole
// samples and the polarity staying: the lanes and the difference are drawn again by the new
// delay, every column, as a session opened then draws them. Over sixteen bars, where what was
// drawn before the new delay was found is still on screen once it has been: none is left as
// filtered for the old one, which would stand out of the null.
TEST(AlignedView, DrawsTheLanesAgainWhenTheDelayMovesByLessThanASample)
{
    constexpr double kRate = 48000.0;
    constexpr float kLeft = 0.002f; // 40 dB below the noise's 0.2.
    const Effect first{.fraction = 0.2, .lowPass = 1.0 / 3.0};
    const Effect second{.fraction = 0.6, .lowPass = 1.0 / 3.0};
    Session session(kRate, 2);
    joinCopy(session, kRate, first);
    showDifference(session, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kSixteenBars));
    const std::optional<DecodedFrame> before = playAndShow(session, false);
    ASSERT_TRUE(before);
    ASSERT_NEAR(matchOf(*before, 1).delay,
                static_cast<double>(first.delaySamples(kRate)) + first.fraction, 0.01);
    session.setSignal(1, afterEffect(kRate, second));
    // More than the search's window: its audio is all of the new routing.
    session.play(kMatchWindowSeconds + 2.0, 0.05);
    session.play(0.01);
    std::optional<DecodedFrame> shown = session.produce();
    if (auto later = settled(session.analyzer())) {
        shown = std::move(later);
    }
    const std::optional<DecodedFrame> expected = freshView(session, false);
    ASSERT_TRUE(shown);
    ASSERT_TRUE(expected);
    const double moved = static_cast<double>(second.delaySamples(kRate)) + second.fraction;
    ASSERT_NEAR(matchOf(*expected, 1).delay, moved, 0.01);
    EXPECT_NEAR(matchOf(*shown, 1).delay, moved, 0.01);
    ASSERT_EQ(tracesIn(*shown), tracesIn(*expected));
    ASSERT_EQ(shown->pointCount, expected->pointCount);
    for (std::size_t index = 1; index < tracesIn(*expected); ++index) {
        const std::span<const float> drawn = trace(*shown, index);
        const std::span<const float> fresh = trace(*expected, index);
        for (std::size_t point = 0; point < fresh.size(); ++point) {
            ASSERT_NEAR(drawn[point], fresh[point], kLeft)
                << (index == 1 ? "the track's lane" : "the difference") << ", point " << point;
        }
    }
}

// Seen from the earlier track, the other track's lane ends where its newest audio is drawn, the
// delay before the playhead, and is empty from there to the playhead. Zoomed in on that end, far
// from the playhead itself, the points between its samples within a reach of it are drawn
// straight, as at any join: the wave does not ring into the empty stretch. Farther back the lane
// is the wave.
TEST(AlignedView, DrawsNoWaveAcrossALinedUpLanesNewestAudio)
{
    constexpr double kRate = 48000.0;
    const Effect effect;
    Session session(kRate, 2);
    joinCopy(session, kRate, effect);
    showDifference(session, TimeBase::kBeats);
    // Three quarters into the second bar at 120 BPM.
    session.play(3.5, 0.05);
    session.play(static_cast<double>(kBlock) / kRate);
    std::optional<DecodedFrame> frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(matchOf(*frame, 1).delay, static_cast<float>(effect.delaySamples(kRate)));
    ASSERT_GE(frame->cursor, 0.0f);
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const double width = 4.0 * protocol::kMinimumViewSamples / spanSamples;
    const double end = static_cast<double>(frame->cursor) -
                       (static_cast<double>(effect.delaySamples(kRate)) / spanSamples);
    const double start = end - (width / 2.0);
    ASSERT_GT(start, 0.0);
    const double cursor = frame->cursor;
    session.analyzer().setView(start, start + width);
    frame = session.produce();
    ASSERT_TRUE(frame);
    if (auto later = settled(session.analyzer())) {
        frame = std::move(later);
    }
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    const std::size_t factor = frame->pointsPerSample;
    ASSERT_GT(factor, 1U);
    // Where the lane's newest audio is drawn, in samples from the first point.
    const double newest = ((cursor - frame->viewStart) / (frame->viewEnd - frame->viewStart) *
                           frame->visibleSamples) +
                          frame->startFraction - static_cast<double>(effect.delaySamples(kRate));
    const std::span<const float> values = trace(*frame, 1);
    const std::size_t samples = ((values.size() - 1) / factor) + 1;
    ASSERT_GT(newest, 2.0 * kReconstructionReach) << "the lane's end is in the view";
    ASSERT_LT(newest, static_cast<double>(samples) - (2.0 * kReconstructionReach));
    const auto straight = [&](std::size_t n) {
        const float from = values[n * factor];
        const float to = values[(n + 1) * factor];
        for (std::size_t phase = 1; phase < factor; ++phase) {
            const float line =
                from + ((to - from) * static_cast<float>(phase) / static_cast<float>(factor));
            if (std::abs(values[(n * factor) + phase] - line) > 1e-5f) {
                return false;
            }
        }
        return true;
    };
    std::size_t curvedBefore = 0;
    std::size_t before = 0;
    for (std::size_t n = 0; n + 1 < samples; ++n) {
        const double distance = (static_cast<double>(n) + 0.5) - newest;
        if (std::abs(distance) < static_cast<double>(kReconstructionReach) - 2.0) {
            EXPECT_TRUE(straight(n)) << "gap " << n << ", " << distance << " from the lane's end";
        } else if (distance < -static_cast<double>(kReconstructionReach) - 2.0) {
            ++before;
            curvedBefore += straight(n) ? 0 : 1;
        }
    }
    EXPECT_GT(curvedBefore, before / 2) << "away from its end, the wave";
}

// Compare switched off with a lined-up track on show: its lane is drawn as recorded again, every
// column, as a session opened then draws it. Live, and frozen, where no later frame would finish
// it.
TEST(AlignedView, DrawsTheLanesAsRecordedOnceCompareIsOff)
{
    constexpr double kRate = 48000.0;
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        Session session(kRate, 2);
        joinCopy(session, kRate, {});
        showDifference(session, TimeBase::kBeats);
        const std::optional<DecodedFrame> aligned = playAndShow(session, frozen);
        ASSERT_TRUE(aligned);
        ASSERT_LE(aligned->nullDepth, protocol::kGainFloorDb);
        show(session.own().settings(), Display::kMulti);
        const std::optional<DecodedFrame> shown = settled(session.analyzer());
        const std::optional<DecodedFrame> expected = freshView(session, frozen);
        ASSERT_TRUE(shown);
        ASSERT_TRUE(expected);
        ASSERT_EQ(tracesIn(*shown), 2U);
        ASSERT_EQ(tracesIn(*expected), 2U);
        EXPECT_FALSE(std::ranges::equal(trace(*shown, 1), trace(*aligned, 1)))
            << "the other track's lane is still drawn lined up";
        for (std::size_t index = 0; index < 2; ++index) {
            EXPECT_TRUE(std::ranges::equal(trace(*shown, index), trace(*expected, index)))
                << "lane " << index;
        }
    }
}

// The effect's polarity is turned while the sum is on show, then the difference comes back: the
// track is at the same delay as before, and lined up with its new polarity all the same, so the
// difference nulls again. It does not stay lined up as it was, which would leave the two tracks
// adding up, 6 dB above this one, under a label saying the track is not flipped.
TEST(AlignedView, TakesUpAPolarityTurnedWhileTheSumWasShown)
{
    constexpr double kRate = 48000.0;
    for (const TimeBase timeBase : {TimeBase::kBeats, TimeBase::kFree}) {
        SCOPED_TRACE(timeBaseName(timeBase));
        Session session(kRate, 2);
        joinCopy(session, kRate, {});
        showDifference(session, timeBase);
        const std::optional<DecodedFrame> before = playAndShow(session, false);
        ASSERT_TRUE(before);
        ASSERT_EQ(matchOf(*before, 1).polarity, -1.0f);
        ASSERT_LE(before->nullDepth, protocol::kGainFloorDb);

        showLane(session.own(), protocol::CompareLane::kSum);
        session.setSignal(1, afterEffect(kRate, {.flipped = false}));
        // More than the search's window: its audio is all of the new routing.
        session.play(kMatchWindowSeconds + 2.0, 0.05);
        showLane(session.own(), protocol::CompareLane::kDifference);
        const std::optional<DecodedFrame> after = playAndShow(session, false);
        ASSERT_TRUE(after);
        const LaneMatch match = matchOf(*after, 1);
        EXPECT_EQ(match.delay, static_cast<float>(Effect{}.delaySamples(kRate)));
        EXPECT_EQ(match.polarity, 1.0f);
        EXPECT_LE(after->nullDepth, protocol::kGainFloorDb);
    }
}

// Seen from the track after the effect, the other one is read earlier than this one. In the
// first second of a session, before the roll is full, that is audio from before it began for the
// first few milliseconds: the other track's lane is empty there and this track's, lined up, from
// there on, with the difference silent. It is not empty all the way for want of those first
// milliseconds.
TEST(AlignedView, StartsALinedUpLaneWhereItsAudioDoes)
{
    constexpr double kRate = 48000.0;
    const Effect effect{.fromCopy = true};
    Session session(kRate, 2);
    joinCopy(session, kRate, effect);
    showDifference(session, TimeBase::kFree);
    session.play(1.0, 0.05);
    session.play(0.01);
    const std::optional<DecodedFrame> frame = session.produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(matchOf(*frame, 1).delay, -static_cast<float>(effect.delaySamples(kRate)));
    ASSERT_EQ(frame->mode, FrameMode::kMinMax);
    const std::span<const float> own = trace(*frame, 0);
    const std::span<const float> lane = trace(*frame, 1);
    std::size_t sounding = 0;
    std::size_t linedUp = 0;
    for (std::size_t point = 0; point + 1 < own.size(); point += 2) {
        if (own[point] == 0.0f && own[point + 1] == 0.0f) {
            continue;
        }
        ++sounding;
        linedUp += std::abs(lane[point] - own[point]) <= 1e-5f &&
                           std::abs(lane[point + 1] - own[point + 1]) <= 1e-5f
                       ? 1
                       : 0;
    }
    // The roll shows the second it holds so far.
    const auto delayColumns = static_cast<std::size_t>(
        std::ceil(static_cast<double>(effect.delaySamples(kRate)) /
                  static_cast<double>(frame->visibleSamples) * static_cast<double>(kColumns)));
    ASSERT_GT(sounding, kColumns / 3);
    EXPECT_GE(linedUp + delayColumns + 2, sounding) << "the lane is not this track's, lined up";
    EXPECT_TRUE(silent(trace(*frame, 2))) << "the difference is not silent";
}

// The depth is measured over all of the view in which the lined-up tracks have their audio, not
// over its start: an effect that changes halfway through it (here the copy turns four times as
// loud) leaves a difference three times this track over the newer half, 6.5 dB above this track
// over the whole view, where its first half alone nulls.
TEST(AlignedView, MeasuresTheDepthOverAllOfTheView)
{
    constexpr double kRate = 48000.0;
    const Effect effect;
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        Session session(kRate, 2);
        session.join(beforeEffect(effect));
        Signal copy = afterEffect(kRate, effect);
        // A second before the newest audio played below, of the two shown.
        copy.louderFrom = std::llround(3.03 * kRate);
        session.join(copy);
        showDifference(session, TimeBase::kFree);
        const std::optional<DecodedFrame> frame = playAndShow(session, frozen);
        ASSERT_TRUE(frame);
        EXPECT_EQ(matchOf(*frame, 1).delay, static_cast<float>(effect.delaySamples(kRate)));
        EXPECT_NEAR(frame->nullDepth, 20.0 * std::log10(3.0 / std::numbers::sqrt2), 0.5);
    }
}

// The other track stops being a copy of this one while it plays (another clip on it, or another
// routing). Once the searches see only the new audio, nothing is left of the alignment: the frame
// has no delay for the track, and its lane and the difference are of the audio as recorded, as a
// session opened then draws them, not shifted and flipped by what was found before.
TEST(AlignedView, DropsTheAlignmentOfATrackNoLongerRelated)
{
    constexpr double kRate = 48000.0;
    for (const TimeBase timeBase : {TimeBase::kBeats, TimeBase::kFree}) {
        SCOPED_TRACE(timeBaseName(timeBase));
        Session session(kRate, 2);
        joinCopy(session, kRate, {});
        showDifference(session, timeBase);
        const std::optional<DecodedFrame> before = playAndShow(session, false);
        ASSERT_TRUE(before);
        ASSERT_EQ(matchOf(*before, 1).polarity, -1.0f);
        session.setSignal(
            1, {.kind = Signal::Kind::kNoise, .amplitude = 0.2, .offset = 0.0, .seed = 5});
        // More than the search's window: its audio is all of the new track.
        session.play(kMatchWindowSeconds + 2.0, 0.05);
        session.play(0.01);
        std::optional<DecodedFrame> shown = session.produce();
        if (auto later = settled(session.analyzer())) {
            shown = std::move(later);
        }
        const std::optional<DecodedFrame> expected = freshView(session, false);
        ASSERT_TRUE(shown);
        ASSERT_TRUE(expected);
        const LaneMatch match = matchOf(*shown, 1);
        EXPECT_EQ(match.polarity, 0.0f);
        EXPECT_TRUE(std::isnan(match.delay)) << match.delay;
        ASSERT_GT(expected->nullDepth, 0.0f);
        expectSameView(*shown, *expected);
    }
}

// An alignment is of the channel it was found on. On another lane channel the track is not
// lined up by it: its match and its alignment are none until that channel's are found, the frame
// has no delay for it, and its lane is its audio as recorded meanwhile.
TEST(AlignedView, DoesNotLineATrackUpByAnotherChannelsAlignment)
{
    constexpr double kRate = 96000.0;
    const Effect effect;
    Session session(kRate, 2);
    joinCopy(session, kRate, effect);
    showDifference(session, TimeBase::kFree);
    const std::optional<DecodedFrame> aligned = playAndShow(session, false);
    ASSERT_TRUE(aligned);
    ASSERT_EQ(matchOf(*aligned, 1).delay, static_cast<float>(effect.delaySamples(kRate)));

    session.own().settings().set(SettingId::kLaneChannel,
                                 static_cast<double>(protocol::ChannelView::kRight));
    const std::optional<DecodedFrame> changed = session.produce();
    ASSERT_TRUE(changed);
    const LaneMatch none = matchOf(*changed, 1);
    EXPECT_EQ(none.polarity, 0.0f);
    EXPECT_TRUE(std::isnan(none.delay)) << none.delay;
    EXPECT_LT(fitOf(trace(*changed, 1), trace(*changed, 0)).same, kColumns / 4)
        << "lined up by the other channel's alignment";

    const std::optional<DecodedFrame> found = settled(session.analyzer());
    ASSERT_TRUE(found);
    EXPECT_EQ(matchOf(*found, 1).delay, static_cast<float>(effect.delaySamples(kRate)));
    EXPECT_LE(found->nullDepth, protocol::kGainFloorDb);
}

// Frozen in Trigger or Free with the difference shown, a compared track's hold has the one
// channel that was shown when it was taken. When the view then asks for another (the host made
// the track mono, or the lane channel is changed), the hold has none of that audio: the track's
// lane is drawn silent, not the other channel's audio lined up by the alignment found before, and
// no alignment is searched in it.
TEST(AlignedView, ReadsNoAudioOfAFrozenTrackHeldInAnotherChannel)
{
    constexpr double kRate = 48000.0;
    enum class Change : std::uint8_t
    {
        kTrackTurnsMono,
        kLaneChannel,
    };
    // Viewed from the track after the effect: the other is lined up by reading it earlier, which
    // from the end of a hold is audio it has.
    const Effect effect{.fromCopy = true};
    for (const TimeBase timeBase : {TimeBase::kTrigger, TimeBase::kFree}) {
        for (const Change change : {Change::kTrackTurnsMono, Change::kLaneChannel}) {
            SCOPED_TRACE(timeBaseName(timeBase) + (change == Change::kTrackTurnsMono
                                                       ? ", the track turns mono"
                                                       : ", another lane channel"));
            Session session(kRate, 2);
            session.join(afterEffect(kRate, effect));
            const auto other = session.join(beforeEffect(effect));
            showDifference(session, timeBase);
            session.own().settings().set(SettingId::kLaneChannel,
                                         static_cast<double>(protocol::ChannelView::kRight));
            const std::optional<DecodedFrame> aligned = playAndShow(session, true);
            ASSERT_TRUE(aligned);
            ASSERT_EQ(matchOf(*aligned, 1).delay, -static_cast<float>(effect.delaySamples(kRate)));
            ASSERT_FALSE(silent(trace(*aligned, 1))) << "nothing drawn before the change";

            const AnalysisWork before = session.analyzer().work();
            if (change == Change::kTrackTurnsMono) {
                other->setInputChannels(1);
                // A frozen view is drawn again when it is zoomed.
                session.analyzer().setView(0.0, 0.5);
            } else {
                session.own().settings().set(SettingId::kLaneChannel,
                                             static_cast<double>(protocol::ChannelView::kLeft));
            }
            std::optional<DecodedFrame> frame = session.produce();
            ASSERT_TRUE(frame);
            if (auto later = settled(session.analyzer())) {
                frame = std::move(later);
            }
            EXPECT_TRUE(silent(trace(*frame, 1))) << "the lane draws audio held in another channel";
            EXPECT_EQ(workSince(before, session.analyzer().work()).matchSearches, 0U)
                << "an alignment was searched in audio held in another channel";
        }
    }
}

// The columns of a lane wholly inside samples [first, end) of a view of `samples` samples.
std::pair<std::size_t, std::size_t> columnsInside(double first, double end, double samples)
{
    const double perColumn = samples / kColumns;
    const double from = std::ceil(std::max(first, 0.0) / perColumn);
    const double to = std::floor(std::min(end, samples) / perColumn);
    return {static_cast<std::size_t>(from), static_cast<std::size_t>(std::max(from, to))};
}

// Whether the columns [first, end) of a min/max trace are all empty.
bool emptyOver(std::span<const float> lane, std::pair<std::size_t, std::size_t> columns)
{
    return std::ranges::all_of(
        lane.subspan(2 * columns.first, 2 * (columns.second - columns.first)),
        [](float value) { return value == 0.0f; });
}

// Viewed from the track before the effect, the other track's audio of the newest moments has not
// arrived yet: over the delay before the newest audio its lane is empty and so is the difference,
// not this track alone, which would read as a large difference; the depth is measured beside it.
// As the audio arrives the columns fill in.
TEST(AlignedView, LeavesTheNewestPartEmptyUntilTheLaterTrackArrives)
{
    constexpr double kRate = 48000.0;
    constexpr double kSpan = 2.0;
    const Effect effect;
    Session session(kRate, 2);
    joinCopy(session, kRate, effect);
    showDifference(session, TimeBase::kFree);
    session.play(4.0, 0.05);
    const double samples = kSpan * kRate;
    const auto delay = static_cast<double>(effect.delaySamples(kRate));
    // The newest columns but one: the last may still be waiting for its neighbours.
    const auto newest = columnsInside(samples - delay, samples, samples);
    ASSERT_GT(newest.second - newest.first, 4U);
    for (int pass = 0; pass < 2; ++pass) {
        SCOPED_TRACE(pass == 0 ? "first" : "half a second on");
        session.play(0.5, 0.05);
        session.play(0.01);
        const std::optional<DecodedFrame> frame = session.produce();
        ASSERT_TRUE(frame);
        const std::span<const float> own = trace(*frame, 0);
        EXPECT_FALSE(emptyOver(own, newest)) << "this track has not played there";
        EXPECT_TRUE(emptyOver(trace(*frame, 1), newest)) << "the late track's lane";
        EXPECT_TRUE(emptyOver(trace(*frame, 2), newest)) << "the difference";
        const LaneFit lane = fitOf(trace(*frame, 1), own);
        EXPECT_GE(lane.same + 2, newest.first) << "the columns before were not filled in";
        EXPECT_TRUE(silent(trace(*frame, 2)));
        EXPECT_LE(frame->nullDepth, protocol::kGainFloorDb);
    }
}

// A tempo-synced sweep holds one pass of the bar, the newest before the playhead and the previous
// one from it on. A track lined up 46 ms earlier or later has no audio of the same moments, and
// is empty with the difference, where the shift reads past the sweep's edge or across the
// playhead; everywhere else it is this track's.
TEST(AlignedView, LeavesAnAlignedLaneEmptyWhereTheSweepLacksItsMoments)
{
    constexpr double kRate = 48000.0;
    for (const bool fromCopy : {false, true}) {
        SCOPED_TRACE(fromCopy ? "viewed from the late track" : "viewed from the early track");
        const Effect effect{.fromCopy = fromCopy};
        Session session(kRate, 2);
        joinCopy(session, kRate, effect);
        showDifference(session, TimeBase::kBeats);
        session.play(4.5, 0.05); // A quarter into the bar of 2 s.
        session.play(0.01);
        const std::optional<DecodedFrame> frame = session.produce();
        ASSERT_TRUE(frame);
        const double samples = 2.0 * kRate;
        const double playhead = frame->cursor * samples;
        const auto delay = static_cast<double>(effect.delaySamples(kRate));
        // The late track's newest audio is missing before the playhead, and its audio after the
        // bar's end at the right edge; the early track's, read later, the other way round.
        const auto atPlayhead = fromCopy ? columnsInside(playhead, playhead + delay, samples)
                                         : columnsInside(playhead - delay, playhead, samples);
        const auto atEdge = fromCopy ? columnsInside(0.0, delay, samples)
                                     : columnsInside(samples - delay, samples, samples);
        const std::span<const float> own = trace(*frame, 0);
        for (const auto& columns : {atPlayhead, atEdge}) {
            ASSERT_GT(columns.second - columns.first, 4U);
            EXPECT_FALSE(emptyOver(own, columns)) << "this track has not played there";
            EXPECT_TRUE(emptyOver(trace(*frame, 1), columns)) << "the other track's lane";
            EXPECT_TRUE(emptyOver(trace(*frame, 2), columns)) << "the difference";
        }
        const LaneFit lane = fitOf(trace(*frame, 1), own);
        const std::size_t empty =
            (atPlayhead.second - atPlayhead.first) + (atEdge.second - atEdge.first);
        EXPECT_GE(lane.same + empty + 4, kColumns) << "the rest is not this track's, lined up";
        EXPECT_TRUE(silent(trace(*frame, 2)));
        EXPECT_LE(frame->nullDepth, protocol::kGainFloorDb);
    }
}

// An effect's latency can exceed the window: 220 ms in a Free view of 100 ms, viewed from the
// track after the effect. The other track's audio of the moments shown lies before the window,
// where its capture (frozen, its hold) still has it: it is read there, so the lane is this
// track's all along and the difference silent.
TEST(AlignedView, ReadsAnAlignedTrackBeyondTheWindowShown)
{
    constexpr double kRate = 48000.0;
    const Effect effect{.delaySeconds = 0.22, .fromCopy = true};
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen ? "frozen" : "live");
        Session session(kRate, 2);
        joinCopy(session, kRate, effect);
        showDifference(session, TimeBase::kFree);
        setSpan(session.own(), TimeBase::kFree, 0.1);
        const std::optional<DecodedFrame> frame = playAndShow(session, frozen);
        ASSERT_TRUE(frame);
        const LaneMatch match = matchOf(*frame, 1);
        EXPECT_EQ(match.delay, -static_cast<float>(effect.delaySamples(kRate)));
        EXPECT_EQ(match.polarity, -1.0f);
        EXPECT_LE(frame->nullDepth, protocol::kGainFloorDb);
        ASSERT_EQ(frame->mode, FrameMode::kMinMax);
        const LaneFit lane = fitOf(trace(*frame, 1), trace(*frame, 0));
        // All of them but for the newest column, which in this track's lane has no samples after
        // it (its newest, live or as held), read for the wave around a peak, and in the other's,
        // read 220 ms back, has them.
        EXPECT_GE(lane.same + 1U, kColumns);
        EXPECT_EQ(lane.empty, 0U);
        EXPECT_FALSE(silent(trace(*frame, 0))) << "nothing drawn to compare";
        EXPECT_TRUE(silent(trace(*frame, 2))) << "the difference is not silent";
    }
}

// The gaps of a raw trace within a reach of its first drawn sample (`atStart`) or its last are
// drawn straight, as where no neighbour is held past an end.
void expectStraightAtTheEnd(std::span<const float> values, std::size_t factor, bool atStart)
{
    const std::size_t samples = ((values.size() - 1) / factor) + 1;
    ASSERT_GT(samples, kReconstructionReach);
    for (std::size_t n = 0; n + 2 < kReconstructionReach; ++n) {
        const std::size_t gap = atStart ? n : samples - 2 - n;
        EXPECT_TRUE(drawnStraight(values, factor, gap)) << "gap " << gap << " of " << samples - 1;
    }
}

// A copy of this track 2 samples late at half its level (an effect's latency), lined up by Align:
// its lane and the difference are read through the alignment, as far as the copy's audio is held,
// and drawn straight where it holds none, beside an end, rather than bending towards zeros. Live in
// Free, the window ends at the newest audio, which the copy's lined-up read passes 2 samples
// before; frozen and panned back to where the audio began, the copy (this track the late one, the
// other read 2 samples earlier) holds none before it. The samples inside the window the copy has
// not reached stay drawn as silence, not as missing.
TEST(AlignedEdges, DrawsALinedUpLaneStraightWhereItsAudioRunsOut)
{
    constexpr double kRate = 48000.0;
    for (const bool atStart : {false, true}) {
        SCOPED_TRACE(atStart ? "frozen, at the held audio's start" : "live, at the newest audio");
        const Effect effect{
            .delaySeconds = 2.0 / kRate, .gain = 0.5, .flipped = false, .fromCopy = atStart};
        Session session(kRate, 2);
        joinCopy(session, kRate, effect);
        showDifference(session, TimeBase::kFree);
        // Frozen, less than the 3 s a view pans back through is played, so the oldest audio
        // reached is where the session began; playing longer, the capture still holds the audio
        // before it, which the lined-up read rightly draws.
        session.play(atStart ? 2.0 : 4.0, 0.05);
        session.play(0.01);
        std::optional<DecodedFrame> frame = session.produce();
        ASSERT_TRUE(frame);
        session.analyzer().setFrozen(atStart);
        if (auto later = settled(session.analyzer())) {
            frame = std::move(later);
        }
        ASSERT_EQ(std::abs(matchOf(*frame, 1).delay), 2.0f) << "lined up";
        const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
        const double width = 4.0 * protocol::kMinimumViewSamples / spanSamples;
        if (atStart) {
            session.analyzer().setPan(10.0); // As far back as the hold reaches.
        }
        session.analyzer().setView(atStart ? 0.0 : 1.0 - width, atStart ? width : 1.0);
        frame = session.produce();
        ASSERT_TRUE(frame);
        ASSERT_EQ(frame->mode, FrameMode::kRaw);
        const std::size_t factor = frame->pointsPerSample;
        ASSERT_GT(factor, 1U);
        EXPECT_TRUE(noneBroken(*frame));
        EXPECT_EQ(frame->invalidLanes, 0U);
        EXPECT_EQ(frame->invalidSamples, 0U);
        for (const std::size_t index : {std::size_t{1}, std::size_t{2}}) {
            SCOPED_TRACE(index == 1 ? "the lined-up lane" : "the difference");
            expectStraightAtTheEnd(trace(*frame, index), factor, atStart);
        }
        if (atStart) {
            const std::span<const float> lane = trace(*frame, 1);
            EXPECT_EQ(lane[0], 0.0f) << "not yet reached: silence";
            EXPECT_EQ(lane[factor], 0.0f) << "not yet reached: silence";
        }
    }
}

// Frozen in Free and panned back to 16.5 samples after where the audio began, zoomed in on the
// window's start: this track (the copy, late by 2 samples) holds all the neighbours of its first
// gap before the window, while the other track, read 2 samples earlier through the alignment,
// misses one or two of them. The lined-up lane and the difference have no neighbour where the other
// holds none, and their first gap is drawn straight, though this track's own is the wave.
TEST(AlignedEdges, DrawsTheDifferenceStraightWhereOnlyTheLinedUpTrackRunsOut)
{
    constexpr double kRate = 48000.0;
    // Samples from the first held to the window's start: the first drawn sample is 16 or 17.
    constexpr double kInside = 16.5;
    const Effect effect{
        .delaySeconds = 2.0 / kRate, .gain = 0.5, .flipped = false, .fromCopy = true};
    Session session(kRate, 2);
    joinCopy(session, kRate, effect);
    showDifference(session, TimeBase::kFree);
    session.play(2.0, 0.05); // Less than the 3 s a frozen view pans back through.
    session.play(0.01);
    std::optional<DecodedFrame> frame = session.produce();
    ASSERT_TRUE(frame);
    session.analyzer().setFrozen(true);
    if (auto later = settled(session.analyzer())) {
        frame = std::move(later);
    }
    ASSERT_TRUE(frame);
    ASSERT_EQ(std::abs(matchOf(*frame, 1).delay), 2.0f) << "lined up";
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    session.analyzer().setPan(10.0); // As far back as the hold reaches, to read the reach.
    ASSERT_TRUE(session.produce());
    const double reach = session.field(FrameHeader::kPanReachOffset);
    ASSERT_GT(reach * kRate, kInside);
    session.analyzer().setPan(reach - (kInside / kRate));
    session.analyzer().setView(0.0, 4.0 * protocol::kMinimumViewSamples / spanSamples);
    frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    const std::size_t factor = frame->pointsPerSample;
    ASSERT_GT(factor, 1U);
    EXPECT_EQ(frame->invalidLanes, 0U);
    EXPECT_FALSE(drawnStraight(trace(*frame, 0), factor, 0)) << "this track's own: held";
    for (const std::size_t index : {std::size_t{1}, std::size_t{2}}) {
        SCOPED_TRACE(index == 1 ? "the lined-up lane" : "the difference");
        const std::span<const float> values = trace(*frame, index);
        EXPECT_TRUE(drawnStraight(values, factor, 0)) << "a neighbour the other does not hold";
        std::size_t curved = 0;
        constexpr std::size_t kFarFirst = 2;
        constexpr std::size_t kFarEnd = kFarFirst + 16;
        for (std::size_t gap = kFarFirst; gap < kFarEnd; ++gap) {
            curved += drawnStraight(values, factor, gap) ? 0 : 1;
        }
        EXPECT_GT(curved, (kFarEnd - kFarFirst) / 2) << "further in, the wave";
    }
}

// Where the copy's lined-up audio is held past the window's start (frozen in Trig, the copy late
// by 2 samples, read 2 samples later), its lane is the wave there, the same as this track's at
// half its level: neighbours read through the alignment, not drawn straight.
TEST(AlignedEdges, DrawsALinedUpLaneAsTheWaveWhereItsAudioIsHeld)
{
    constexpr double kRate = 48000.0;
    const Effect effect{.delaySeconds = 2.0 / kRate, .gain = 0.5, .flipped = false};
    Session session(kRate, 2);
    joinCopy(session, kRate, effect);
    showDifference(session, TimeBase::kTrigger);
    auto frame = playAndShow(session, true);
    ASSERT_TRUE(frame);
    ASSERT_EQ(std::abs(matchOf(*frame, 1).delay), 2.0f) << "lined up";
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    session.analyzer().setView(0.0, 4.0 * protocol::kMinimumViewSamples / spanSamples);
    frame = session.produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    const std::size_t factor = frame->pointsPerSample;
    const std::span<const float> own = trace(*frame, 0);
    const std::span<const float> lined = trace(*frame, 1);
    ASSERT_EQ(lined.size(), own.size());
    for (std::size_t k = 0; k < kReconstructionReach * factor; ++k) {
        EXPECT_NEAR(lined[k], 0.5f * own[k], 1e-4f) << "point " << k;
    }
    EXPECT_EQ(frame->invalidLanes, 0U);
}

struct PreviewedEdgeCase
{
    TimeBase timeBase = TimeBase::kTrigger;
    bool frozen = false;
};

class PreviewedSumEdges : public ::testing::TestWithParam<PreviewedEdgeCase>
{
};

// With the fix previewed, a copy 37 samples late is moved by its match and is silent where the
// move reads off the window: the sum has no neighbours past the window's ends, and is drawn
// straight within a reach of each, rather than bending towards the copy's audio as recorded.
TEST_P(PreviewedSumEdges, DrawsThePreviewedSumStraightAtTheWindowsEnds)
{
    const PreviewedEdgeCase c = GetParam();
    constexpr double kRate = 48000.0;
    const Effect effect{.delaySeconds = 37.0 / kRate, .flipped = false};
    Session session(kRate, 2);
    joinCopy(session, kRate, effect);
    configure(session.own(), Display::kCompare, c.timeBase);
    showLane(session.own(), protocol::CompareLane::kSum);
    setAlign(session.own(), false);
    setSpan(session.own(), c.timeBase, c.timeBase == TimeBase::kFree ? 2.0 : 0.25);
    session.analyzer().setShown(othersOf(session));
    auto frame = playAndShow(session, c.frozen);
    ASSERT_TRUE(frame);
    session.analyzer().setPreviewFix(true);
    if (auto previewed = settled(session.analyzer())) {
        frame = std::move(previewed);
    }
    const double spanSamples = frame->visibleSamples / (frame->viewEnd - frame->viewStart);
    const double width = 4.0 * protocol::kMinimumViewSamples / spanSamples;
    for (const bool atStart : {true, false}) {
        SCOPED_TRACE(atStart ? "the window's start" : "the window's end");
        session.analyzer().setView(atStart ? 0.0 : 1.0 - width, atStart ? width : 1.0);
        frame = session.produce();
        ASSERT_TRUE(frame);
        ASSERT_EQ(frame->mode, FrameMode::kRaw);
        EXPECT_EQ(frame->invalidLanes, 0U);
        expectStraightAtTheEnd(trace(*frame, 2), frame->pointsPerSample, atStart);
    }
}

INSTANTIATE_TEST_SUITE_P(TimeBases, PreviewedSumEdges,
                         ::testing::Values(PreviewedEdgeCase{TimeBase::kTrigger, false},
                                           PreviewedEdgeCase{TimeBase::kTrigger, true},
                                           PreviewedEdgeCase{TimeBase::kFree, false},
                                           PreviewedEdgeCase{TimeBase::kFree, true}),
                         [](const ::testing::TestParamInfo<PreviewedEdgeCase>& info) {
                             return timeBaseName(info.param.timeBase) +
                                    (info.param.frozen ? "_Frozen" : "_Live");
                         });

// A steady routing is searched again every half second, and each search finds the same
// alignment. It is not another one: the lanes and the difference keep their columns, and only
// the new audio is drawn, as with Align off.
TEST(AlignedWork, DrawsOnlyTheNewAudioWhileTheAlignmentHolds)
{
    constexpr double kRate = 48000.0;
    constexpr double kSeconds = 4.0;
    for (const TimeBase timeBase : {TimeBase::kBeats, TimeBase::kFree}) {
        SCOPED_TRACE(timeBaseName(timeBase));
        const auto reducedWhilePlaying = [&](bool align) {
            Session session(kRate, 2);
            // Viewed from the late track, whose lanes wait for no audio.
            joinCopy(session, kRate, {.fromCopy = true});
            showDifference(session, timeBase);
            setAlign(session.own(), align);
            session.play(6.0, 0.1);
            const AnalysisWork before = session.analyzer().work();
            session.play(kSeconds, 0.1);
            return workSince(before, session.analyzer().work());
        };
        const AnalysisWork aligned = reducedWhilePlaying(true);
        const AnalysisWork recorded = reducedWhilePlaying(false);
        EXPECT_GE(aligned.matchSearches, 4U) << "no search to find the same alignment";
        // A lane drawn again at each search would add the whole span each time: several times
        // what the new audio takes. Lined up, a change reaches as far as the shift, no further.
        EXPECT_LT(aligned.samplesReduced, 2 * recorded.samplesReduced)
            << "the lanes were drawn again though the alignment held";
    }
}

} // namespace
} // namespace orizon::engine
