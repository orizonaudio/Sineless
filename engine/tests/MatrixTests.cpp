// The matrix: the engine's rules run at every sample rate a host may use, with spans on both sides
// of each limit the engine branches on, in every display mode and time base, mono and stereo, live
// and frozen. Spans, modes, beat spans and limits come from the protocol, so a new one is covered
// as it is added. Past bugs sat on exactly these edges: a frozen hold sized to the longest span
// left lanes blank at the pan reach, a view at the zoom limit was rejected after rounding, and a
// quiet level read as a number where there was none.
//
// Signals ride on a DC offset, so a trace value near zero is a hole in the data, never a zero
// crossing.

#include "FrameDecoding.h"
#include "engine/Analyzer.h"
#include "engine/CaptureRing.h"
#include "engine/HoldBuffer.h"
#include "engine/InstanceRegistry.h"
#include "engine/Sweep.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::BeatSpan;
using protocol::DisplayMode;
using protocol::FrameFlag;
using protocol::FrameHeader;
using protocol::FrameMode;
using protocol::SettingId;
using protocol::TimeBase;

// The sample rates hosts run at, from CD audio to the highest any of them offers. The protocol
// does not list them: nothing the engine and the UI share depends on one.
constexpr std::array kSampleRates{44100.0,  48000.0,  88200.0,  96000.0,
                                  176400.0, 192000.0, 352800.0, 384000.0};
static_assert(kSampleRates.back() == CaptureRing::kHighestSampleRate,
              "the capture is sized for the highest rate tested");

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

std::string modeName(DisplayMode mode)
{
    return capitalised(protocol::kDisplayModeNames.at(static_cast<std::size_t>(mode)));
}

std::string timeBaseName(TimeBase timeBase)
{
    return capitalised(protocol::kTimeBaseNames.at(static_cast<std::size_t>(timeBase)));
}

const char* channelsName(int channels)
{
    return channels == 1 ? "Mono" : "Stereo";
}

