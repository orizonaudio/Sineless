// Rules every view must keep, run across every display mode and time base (from the protocol, so
// a new one is tested as it is added), live and frozen, on mono and stereo tracks: a frame comes,
// it says whether it is frozen, it holds the views or lanes it should, none of them is silent
// while its track plays, its readouts measure something, and frames keep coming while live. A fix
// that reaches only one of these cases fails the others.

#include "FrameDecoding.h"
#include "engine/Analyzer.h"
#include "engine/InstanceRegistry.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <gtest/gtest.h>
#include <memory>
#include <numbers>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::DisplayMode;
using protocol::FrameMode;
using protocol::SettingId;
using protocol::TimeBase;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 480;

struct Case
{
    DisplayMode mode = DisplayMode::kSingle;
    TimeBase timeBase = TimeBase::kBeats;
    bool frozen = false;
    int channels = 2;
};

std::vector<Case> everyCase()
{
    std::vector<Case> cases;
    for (std::size_t mode = 0; mode < protocol::kDisplayModeCount; ++mode) {
        for (std::size_t timeBase = 0; timeBase < protocol::kTimeBaseCount; ++timeBase) {
            for (const bool frozen : {false, true}) {
                for (const int channels : {1, 2}) {
                    cases.push_back({.mode = static_cast<DisplayMode>(mode),
                                     .timeBase = static_cast<TimeBase>(timeBase),
                                     .frozen = frozen,
                                     .channels = channels});
                }
            }
        }
    }
    return cases;
}

std::string capitalised(std::string_view name)
{
    std::string text(name);
    if (!text.empty()) {
        text.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
    }
    return text;
}

// "Compare_Seconds_Frozen_Mono".
std::string nameOf(const Case& c)
{
    return capitalised(protocol::kDisplayModeNames.at(static_cast<std::size_t>(c.mode))) + "_" +
           capitalised(protocol::kTimeBaseNames.at(static_cast<std::size_t>(c.timeBase))) + "_" +
           (c.frozen ? "Frozen" : "Live") + "_" + (c.channels == 1 ? "Mono" : "Stereo");
}

std::string testName(const ::testing::TestParamInfo<Case>& info)
{
    return nameOf(info.param);
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const Case& c, std::ostream* out)
{
    *out << nameOf(c);
}

// Two tracks in group A playing tones on one host clock, viewed from the first.
class Tracks
{
public:
    explicit Tracks(int channels) : channelCount(channels)
    {
        join(220.0);
        join(330.0);
    }

    // A track that starts playing a tone now.
    void join(double frequency)
    {
        auto engine = std::make_shared<Engine>();
        engine->setSampleRate(kSampleRate);
        engine->setInputChannels(channelCount);
        registry.add(engine);
        engines.push_back(engine);
        frequencies.push_back(frequency);
    }

    Engine& own() { return *engines.front(); }

    // Plays `samples`, the tempo moving by `ramp` BPM per block.
    void play(std::uint64_t samples, double ramp = 0.0)
    {
        std::array<float, kBlock> left{};
        std::array<float, kBlock> right{};
        for (std::uint64_t done = 0; done < samples; done += kBlock) {
            for (std::size_t track = 0; track < engines.size(); ++track) {
                for (int i = 0; i < kBlock; ++i) {
                    const double t =
                        static_cast<double>(position + static_cast<std::uint64_t>(i)) / kSampleRate;
                    left[static_cast<std::size_t>(i)] = static_cast<float>(
                        0.5 * std::sin(2.0 * std::numbers::pi * frequencies[track] * t));
                    right[static_cast<std::size_t>(i)] = 0.5f * left[static_cast<std::size_t>(i)];
                }
                std::array<const float*, 2> channels{left.data(), right.data()};
                engines[track]->captureRing().write(channels.data(), 2, kBlock);
                engines[track]->timeline().advance({.playing = true,
                                                    .positionValid = true,
                                                    .quarterNotes = quarterNotes,
                                                    .tempoValid = true,
                                                    .tempo = tempo},
                                                   kBlock, kSampleRate);
            }
            position += kBlock;
            quarterNotes += kBlock * tempo / (60.0 * kSampleRate);
            tempo += ramp;
        }
    }

    int channelCount;
    // Writes `samples` more to one track's capture only, as if its audio arrived early.
    void lead(std::size_t track, int samples)
    {
        std::vector<float> silence(static_cast<std::size_t>(samples), 0.1f);
        std::array<const float*, 2> channels{silence.data(), silence.data()};
        engines.at(track)->captureRing().write(channels.data(), 2, samples);
    }

