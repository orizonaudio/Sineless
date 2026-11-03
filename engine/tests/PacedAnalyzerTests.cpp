#include "FrameDecoding.h"
#include "SampleRates.h"
#include "engine/Analyzer.h"
#include "engine/Engine.h"
#include "engine/FramePacing.h"
#include "engine/Motion.h"

#include <array>
#include <cmath>
#include <functional>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::FrameFlag;
using protocol::SettingId;
using protocol::TriggerMode;

// A host delivering blocks of a ramp (each sample holds its own index, so a frame says which
// samples it shows) on time, with frames drawn at 60 Hz, on a simulated clock: the analyzer is
// paced by the stamps and the time this host gives it.
class PacedHost
{
public:
    PacedHost(double rate, std::uint64_t block) : rate(rate), block(block)
    {
        engine.prepare(rate);
        engine.settings().set(SettingId::kTimeBase,
                              static_cast<double>(protocol::TimeBase::kSeconds));
        engine.settings().set(SettingId::kChannelViews, 0b0001);
        analyzer.setClock([this] { return nanos(); });
        analyzer.setColumns(8000);
    }

    // Plays `seconds` of host time: blocks as they fall due, the analysis worker's 2 ms steps and
    // a frame at each 60 Hz draw, whose right edge is recorded.
    void play(double seconds, bool draw = true)
    {
        const double end = time + seconds;
        for (; time < end; time += 0.001) {
            while (static_cast<double>(written + block) / rate <= time - kStart) {
                writeBlock();
            }
            if (std::fmod(time * 1000.0, 2.0) < 1.0) {
                analyzer.advance();
            }
            if (draw && time >= nextDraw) {
                nextDraw += 1.0 / 60.0;
                std::vector<std::byte> bytes;
                if (analyzer.produce(bytes)) {
                    last = decode(bytes);
                    if (const auto shown = rightEdge(*last)) {
                        edges.push_back({.seconds = time,
                                         .edge = *shown,
                                         .newest = static_cast<double>(written)});
                    }
                }
            }
        }
    }

    // Plays `seconds` as the editor's frame flow drives the analyzer (editor/src/FrameFlow.cpp):
    // the page grants a credit at each 60 Hz draw; each 2 ms step, while the credit is held, asks
    // for a frame (which takes the credit when one comes) and otherwise advances.
    void flow(double seconds)
    {
        const double end = time + seconds;
        for (; time < end; time += 0.001) {
            while (static_cast<double>(written + block) / rate <= time - kStart) {
                writeBlock();
            }
            if (time >= nextDraw) {
                nextDraw += 1.0 / 60.0;
                credit = true;
            }
            if (std::fmod(time * 1000.0, 2.0) >= 1.0) {
                continue;
            }
            if (!credit) {
                analyzer.advance();
                continue;
            }
            std::vector<std::byte> bytes;
            if (analyzer.produce(bytes)) {
                credit = false;
                last = decode(bytes);
                if (const auto shown = rightEdge(*last); shown && !last->has(FrameFlag::kFrozen)) {
                    edges.push_back(
                        {.seconds = time, .edge = *shown, .newest = static_cast<double>(written)});
                }
            }
        }
    }

    // Plays `seconds` as the macOS editor's native drawing drives the analyzer: a display link at
    // `refreshHertz` calls back `lead` before each refresh, `pacing` keeps the analyzer's clock
    // (set by useDisplayClock) and says which refreshes want a frame, and each 2 ms worker step
    // makes the wanted frame or advances. A frame is recorded at the time it is placed for.
    void link(double seconds, FramePacing& pacing, double refreshHertz, double lead)
    {
        const double end = time + seconds;
        for (; time < end; time += 0.0005) {
            while (static_cast<double>(written + block) / rate <= time - kStart) {
                writeBlock();
            }
            if (time >= nextTarget - lead) {
                if (pacing.callback(nextTarget, time, nanos()).requestFrame) {
                    credit = true;
                }
                nextTarget += 1.0 / refreshHertz;
            }
            if (std::fmod(time * 1000.0, 2.0) >= 0.5) {
                continue;
            }
            if (!credit) {
                analyzer.advance();
                continue;
            }
            std::vector<std::byte> bytes;
            if (analyzer.produce(bytes)) {
                credit = false;
                last = decode(bytes);
                if (const auto shown = rightEdge(*last); shown && !last->has(FrameFlag::kFrozen)) {
                    edges.push_back({.seconds = static_cast<double>(pacing.clock(nanos())) / 1e9,
                                     .edge = *shown,
                                     .newest = static_cast<double>(written)});
                }
            }
        }
    }