std::vector<DisplayMode> everyMode()
{
    std::vector<DisplayMode> modes;
    modes.reserve(protocol::kDisplayModeCount);
    for (std::size_t mode = 0; mode < protocol::kDisplayModeCount; ++mode) {
        modes.push_back(static_cast<DisplayMode>(mode));
    }
    return modes;
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

// Whether the capture keeps what a freeze in Seconds mode holds at `rate`, for readers while the
// audio thread writes on (the most the engine reads at once, as frozenLength in Analyzer.cpp).
bool captureHoldsTheFreeze(double rate)
{
    return protocol::kFrozenHoldSeconds * rate <=
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
    // Every `brokenEvery` samples (none if 0), one that is not finite, as a broken plugin upstream
    // might send.
    std::int64_t brokenEvery = 0;
    float broken = std::numeric_limits<float>::quiet_NaN();
    std::int64_t brokenUntil = std::numeric_limits<std::int64_t>::max(); // Then whole again.
};

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
        return engine;
    }

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
            for (Track& track : tracks) {
                fill(track.signal, position);
                std::array<const float*, 2> channels{left.data(), right.data()};
                track.engine->captureRing().write(channels.data(), channelCount, kBlock);
                track.engine->timeline().advance(
                    {.playing = true,
                     .positionValid = true,
                     .quarterNotes = static_cast<double>(position) * track.tempo / (60.0 * rate),
                     .tempoValid = true,
                     .tempo = track.tempo},
                    kBlock, rate);
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

    // Writes `samples` more to one track's capture only, as if its audio arrived early.
    void lead(std::size_t track, int samples)
    {
        std::vector<float> values(static_cast<std::size_t>(samples), 0.5f);
        std::array<const float*, 2> channels{values.data(), values.data()};
        tracks.at(track).engine->captureRing().write(channels.data(), channelCount, samples);
    }

    // The next frame, if one comes within a few tries.
    std::optional<DecodedFrame> produce()
    {
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (analyzer().produce(bytes)) {
                return decode(bytes);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] float field(std::size_t offset) const { return get<float>(bytes, offset); }

    double rate;
    InstanceRegistry registry;

private:
    struct Track
    {
        std::shared_ptr<Engine> engine;
        Signal signal;
        double tempo = 120.0;
    };

    void fill(const Signal& signal, std::uint64_t from)
    {
        const double step = 2.0 * std::numbers::pi * signal.frequency / rate;
        for (int i = 0; i < kBlock; ++i) {
            const auto index = static_cast<std::int64_t>(from) + i;
            double value = 0.0;
            switch (signal.kind) {
            case Signal::Kind::kTone:
                value =
                    signal.offset + signal.amplitude * std::sin(step * static_cast<double>(index));
                break;
            case Signal::Kind::kNoise:
                value = signal.amplitude * 2.0 * noiseAt(index - signal.delay, signal.seed);
                break;
            case Signal::Kind::kSilence:
                break;
            }
            left.at(static_cast<std::size_t>(i)) = static_cast<float>(value);
            right.at(static_cast<std::size_t>(i)) = static_cast<float>(0.5 * value);
            if (signal.brokenEvery > 0 && index % signal.brokenEvery == 0 &&
                index < signal.brokenUntil) {
                left.at(static_cast<std::size_t>(i)) = signal.broken;
                right.at(static_cast<std::size_t>(i)) = signal.broken;
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

void configure(Engine& engine, DisplayMode mode, TimeBase timeBase)
{
    engine.settings().set(SettingId::kDisplayMode, static_cast<double>(mode));
    engine.settings().set(SettingId::kTimeBase, static_cast<double>(timeBase));
    engine.settings().set(SettingId::kTriggerMode,
                          static_cast<double>(protocol::TriggerMode::kFree));
    engine.settings().set(SettingId::kChannelViews, 0b0011); // L and R asked for.
}

// The traces a frame should hold: views in Single (one for a mono track), else one lane per
// track and, in Compare, the sum.
std::size_t expectedTraces(DisplayMode mode, int channels, std::size_t tracks)
{
    switch (mode) {
    case DisplayMode::kSingle:
        return channels == 1 ? 1 : 2;
    case DisplayMode::kMulti:
        return tracks;
    case DisplayMode::kCompare:
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

// Whether two frames' data are the same value for value, a missing value (NaN) matching another.
bool sameValues(const std::vector<float>& a, const std::vector<float>& b)
{
    return std::ranges::equal(
        a, b, [](float x, float y) { return x == y || (std::isnan(x) && std::isnan(y)); });
}

// Checks that the frame's data holds every trace, and that no value of trace `index` is a hole
// (or not a number); a trace in `silent` must be all zeros instead.
void expectTraces(const DecodedFrame& frame, std::size_t expected,
                  std::optional<std::size_t> silent = std::nullopt)
{
    ASSERT_EQ(tracesIn(frame), expected);
    ASSERT_GE(frame.data.size(), expected * traceLength(frame)) << "data shorter than its traces";
    ASSERT_GT(frame.pointCount, 0U);
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
// holds, and the freeze holds the longest span, so a frame of the longest span can be read live
// and frozen.
TEST_P(CaptureCapacity, HoldsAFreeze)
{
    const double rate = GetParam();
    Engine engine;
    engine.prepare(rate);
    const std::size_t capacity = engine.captureRing().capacity();
    EXPECT_EQ(capacity, CaptureRing::capacityFor(rate));
    EXPECT_TRUE(captureHoldsTheFreeze(rate))
        << protocol::kFrozenHoldSeconds * rate << " samples, " << capacity << " held";
    EXPECT_TRUE(capacity == CaptureRing::kMinimumCapacity ||
                static_cast<double>(CaptureRing::readableLength(capacity / 2)) <
                    protocol::kFrozenHoldSeconds * rate)
        << "no larger than it needs";
    const HoldBuffer hold(capacity);
    EXPECT_EQ(hold.holdLength(rate),
              static_cast<std::size_t>(std::llround(protocol::kFrozenHoldSeconds * rate)));
    EXPECT_EQ(protocol::kFrozenHoldSeconds, setting(SettingId::kTimeSpan).maximum)
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
                return length.inBars ? length.amount * barQuarterNotes : length.amount;
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

// --- Seconds spans --------------------------------------------------------------------------

// A span of the seconds time base, from the protocol: the shortest, the roll threshold and the
// next span above it, and the longest.
struct SecondsSpan
{
    std::string name;
    double seconds = 0.0;
};

std::vector<SecondsSpan> secondsSpans()
{
    return {{.name = "Shortest", .seconds = setting(SettingId::kTimeSpan).minimum},
            {.name = "AtRollThreshold", .seconds = protocol::kRollThresholdSeconds},
            {.name = "AboveRollThreshold",
             .seconds = std::nextafter(protocol::kRollThresholdSeconds,
                                       std::numeric_limits<double>::infinity())},
            {.name = "Longest", .seconds = setting(SettingId::kTimeSpan).maximum}};
}

struct SpanCase
{
    double rate = 48000.0;
    SecondsSpan span;
    DisplayMode mode = DisplayMode::kSingle;
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
            for (const DisplayMode mode : everyMode()) {
                cases.push_back({.rate = kSampleRates.at(rate),
                                 .span = spans[span],
                                 .mode = mode,
                                 .channels = (rate + span) % 2 == 0 ? 2 : 1});
            }
        }
    }
    return cases;
}

// Two tracks of a case in Seconds mode, played past a whole capture, so that it has wrapped as in
// a host that has played for a while, and `extraSeconds` more.
struct SecondsSession
{
    SecondsSession(const SpanCase& c, double extraSeconds) : session(c.rate, c.channels)
    {
        session.join(own);
        session.join({.frequency = 1500.0});
        configure(session.own(), c.mode, TimeBase::kSeconds);
        EXPECT_TRUE(session.own().settings().set(SettingId::kTimeSpan, c.span.seconds));
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

// Live, the frame holds the whole span, rolls exactly above the threshold, and measures it.
// Frozen, it holds the same.
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
    EXPECT_EQ(frame->has(FrameFlag::kRoll), c.span.seconds > protocol::kRollThresholdSeconds);
    EXPECT_EQ(frame->has(FrameFlag::kCompared), c.mode == DisplayMode::kCompare);
    EXPECT_NEAR(frame->visibleSamples, spanSamples, kColumns / 2.0 + 1.0);
    expectTraces(*frame, traces);
    EXPECT_TRUE(std::isfinite(frame->peak));
    EXPECT_GT(frame->peak, 0.1f) << "readouts measured nothing";
    EXPECT_GT(frame->rms, 0.1f);
    if (c.span.seconds * seconds.own.frequency >= 4.0) {
        EXPECT_NEAR(frame->frequency, seconds.own.frequency, seconds.own.frequency * 0.01);
    }

    frame = seconds.freeze();
    ASSERT_TRUE(frame) << "no frozen frame";
    ASSERT_TRUE(frame->has(FrameFlag::kFrozen));
    EXPECT_NEAR(frame->visibleSamples, spanSamples, kColumns / 2.0 + 1.0);
    expectTraces(*frame, traces);
}

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
    const double width = protocol::kMinimumViewWidth * (1.0 - 1e-12);
    session.analyzer().setView(0.5, 0.5 + width);
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frame at the zoom limit";
    EXPECT_NEAR(frame->viewEnd - frame->viewStart, protocol::kMinimumViewWidth, 1e-6);
    EXPECT_NEAR(frame->visibleSamples, width * spanSamples, 1e-3 * spanSamples + 1e-3);
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

// --- A change of sample rate ----------------------------------------------------------------

struct RateChange
{
    double from = 48000.0;
    double to = 384000.0;
    DisplayMode mode = DisplayMode::kSingle;
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
        for (const DisplayMode mode : everyMode()) {
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
    configure(session.own(), c.mode, TimeBase::kSeconds);
    const double span = setting(SettingId::kTimeSpan).minimum;
    EXPECT_TRUE(session.own().settings().set(SettingId::kTimeSpan, span));
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
    DisplayMode mode = DisplayMode::kSingle;
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
        for (const DisplayMode mode : everyMode()) {
            cases.push_back({.rate = kSampleRates.at(rate),
                             .mode = mode,
                             .channels = rate % 2 == 0 ? 2 : 1,
                             .timeBase = TimeBase::kBeats});
        }
    }
    return cases;
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
// span, rebuilt for a longer one from the held audio, and at the zoom limit.
TEST_P(BeatSpans, DrawsTheWholeSweepLiveAndFrozen)
{
    const ModeCase c = GetParam();
    const double rate = c.rate;
    Session session(rate, c.channels);
    session.join({.frequency = 1000.0});
    session.join({.frequency = 1500.0});
    configure(session.own(), c.mode, TimeBase::kBeats);
    const auto span = static_cast<std::size_t>(BeatSpan::kOneBeat);
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

    // Shorter and longer spans chosen while frozen, then back.
    for (const std::size_t other : {span - 1, span + 1, span}) {
        SCOPED_TRACE(protocol::kBeatSpanNames.at(other));
        session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(other));
        frame = session.produce();
        ASSERT_TRUE(frame);
        EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
        EXPECT_NEAR(frame->visibleSamples, static_cast<float>(lengthOf(other)), 1.0f);
        expectTraces(*frame, traces);
    }

    const double width = protocol::kMinimumViewWidth * (1.0 - 1e-12);
    session.analyzer().setView(1.0 - width, 1.0);
    frame = session.produce();
    ASSERT_TRUE(frame) << "no frame at the zoom limit";
    EXPECT_NEAR(frame->viewEnd - frame->viewStart, protocol::kMinimumViewWidth, 1e-6);
    SCOPED_TRACE("zoomed to the limit");
    expectTraces(*frame, traces);
}

INSTANTIATE_TEST_SUITE_P(EveryRate, BeatSpans, ::testing::ValuesIn(beatCases()), modeTestName);

// --- Beat spans at the freeze hold ----------------------------------------------------------

// The longest beat span whose sweep at `tempo` fits in the capture, which bounds what a freeze in
// Beats mode holds. The freeze keeps a little less, leaving the audio thread a margin to write on
// (frozenLength in Analyzer.cpp); spans double, so at these rates none falls in that margin, and
// the test below would find one that did, its frozen frame not whole.
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
        for (const DisplayMode mode : everyMode()) {
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
// the frame is whole: as it is, cropped to the held span, and back.
TEST_P(HeldBeatSpans, StaysWholeFrozenFromALongerSpanAndBack)
{
    const ModeCase c = GetParam();
    const std::size_t fits = longestBeatSpanHeld(c.rate, kHeldTempo);
    const std::size_t longer = fits + 1;
    ASSERT_LT(longer, protocol::kBeatSpanCount) << "every beat span fits the capture";
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
    for (const std::size_t span : {fits, longer}) {
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
class FrozenSweepFillingIn : public ::testing::TestWithParam<DisplayMode>
{
};

TEST_P(FrozenSweepFillingIn, IsAsItWasBackFromAShorterSpan)
{
    constexpr double kRate = 48000.0;
    const DisplayMode mode = GetParam();
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

INSTANTIATE_TEST_SUITE_P(EveryMode, FrozenSweepFillingIn,
                         ::testing::Values(DisplayMode::kSingle, DisplayMode::kMulti,
                                           DisplayMode::kCompare),
                         [](const ::testing::TestParamInfo<DisplayMode>& info) {
                             return modeName(info.param);
                         });

// Frozen with a lane still filling in, beside whole ones: a shorter span longer than the hold is
// cropped from the whole lanes, which keep all they drew, and rebuilt from the held audio only for
// the lane that is filling in. At 90 BPM, 16 bars is 42.7 s and 8 bars 21.3 s, longer than the
// 19.1 s held at 48 kHz.
class MixedFrozenLanes : public ::testing::TestWithParam<DisplayMode>
{
};

TEST_P(MixedFrozenLanes, KeepsWholeLanesWholeBesideALaneFillingIn)
{
    constexpr double kRate = 48000.0;
    constexpr double kTempo = 90.0;
    const DisplayMode mode = GetParam();
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
    if (mode == DisplayMode::kCompare) {
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
                         ::testing::Values(DisplayMode::kMulti, DisplayMode::kCompare),
                         [](const ::testing::TestParamInfo<DisplayMode>& info) {
                             return info.param == DisplayMode::kMulti ? std::string("Multi")
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
    std::uint64_t length = 0; // The own track's sweep, in samples; 0 for a sixteenth at 120 BPM.
    bool within = true;       // The other track's sweep: at the tolerance, or one sample beyond.
    DisplayMode mode = DisplayMode::kMulti;
};

std::uint64_t ownLength(const LengthCase& c)
{
    const double sixteenth = protocol::kBeatSpanLengths.at(0).amount;
    return c.length > 0 ? c.length : beatSamples(c.rate, 120.0, sixteenth);
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

// Every rate at a sixteenth beat, the lanes' modes; and at 48 kHz, lengths on both sides of each
// edge sweepLengthsSimilar has, each within and beyond the tolerance. Below the first (1023
// samples) it allows nothing and only the one sample lets a lane through; from it, it allows that
// sample too; from the next ones (2046, 3069) it allows two and three samples.
std::vector<LengthCase> lengthCases()
{
    std::vector<LengthCase> cases;
    for (const double rate : kSampleRates) {
        for (const bool within : {true, false}) {
            for (const DisplayMode mode : {DisplayMode::kMulti, DisplayMode::kCompare}) {
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
    const double sixteenth = protocol::kBeatSpanLengths.at(0).amount;
    const auto tempoFor = [&](std::uint64_t samples) {
        return sixteenth * 60.0 * rate / static_cast<double>(samples);
    };
    ASSERT_EQ(beatSamples(rate, tempoFor(length), sixteenth), length);
    ASSERT_EQ(beatSamples(rate, tempoFor(length + apart), sixteenth), length + apart);

    Session session(rate, 2);
    session.join({.frequency = 1000.0}, tempoFor(length));
    session.join({.frequency = 1500.0}, tempoFor(length + apart));
    configure(session.own(), c.mode, TimeBase::kBeats);
    session.own().settings().set(SettingId::kBeatSpan, 0.0);
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
    configure(session.own(), DisplayMode::kCompare, TimeBase::kSeconds);
    // A span of ten times the reach: the match is searched over the visible span, so even a track
    // offset by the whole reach overlaps nine tenths of it.
    session.own().settings().set(SettingId::kTimeSpan, 10.0 * protocol::kMatchReachSeconds);
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

std::vector<ModeCase> timeBaseCases(DisplayMode mode)
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

// Plays enough for the readouts of `timeBase` (a sweep of one beat, or a 50 ms window), producing
// frames as a page does, and returns the last frame.
std::optional<DecodedFrame> playForReadouts(Session& session, TimeBase timeBase)
{
    configure(session.own(),
              session.own().settings().get().enumValue<DisplayMode>(SettingId::kDisplayMode),
              timeBase);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBeat));
    session.own().settings().set(SettingId::kTimeSpan, 0.05);
    session.play(protocol::kFrequencyWindowSeconds + 0.1, protocol::kReadoutIntervalSeconds);
    session.play(protocol::kReadoutIntervalSeconds);
    return session.produce();
}

class AnalysisGate : public ::testing::TestWithParam<ModeCase>
{
};

// A tone just above the analysis gate has its frequency measured, one just below has none
// (frequency 0, as the frame defines it); either way the level travels as measured, not floored.
TEST_P(AnalysisGate, MeasuresTheFrequencyAboveTheGateOnly)
{
    const ModeCase c = GetParam();
    for (const double aboveGate : {1.0, -1.0}) {
        SCOPED_TRACE(aboveGate > 0.0 ? "above the gate" : "below the gate");
        Session session(c.rate, c.channels);
        // An RMS level aboveGate dB from the gate, as the readout view (the Mid of L and half of
        // L, or the one channel of a mono track) measures it.
        const double viewGain = c.channels == 1 ? 1.0 : 0.75;
        const double rms = std::pow(10.0, (protocol::kAnalysisGateDbfs + aboveGate) / 20.0);
        const double amplitude = rms * std::numbers::sqrt2 / viewGain;
        session.join({.frequency = 1000.0, .amplitude = amplitude, .offset = 0.0});
        session.own().settings().set(SettingId::kDisplayMode, static_cast<double>(c.mode));
        session.own().settings().set(SettingId::kTriggerSource,
                                     static_cast<double>(protocol::ChannelView::kMid));
        const auto frame = playForReadouts(session, c.timeBase);
        ASSERT_TRUE(frame);
        EXPECT_NEAR(frame->peak, amplitude * viewGain, amplitude * viewGain * 1e-3)
            << "the peak as measured";
        EXPECT_NEAR(frame->rms, rms, rms * 0.02);
        if (aboveGate > 0.0) {
            EXPECT_NEAR(frame->frequency, 1000.0f, 10.0f);
        } else {
            EXPECT_TRUE(std::isnan(frame->frequency)) << "no frequency: no value";
        }
    }
}

INSTANTIATE_TEST_SUITE_P(EveryRate, AnalysisGate,
                         ::testing::ValuesIn(timeBaseCases(DisplayMode::kSingle)), modeTestName);

// Samples that are not finite, one every 100 ms on this track (NaN or infinite): measured around,
// so the readouts are those of the tone, and counted. The frame says how many the readouts measured
// around, and which lanes hold any: this track's and, in Compare, the sum's, never the other
// track's; none once they are past. In every mode and time base, live and in Seconds mode
// rolling, at the lowest and highest rates.
struct BrokenCase
{
    ModeCase mode;
    bool rolling = false;
    bool infinite = false;
};

std::string nameOf(const BrokenCase& c)
{
    return nameOf(c.mode) + (c.rolling ? "_Rolling" : "") + (c.infinite ? "_Infinity" : "_NaN");
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
        for (const DisplayMode mode : everyMode()) {
            for (const bool infinite : {false, true}) {
                for (const auto [timeBase, rolling] :
                     {std::pair{TimeBase::kBeats, false}, std::pair{TimeBase::kSeconds, false},
                      std::pair{TimeBase::kSeconds, true}}) {
                    cases.push_back({.mode = {.rate = rate,
                                              .mode = mode,
                                              .channels = index++ % 2 == 0 ? 2 : 1,
                                              .timeBase = timeBase},
                                     .rolling = rolling,
                                     .infinite = infinite});
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
    // What the readouts measure: a beat at 120 BPM, or the span.
    const double span = c.mode.timeBase == TimeBase::kBeats ? 0.5 : c.rolling ? 2.0 : 0.2;
    const double played = c.rolling ? 3.0 : 1.5;
    Session session(rate, c.mode.channels);
    session.join({.frequency = 1000.0,
                  .brokenEvery = std::llround(rate / 10.0),
                  .broken = c.infinite ? std::numeric_limits<float>::infinity()
                                       : std::numeric_limits<float>::quiet_NaN(),
                  .brokenUntil = std::llround(rate * (played + 0.1))});
    session.join({.frequency = 1500.0});
    configure(session.own(), c.mode.mode, c.mode.timeBase);
    session.own().settings().set(SettingId::kTimeSpan, span);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBeat));
    session.play(played);
    auto frame = session.produce();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(std::isfinite(frame->peak)) << "measured around";
    EXPECT_TRUE(std::isfinite(frame->rms)) << "measured around";
    EXPECT_EQ(frame->invalidSamples, static_cast<std::uint32_t>(std::lround(span * 10.0)))
        << "one every 100 ms";
    switch (c.mode.mode) {
    case DisplayMode::kSingle:
        EXPECT_EQ(frame->invalidLanes, 0U) << "no lanes";
        break;
    case DisplayMode::kMulti:
        EXPECT_EQ(frame->invalidLanes, 0b01U) << "this track's lane only";
        break;
    case DisplayMode::kCompare:
        EXPECT_EQ(frame->invalidLanes, 0b101U) << "this track's lane and the sum's";
        break;
    }

    // Once they are past what the frame shows, nothing is flagged any more: with frames along the
    // way, so a rolling lane keeps its columns and moves their flags with them.
    session.play(span * 2.0 + 0.5, 0.02);
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

// Every sample of this track not finite: nothing to measure, so its readouts have no value (NaN),
// never the level of silence, and every sample measured is counted. In every mode and time base,
// live and rolling, at the lowest and highest rates.
class AllBroken : public ::testing::TestWithParam<BrokenCase>
{
};

TEST_P(AllBroken, HasNoReadouts)
{
    const BrokenCase c = GetParam();
    const double rate = c.mode.rate;
    const double span = c.mode.timeBase == TimeBase::kBeats ? 0.5 : c.rolling ? 2.0 : 0.2;
    Session session(rate, c.mode.channels);
    session.join({.frequency = 1000.0,
                  .brokenEvery = 1,
                  .broken = c.infinite ? std::numeric_limits<float>::infinity()
                                       : std::numeric_limits<float>::quiet_NaN()});
    session.join({.frequency = 1500.0});
    configure(session.own(), c.mode.mode, c.mode.timeBase);
    session.own().settings().set(SettingId::kTimeSpan, span);
    session.own().settings().set(SettingId::kBeatSpan, static_cast<double>(BeatSpan::kOneBeat));
    session.play(c.rolling ? 3.0 : 1.5);
    const auto frame = session.produce();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(std::isnan(frame->peak)) << "no value, not silence";
    EXPECT_TRUE(std::isnan(frame->rms)) << "no value, not silence";
    EXPECT_TRUE(std::isnan(frame->frequency)) << "no value";
    // Every sample the readouts measure: the span, rounded out to whole blocks of levels.
    const auto spanSamples = static_cast<std::uint32_t>(std::lround(span * rate));
    const auto block = static_cast<std::uint32_t>(std::lround(kStatsBlockSeconds * rate));
    EXPECT_GE(frame->invalidSamples, spanSamples);
    EXPECT_LE(frame->invalidSamples, spanSamples + 2 * block);
    if (c.mode.mode == DisplayMode::kCompare) {
        // The sum is never finite, so it is not measured, and neither is the other track there.
        EXPECT_TRUE(std::isnan(session.field(FrameHeader::kSumGainOffset)))
            << "sum gain " << session.field(FrameHeader::kSumGainOffset);
        EXPECT_TRUE(std::isnan(session.field(FrameHeader::kFixedGainOffset)))
            << "fixed gain " << session.field(FrameHeader::kFixedGainOffset);
    }
}

INSTANTIATE_TEST_SUITE_P(EdgeRates, AllBroken, ::testing::ValuesIn(brokenCases()),
                         [](const ::testing::TestParamInfo<BrokenCase>& info) {
                             return nameOf(info.param);
                         });

// A copy with every other sample of this track not finite still sums as a copy, 10 log10(2) =
// +3.0103 dB, in Beats as in Seconds, live and frozen: where the sum is not finite, neither track
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
    session.own().settings().set(SettingId::kDisplayMode,
                                 static_cast<double>(DisplayMode::kCompare));
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
                         ::testing::ValuesIn(timeBaseCases(DisplayMode::kCompare)), modeTestName);

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
    session.own().settings().set(SettingId::kDisplayMode,
                                 static_cast<double>(DisplayMode::kCompare));
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
                         ::testing::ValuesIn(timeBaseCases(DisplayMode::kCompare)), modeTestName);

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
    for (const ModeCase& mode : timeBaseCases(DisplayMode::kCompare)) {
        const std::int64_t reach = std::lround(protocol::kMatchReachSeconds * mode.rate);
        cases.push_back({.mode = mode, .name = "OneSample", .delay = 1});
        cases.push_back({.mode = mode, .name = "UnderTheReach", .delay = reach - 1});
        cases.push_back({.mode = mode, .name = "AtTheReach", .delay = reach});
        cases.push_back({.mode = mode, .name = "LeadingAtTheReach", .delay = -reach});
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

class DelayedCopy : public ::testing::TestWithParam<DelayCase>
{
};

// Lined up again, a delayed copy sums as a copy does, 10 log10(2) = +3.0103 dB louder than two
// unrelated tracks, in Beats as in Seconds, live and frozen: the fix is measured only where the
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
    session.own().settings().set(SettingId::kDisplayMode,
                                 static_cast<double>(DisplayMode::kCompare));
    auto frame = playForReadouts(session, c.mode.timeBase);
    // A block at a time, until the playhead is within the delay of the sweep's edge.
    const auto distance = static_cast<float>(std::abs(c.delay));
    for (int block = 0; c.stop != Stop::kAnywhere && block < 4000; ++block) {
        ASSERT_TRUE(frame);
        const float playhead = frame->cursor * frame->visibleSamples;
        if (c.stop == Stop::kAfterAWrap
                ? playhead > 0.0f && playhead < distance
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
}

INSTANTIATE_TEST_SUITE_P(EveryRate, DelayedCopy, ::testing::ValuesIn(delayCases()), delayTestName);

} // namespace
} // namespace orizon::engine