    InstanceRegistry registry;
    std::vector<std::shared_ptr<Engine>> engines;
    std::vector<double> frequencies;
    std::uint64_t position = 0;
    double quarterNotes = 0.0;
    double tempo = 120.0;
};

void configure(Engine& engine, DisplayMode mode, TimeBase timeBase)
{
    engine.settings().set(SettingId::kDisplayMode, static_cast<double>(mode));
    engine.settings().set(SettingId::kTimeBase, static_cast<double>(timeBase));
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBeat));
    engine.settings().set(SettingId::kTimeSpan, 0.05);
    engine.settings().set(SettingId::kTriggerMode,
                          static_cast<double>(protocol::TriggerMode::kFree));
    engine.settings().set(SettingId::kChannelViews, 0b0011); // L and R asked for.
}

// The traces the frame should hold: views in Single (one for a mono track), else one lane per
// track and, in Compare, the sum.
std::size_t expectedTraces(DisplayMode mode, int channels, std::size_t tracks = 2)
{
    switch (mode) {
    case DisplayMode::kSingle:
        return channels == 1 ? 1 : 2;
    case DisplayMode::kMulti:
        return tracks;
    default:
        return tracks + 1;
    }
}

std::size_t tracesIn(const DecodedFrame& frame)
{
    return frame.laneCount > 0 ? frame.laneCount
                               : static_cast<std::size_t>(std::popcount(frame.views));
}

// Each trace's loudest value.
std::vector<float> loudestPerTrace(const DecodedFrame& frame)
{
    const std::size_t traces = tracesIn(frame);
    const std::size_t perPoint = frame.mode == FrameMode::kMinMax ? 2 : 1;
    const std::size_t length = static_cast<std::size_t>(frame.pointCount) * perPoint;
    std::vector<float> loudest(traces, 0.0f);
    for (std::size_t trace = 0; trace < traces && (trace + 1) * length <= frame.data.size();
         ++trace) {
        for (std::size_t i = 0; i < length; ++i) {
            loudest[trace] = std::max(loudest[trace], std::abs(frame.data[trace * length + i]));
        }
    }
    return loudest;
}

std::vector<std::byte> lastProduced;

std::optional<DecodedFrame> produceSome(Analyzer& analyzer, int attempts = 4)
{
    std::vector<std::byte> bytes;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (analyzer.produce(bytes)) {
            lastProduced = bytes;
            return decode(bytes);
        }
    }
    return std::nullopt;
}

// The bytes of the last frame produceSome returned.
const std::vector<std::byte>& lastBytes(const Analyzer& /*analyzer*/)
{
    return lastProduced;
}

class AnalyzerGrid : public ::testing::TestWithParam<Case>
{
};

TEST_P(AnalyzerGrid, DrawsEveryTraceAndKeepsGoing)
{
    const Case c = GetParam();
    Tracks tracks(c.channels);
    configure(tracks.own(), c.mode, c.timeBase);
    Analyzer analyzer(tracks.own(), tracks.registry);
    analyzer.setColumns(400);
    std::optional<DecodedFrame> frame;
    for (int step = 0; step < 100; ++step) { // One second, a frame per block as a page asks.
        tracks.play(kBlock);
        if (auto produced = produceSome(analyzer, 1)) {
            frame = std::move(produced);
        }
    }
    if (c.frozen) {
        analyzer.setFrozen(true);
        tracks.play(kBlock);
        frame = produceSome(analyzer);
    }
    ASSERT_TRUE(frame) << "no frame";
    EXPECT_EQ(frame->has(protocol::FrameFlag::kFrozen), c.frozen);
    ASSERT_EQ(tracesIn(*frame), expectedTraces(c.mode, c.channels));
    const std::vector<float> loudest = loudestPerTrace(*frame);
    for (std::size_t trace = 0; trace < loudest.size(); ++trace) {
        EXPECT_GT(loudest[trace], 0.01f) << "trace " << trace << " is silent";
    }
    EXPECT_GT(frame->peak, 0.01f) << "readouts measured nothing";
    if (!c.frozen) {
        tracks.play(std::uint64_t{kBlock} * 4);
        EXPECT_TRUE(produceSome(analyzer)) << "frames stopped coming";
    }
}