    // The analyzer's clock from now on: `pacing`'s, which this host's link() drives.
    void useDisplayClock(FramePacing& pacing)
    {
        analyzer.setClock([this, &pacing] { return pacing.clock(nanos()); });
    }

    // One past the last sample a frame of the ramp shows, from its last column's maximum.
    static std::optional<double> rightEdge(const DecodedFrame& frame)
    {
        if (frame.data.empty()) {
            return std::nullopt;
        }
        return static_cast<double>(frame.data.back()) + 1.0;
    }

    [[nodiscard]] std::uint64_t nanos() const
    {
        return static_cast<std::uint64_t>(std::llround(time * 1e9));
    }

    static constexpr double kStart = 1.0;
    Engine engine;
    Analyzer analyzer{engine};
    double rate;
    std::uint64_t block;
    std::uint64_t written = 0;
    double time = kStart;
    double nextDraw = kStart;
    std::optional<DecodedFrame> last;
    std::vector<EdgeDraw> edges;
    bool credit = false; // flow(): the page waits for a frame; link(): the display link does.
    double nextTarget = kStart + 0.1; // link(): the next refresh's target.
    // The audio, by sample index; the ramp when unset.
    std::function<float(std::uint64_t)> signal;

private:
    void writeBlock()
    {
        std::vector<float> ramp(block);
        for (std::size_t i = 0; i < ramp.size(); ++i) {
            ramp[i] = signal ? signal(written + i) : static_cast<float>(written + i);
        }
        std::array<const float*, 2> channels{ramp.data(), ramp.data()};
        engine.captureRing().write(channels.data(), 2, static_cast<int>(block));
        written += block;
        // The stamp as the audio thread would make it at this time on the simulated clock.
        engine.blockStamp().publish({.index = written, .nanos = nanos(), .paced = true});
    }
};

struct PacedCase
{
    std::string name;
    TriggerMode mode;
    double span;
};

class PacedWindows : public ::testing::TestWithParam<PacedCase>
{
};

// Free-running windows and Auto drawing untriggered end at the paced edge: per frame it moves as
// far as the time drawn, within a millisecond and a column, never past the audio captured, and
// behind it by about a block. With the newest sample as the edge, blocks of 10.7 ms at 60 Hz
// frames moved it by one block or two.
TEST_P(PacedWindows, EndAtThePacedEdge)
{
    PacedHost host(48000.0, 512);
    host.engine.settings().set(SettingId::kTriggerMode, static_cast<double>(GetParam().mode));
    host.engine.settings().set(SettingId::kTimeSpan, GetParam().span);
    host.play(3.0, false); // The clock locks; Auto times out (a ramp below the trigger level).
    host.play(4.0);
    ASSERT_GT(host.edges.size(), 200U);
    std::vector<EdgeDraw> settled(host.edges.begin() + 30, host.edges.end());
    const Motion motion = measureMotion(settled, host.rate);
    const double column = GetParam().span * 1000.0 / 8000.0;
    EXPECT_LE(motion.advanceErrorP99, 1.0 + 2.0 * column);
    EXPECT_FALSE(motion.ahead);
    EXPECT_FALSE(motion.backwards);
    EXPECT_GT(motion.lagMax, 512.0 / 48.0 * 0.5) << "behind the newest audio";
    EXPECT_LE(motion.lagMax, protocol::kPacingDelayCapSeconds * 1000.0 + 1.0);
}

INSTANTIATE_TEST_SUITE_P(
    FreeAndAuto, PacedWindows,
    ::testing::Values(
        PacedCase{.name = "FreeAtHalfASecond", .mode = TriggerMode::kFree, .span = 0.5},
        PacedCase{.name = "NormalAtTwoSeconds", .mode = TriggerMode::kNormal, .span = 2.0},
        PacedCase{.name = "AutoUntriggered", .mode = TriggerMode::kAuto, .span = 0.2}),
    [](const ::testing::TestParamInfo<PacedCase>& info) { return info.param.name; });

