#include "FrameDecoding.h"
#include "SampleRates.h"
#include "engine/Analyzer.h"
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

// A triggered window changes only with new audio: between blocks no frame is made, however often
// the page asks, so no work is done for the same trace.
TEST(PacedWindowsTriggered, DrawNoFrameBetweenBlocks)
{
    PacedHost host(44100.0, 4096); // 93 ms blocks, under 11 a second.
    host.signal = [](std::uint64_t sample) {
        return static_cast<float>(std::sin(static_cast<double>(sample) * 0.0071)); // 50 Hz.
    };
    host.engine.settings().set(SettingId::kTriggerMode, static_cast<double>(TriggerMode::kNormal));
    host.engine.settings().set(SettingId::kTimeSpan, 0.01);
    host.engine.settings().set(SettingId::kTriggerLevel, 0.0);
    host.play(2.0, false);
    std::size_t frames = 0;
    for (int draw = 0; draw < 120; ++draw) {
        host.play(1.0 / 60.0, false);
        std::vector<std::byte> bytes;
        frames += host.analyzer.produce(bytes) ? 1 : 0;
    }
    EXPECT_LE(frames, 2U * 11U + 1U) << "at most one frame per block";
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

} // namespace
} // namespace orizon::engine