INSTANTIATE_TEST_SUITE_P(EveryView, AnalyzerGrid, ::testing::ValuesIn(everyCase()), testName);

// A tempo ramp moves each track's sweep length a little from block to block: every lane must still
// be drawn, including one that joins during the ramp and so measures a different length.
class TempoRamp : public ::testing::TestWithParam<Case>
{
};

TEST_P(TempoRamp, DrawsEveryLaneThroughATempoRamp)
{
    const Case c = GetParam();
    Tracks tracks(c.channels);
    configure(tracks.own(), c.mode, TimeBase::kBeats);
    Analyzer analyzer(tracks.own(), tracks.registry);
    analyzer.setColumns(400);
    std::optional<DecodedFrame> frame;
    for (int step = 0; step < 150; ++step) {
        if (step == 60) {
            tracks.join(440.0); // Its sweep length is measured mid-ramp.
        }
        tracks.play(kBlock, 0.02); // 120 to 123 BPM.
        if (auto produced = produceSome(analyzer, 1)) {
            frame = std::move(produced);
        }
    }
    ASSERT_TRUE(frame);
    ASSERT_EQ(tracesIn(*frame), expectedTraces(c.mode, c.channels, 3));
    const std::vector<float> loudest = loudestPerTrace(*frame);
    for (std::size_t trace = 0; trace < loudest.size(); ++trace) {
        EXPECT_GT(loudest[trace], 0.01f) << "trace " << trace << " is silent";
    }
}

std::vector<Case> rampCases()
{
    std::vector<Case> cases;
    for (std::size_t mode = 0; mode < protocol::kDisplayModeCount; ++mode) {
        for (const int channels : {1, 2}) {
            cases.push_back({.mode = static_cast<DisplayMode>(mode),
                             .timeBase = TimeBase::kBeats,
                             .channels = channels});
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(EveryView, TempoRamp, ::testing::ValuesIn(rampCases()), testName);

// A view at the zoom limit can come out a few ulps narrower in the UI's floating point: the engine
// still takes it.
TEST(AnalyzerLimits, AcceptsAViewAtTheZoomLimit)
{
    Tracks tracks(2);
    configure(tracks.own(), DisplayMode::kSingle, TimeBase::kSeconds);
    Analyzer analyzer(tracks.own(), tracks.registry);
    analyzer.setColumns(400);
    tracks.play(9600);
    const double start = 0.3;
    analyzer.setView(start, start + (protocol::kMinimumViewWidth - 1e-15));
    const auto frame = produceSome(analyzer);
    ASSERT_TRUE(frame);
    EXPECT_NEAR(frame->viewEnd - frame->viewStart, protocol::kMinimumViewWidth, 1e-6);
}

// Frozen in Seconds mode and panned as far back as the frame says it can be, every lane still
// holds the span, though another track's capture runs ahead of this one's.
// Switching to Beats then makes frames only when something changes.
TEST(AnalyzerLimits, PansFrozenLanesToTheReportedReach)
{
    Tracks tracks(2);
    configure(tracks.own(), DisplayMode::kMulti, TimeBase::kSeconds);
    tracks.own().settings().set(SettingId::kTimeSpan, 2.0); // Two seconds to pan back through.
    Analyzer analyzer(tracks.own(), tracks.registry);
    analyzer.setColumns(400);
    for (int step = 0; step < 700; ++step) { // Seven seconds.
        tracks.play(kBlock);
        produceSome(analyzer, 1);
    }
    tracks.lead(1, kBlock * 4); // The other track's capture is four blocks ahead.
    analyzer.setFrozen(true);
    auto frame = produceSome(analyzer);
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->has(protocol::FrameFlag::kFrozen));
    const auto reach = get<float>(lastBytes(analyzer), protocol::FrameHeader::kPanReachOffset);
    EXPECT_GT(reach, 0.0f);
    analyzer.setPan(reach);
    frame = produceSome(analyzer);
    ASSERT_TRUE(frame);
    for (const float loudest : loudestPerTrace(*frame)) {
        EXPECT_GT(loudest, 0.01f) << "a lane is blank at the reach";
    }

    tracks.own().settings().set(SettingId::kTimeBase, static_cast<double>(TimeBase::kBeats));
    produceSome(analyzer);
    std::vector<std::byte> bytes;
    EXPECT_FALSE(analyzer.produce(bytes)) << "frames made with nothing new";
}

} // namespace
} // namespace orizon::engine