// Spans shorter than pacedSpanMinimumSeconds end at the newest sample: their whole trace is new
// every frame, and pacing would only show it later. Just above, they are paced.
TEST(PacedWindowsMinimum, PaceOnlyFromTheMinimumSpan)
{
    for (const double span :
         {protocol::kPacedSpanMinimumSeconds * 0.9, protocol::kPacedSpanMinimumSeconds * 1.1}) {
        SCOPED_TRACE(span);
        PacedHost host(48000.0, 512);
        host.engine.settings().set(SettingId::kTriggerMode,
                                   static_cast<double>(TriggerMode::kFree));
        host.engine.settings().set(SettingId::kTimeSpan, span);
        host.play(3.0);
        const double lag = host.edges.back().newest - host.edges.back().edge;
        if (span < protocol::kPacedSpanMinimumSeconds) {
            EXPECT_LE(lag, 1.0) << "the newest sample";
        } else {
            EXPECT_GT(lag, 1.0) << "paced";
        }
    }
}

// Lengthened from a span drawn at the newest audio to one that is paced, the edge goes on from
// the newest audio it showed: it may wait for the paced edge to catch up, but never steps back.
// With 93 ms blocks the delay is longer than a frame, so the paced edge starts behind it; it
// stepped back by most of a block.
TEST(PacedWindowsMinimum, NeverStepBackWhenPacingStarts)
{
    PacedHost host(44100.0, 4096);
    host.engine.settings().set(SettingId::kTriggerMode, static_cast<double>(TriggerMode::kFree));
    host.engine.settings().set(SettingId::kTimeSpan, protocol::kPacedSpanMinimumSeconds * 0.9);
    host.play(3.0);
    host.engine.settings().set(SettingId::kTimeSpan, 0.5);
    host.play(1.0);
    // Columns end on the last complete one: up to a column short of the edge.
    const double column = 0.5 * host.rate / 8000.0;
    for (std::size_t index = 1; index < host.edges.size(); ++index) {
        ASSERT_GE(host.edges[index].edge, host.edges[index - 1].edge - column)
            << "stepped back at " << host.edges[index].seconds;
    }
    EXPECT_FALSE(measureMotion(host.edges, host.rate).ahead);
}

// ----- macOS native drawing: the display clock -----

struct NativeCase
{
    double rate;
    std::uint64_t block;
    double refresh;
};

std::vector<NativeCase> nativeCases()
{
    std::vector<NativeCase> cases;
    for (const double rate : kSampleRates) {
        // Blocks of 32 and 4096 samples at 48 kHz, and about 10.7 ms, at every rate.
        for (const double blockAt48 : {32.0, 512.0, 4096.0}) {
            cases.push_back(
                {.rate = rate,
                 .block = static_cast<std::uint64_t>(std::llround(blockAt48 * rate / 48000.0)),
                 .refresh = 60.0});
        }
    }
    cases.push_back({.rate = 48000.0, .block = 512, .refresh = 120.0});
    cases.push_back({.rate = 48000.0, .block = 512, .refresh = 144.0});
    return cases;
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const NativeCase& native, std::ostream* out)
{
    *out << native.rate << " Hz, blocks of " << native.block << ", " << native.refresh
         << " Hz refreshes";
}

class PacedNative : public ::testing::TestWithParam<NativeCase>
{
};

// Drawn natively, frames are made on the worker's 2 ms steps after the display link asks, at no
// steady time; placed at the display clock, which steps by whole refreshes, the free-running edge
// still moves per frame as far as the frames are shown apart, within a millisecond. Placed when
// made, as on the page, the same frames missed that (PlacedWhenMadeMissesTheMillisecond).
TEST_P(PacedNative, MovesTheEdgeEvenlyAtTheDisplayClock)
{
    const auto [rate, block, refresh] = GetParam();
    PacedHost host(rate, block);
    host.engine.settings().set(SettingId::kTriggerMode, static_cast<double>(TriggerMode::kFree));
    host.engine.settings().set(SettingId::kTimeSpan, 0.5);
    FramePacing pacing(refresh);
    host.useDisplayClock(pacing);
    host.link(3.0, pacing, refresh, 0.05); // The clocks lock.
    host.edges.clear();
    host.link(4.0, pacing, refresh, 0.05);
    ASSERT_GT(host.edges.size(), 150U);
    const Motion motion = measureMotion(host.edges, host.rate);
    const double column = 0.5 * 1000.0 / 8000.0;
    EXPECT_LE(motion.advanceErrorP99, 1.0 + 2.0 * column);
    EXPECT_LE(motion.advanceErrorMax, 2.0 + 2.0 * column);
    EXPECT_FALSE(motion.ahead);
    EXPECT_FALSE(motion.backwards);
}

INSTANTIATE_TEST_SUITE_P(RatesAndBlocks, PacedNative, ::testing::ValuesIn(nativeCases()));

