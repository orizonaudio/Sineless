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

std::optional<DecodedFrame> produceSome(Analyzer& analyzer, int attempts = 4)
{
    std::vector<std::byte> bytes;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (analyzer.produce(bytes)) {
            return decode(bytes);
        }
    }
    return std::nullopt;
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
        tracks.play(kBlock * 4);
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

} // namespace
} // namespace orizon::engine