// The same host and frames, placed when made (the analyzer reading the host's time): the 2 ms
// worker steps reach the edge, which misses the millisecond. So the test above tells the two
// apart.
TEST(PacedNativeContrast, PlacedWhenMadeMissesTheMillisecond)
{
    PacedHost host(48000.0, 512);
    host.engine.settings().set(SettingId::kTriggerMode, static_cast<double>(TriggerMode::kFree));
    host.engine.settings().set(SettingId::kTimeSpan, 0.5);
    FramePacing pacing(60.0);
    host.link(3.0, pacing, 60.0, 0.05);
    host.edges.clear();
    host.link(4.0, pacing, 60.0, 0.05);
    // Recorded at the display clock (when the frames are shown); the analyzer placed them when
    // made.
    EXPECT_GT(measureMotion(host.edges, host.rate).advanceErrorP99, 1.0);
}

// Spans shorter than pacedSpanMinimumSeconds end at the newest sample with the display clock too;
// just above, they are paced at it, and neither goes back.
TEST(PacedNativeMinimum, PacesOnlyFromTheMinimumSpanAtTheDisplayClock)
{
    for (const double span :
         {protocol::kPacedSpanMinimumSeconds * 0.9, protocol::kPacedSpanMinimumSeconds * 1.1}) {
        SCOPED_TRACE(span);
        PacedHost host(48000.0, 512);
        host.engine.settings().set(SettingId::kTriggerMode,
                                   static_cast<double>(TriggerMode::kFree));
        host.engine.settings().set(SettingId::kTimeSpan, span);
        FramePacing pacing(60.0);
        host.useDisplayClock(pacing);
        host.link(3.0, pacing, 60.0, 0.05);
        const double lag = host.edges.back().newest - host.edges.back().edge;
        if (span < protocol::kPacedSpanMinimumSeconds) {
            EXPECT_LE(lag, 1.0) << "the newest sample";
        } else {
            EXPECT_GT(lag, 1.0) << "paced";
        }
        EXPECT_FALSE(measureMotion(host.edges, host.rate).backwards);
    }
}

// While the page waits for a frame, the editor asks for one on every step and never advances, as
// a freeze keeps it waiting (frozen frames do not change). Frames keep the block clock fed, so
// once unfrozen the trace moves evenly at once: with the clock starved for the freeze it would
// start over and step by whole blocks until it locked again (16 blocks, 0.2 s here).
TEST(PacedFreeze, KeepsTheClockFedWhileTheEditorWaitsForAFrame)
{
    PacedHost host(48000.0, 512);
    host.engine.settings().set(SettingId::kTriggerMode, static_cast<double>(TriggerMode::kFree));
    host.engine.settings().set(SettingId::kTimeSpan, 0.5);
    host.flow(3.0); // The clock locks.
    host.analyzer.setFrozen(true);
    host.flow(1.0);
    host.analyzer.setFrozen(false);
    host.edges.clear();
    host.flow(0.3);
    ASSERT_GT(host.edges.size(), 10U);
    const Motion motion = measureMotion(host.edges, host.rate);
    const double column = 0.5 * 1000.0 / 8000.0;
    EXPECT_LE(motion.advanceErrorMax, 1.0 + 2.0 * column) << "the trace stepped after the freeze";
    EXPECT_FALSE(motion.backwards);
}

// A triggered window changes only with new audio: between blocks no frame is made and nothing
// is read, however often the page asks, so no work is done for the same trace. At a span that is
// paced, in Normal and in Auto once it triggers.
TEST(PacedWindowsTriggered, DrawNoFrameBetweenBlocks)
{
    for (const TriggerMode mode : {TriggerMode::kNormal, TriggerMode::kAuto}) {
        SCOPED_TRACE(static_cast<int>(mode));
        PacedHost host(44100.0, 4096); // 93 ms blocks, under 11 a second.
        host.signal = [](std::uint64_t sample) {
            return static_cast<float>(std::sin(static_cast<double>(sample) * 0.0071)); // 50 Hz.
        };
        host.engine.settings().set(SettingId::kTriggerMode, static_cast<double>(mode));
        host.engine.settings().set(SettingId::kTimeSpan, 0.5);
        host.engine.settings().set(SettingId::kTriggerLevel, 0.0);
        ASSERT_GE(0.5, protocol::kPacedSpanMinimumSeconds);
        host.play(2.0, false);
        std::vector<std::byte> first;
        ASSERT_TRUE(host.analyzer.produce(first)); // The first frame reads its span.
        std::size_t frames = 0;
        std::size_t idle = 0;
        for (int draw = 0; draw < 120; ++draw) {
            const std::uint64_t written = host.written;
            host.play(1.0 / 60.0, false);
            const std::uint64_t read = host.analyzer.work().samplesRead;
            std::vector<std::byte> bytes;
            if (host.analyzer.produce(bytes)) {
                ++frames;
                ASSERT_TRUE(decode(bytes).has(FrameFlag::kTriggered));
            }
            if (host.written == written) {
                ++idle;
                EXPECT_EQ(host.analyzer.work().samplesRead, read) << "read between blocks";
            }
        }
        EXPECT_LE(frames, 2U * 11U + 1U) << "at most one frame per block";
        EXPECT_GT(idle, 60U);
    }
}

// Auto drawing untriggered follows the paced edge, as a free-running window does: a frame at
// nearly every draw, also when blocks come 93 ms apart, not only when a block arrives (about 11
// a second, a visible stutter).
TEST(PacedWindowsTriggered, DrawAutoUntriggeredAtEveryDraw)
{
    PacedHost host(44100.0, 4096);
    host.signal = [](std::uint64_t) { return -0.5F; }; // Nothing crosses the level.
    host.engine.settings().set(SettingId::kTriggerMode, static_cast<double>(TriggerMode::kAuto));
    host.engine.settings().set(SettingId::kTimeSpan, 0.5);
    host.play(3.0); // The clock locks; Auto times out.
    std::size_t frames = 0;
    for (int draw = 0; draw < 120; ++draw) {
        host.play(1.0 / 60.0, false);
        std::vector<std::byte> bytes;
        if (host.analyzer.produce(bytes)) {
            ++frames;
            ASSERT_FALSE(decode(bytes).has(FrameFlag::kTriggered));
        }
    }
    EXPECT_GE(frames, 110U) << "a frame only when a block arrives";
}

// Freezing a paced window keeps the window last shown, also when another block arrived before
// the freeze and the delay is as large as the hosts it covers: the longest span, at every rate,
// with blocks of about 93 ms. Not zoomed and zoomed.
TEST(PacedFreeze, KeepsTheWindowLastShown)
{
    for (const double rate : kSampleRates) {
        for (const bool zoomed : {false, true}) {
            SCOPED_TRACE(std::to_string(rate) + (zoomed ? " zoomed" : ""));
            const auto block = static_cast<std::uint64_t>(std::llround(0.0929 * rate));
            PacedHost host(rate, block);
            host.analyzer.setColumns(1000);
            host.engine.settings().set(SettingId::kTimeSpan, 4.0);
            if (zoomed) {
                host.analyzer.setView(0.5, 1.0);
            }
            host.play(7.0);
            ASSERT_TRUE(host.last);
            const DecodedFrame live = *host.last;
            ASSERT_FALSE(live.has(FrameFlag::kFrozen));
            // The next block arrives, then the freeze.
            host.play(static_cast<double>(block) / rate + 0.001, false);
            host.analyzer.setFrozen(true);
            std::vector<std::byte> bytes;
            ASSERT_TRUE(host.analyzer.produce(bytes));
            const DecodedFrame frozen = decode(bytes);
            ASSERT_TRUE(frozen.has(FrameFlag::kFrozen));
            const auto column = static_cast<float>(4.0 * rate / 1000.0);
            EXPECT_NEAR(frozen.data.front(), live.data.front(), column) << "the first sample";
            EXPECT_NEAR(frozen.data.back(), live.data.back(), column) << "the last sample";
            EXPECT_FALSE(frozen.has(FrameFlag::kLimited));
        }
    }
}

// Each live frame records its edge for the benchmark, but in a host nothing takes them: over a
// long session the log keeps only the newest kEdgeRecords, so an instance's memory stays bounded.
TEST(EdgeLog, KeepsOnlyTheNewestEdgesWhenNothingTakesThem)
{
    Engine engine;
    const std::uint64_t recorded = 3 * Engine::kEdgeRecords;
    for (std::uint64_t index = 0; index < recorded; ++index) {
        engine.recordEdge({.nanos = index, .edge = index, .newest = index});
    }
    const auto edges = engine.takeEdges();
    ASSERT_FALSE(edges.empty());
    EXPECT_LE(edges.size(), Engine::kEdgeRecords);
    EXPECT_EQ(edges.back().edge, recorded - 1) << "the newest kept";
    EXPECT_TRUE(engine.takeEdges().empty()) << "taken";
}

} // namespace
} // namespace orizon::engine
