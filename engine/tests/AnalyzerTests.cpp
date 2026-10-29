#include "FrameDecoding.h"
#include "engine/Analyzer.h"

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::ChannelView;
using protocol::FrameFlag;
using protocol::FrameHeader;
using protocol::FrameMode;
using protocol::SettingId;

constexpr double kSampleRate = 48000.0;

class AnalyzerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        engine.prepare(kSampleRate);
        // These tests use the seconds time base at a 20 ms span unless they set another.
        engine.settings().set(SettingId::kTimeBase,
                              static_cast<double>(protocol::TimeBase::kSeconds));
        engine.settings().set(SettingId::kTimeSpan, 0.02);
    }

    // Appends a tone on L and the same tone at half amplitude on R.
    void writeTone(double frequency, double seconds, double amplitude)
    {
        const auto count = static_cast<std::size_t>(seconds * kSampleRate);
        std::vector<float> left(count);
        std::vector<float> right(count);
        const std::uint64_t start = engine.captureRing().writeIndex();
        for (std::size_t i = 0; i < count; ++i) {
            const double time = static_cast<double>(start + i) / kSampleRate;
            left[i] =
                static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * frequency * time));
            right[i] = left[i] * 0.5f;
        }
        std::array<const float*, 2> channels{left.data(), right.data()};
        engine.captureRing().write(channels.data(), 2, static_cast<int>(count));
    }

    void writeSilence(double seconds) { writeTone(0.0, seconds, 0.0); }

    // Appends `count` samples that hold their own index on both channels, so a frame's raw points
    // say which samples it shows.
    void writeRamp(std::size_t count)
    {
        std::vector<float> ramp(count);
        const std::uint64_t start = engine.captureRing().writeIndex();
        for (std::size_t i = 0; i < count; ++i) {
            ramp[i] = static_cast<float>(start + i);
        }
        std::array<const float*, 2> channels{ramp.data(), ramp.data()};
        engine.captureRing().write(channels.data(), 2, static_cast<int>(count));
    }

    void setTriggerMode(protocol::TriggerMode mode)
    {
        engine.settings().set(SettingId::kTriggerMode, static_cast<double>(mode));
    }

    // The pan reach of the next frame, in seconds; NaN if none comes.
    float producePanReach()
    {
        std::vector<std::byte> bytes;
        if (!analyzer.produce(bytes)) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        return get<float>(bytes, FrameHeader::kPanReachOffset);
    }

    std::optional<DecodedFrame> produce()
    {
        std::vector<std::byte> bytes;
        if (!analyzer.produce(bytes)) {
            return std::nullopt;
        }
        return decode(bytes);
    }

    Engine engine;
    Analyzer analyzer{engine};
};

// Views keep their columns from frame to frame; what they show must equal a fresh reduction.
TEST_F(AnalyzerTest, KeepsRollColumnsInStepWithAFreshReduction)
{
    engine.settings().set(SettingId::kTimeSpan, 2.0);        // Rolls.
    engine.settings().set(SettingId::kChannelViews, 0b0101); // L and M.
    analyzer.setColumns(400);
    writeTone(220.0, 2.5, 0.5);
    std::optional<DecodedFrame> frame;
    for (int step = 0; step < 30; ++step) {
        writeTone(220.0 + step, 0.01, 0.5);
        if (auto produced = produce()) {
            frame = std::move(produced);
        }
    }
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->has(FrameFlag::kFreeRunning));
    Analyzer fresh(engine);
    fresh.setColumns(400);
    std::vector<std::byte> bytes;
    ASSERT_TRUE(fresh.produce(bytes));
    EXPECT_EQ(decode(bytes).data, frame->data) << "frame by frame equals all at once";
}

TEST_F(AnalyzerTest, KeepsSweepColumnsInStepWithAFreshReduction)
{
    engine.settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kBeats));
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBeat));
    engine.settings().set(SettingId::kChannelViews, 0b0011);
    analyzer.setColumns(300);
    std::uint64_t written = 0;
    std::optional<DecodedFrame> frame;
    for (int step = 0; step < 80; ++step) {
        writeTone(330.0 + step, 0.01, 0.5);
        engine.timeline().advance({.playing = true,
                                   .positionValid = true,
                                   .quarterNotes = static_cast<double>(written) / 24000.0,
                                   .tempoValid = true,
                                   .tempo = 120.0},
                                  480, kSampleRate);
        written += 480;
        if (auto produced = produce()) {
            frame = std::move(produced);
        }
    }
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->has(FrameFlag::kSynced));
    ASSERT_EQ(frame->mode, FrameMode::kMinMax);
    Analyzer fresh(engine);
    fresh.setColumns(300);
    std::vector<std::byte> bytes;
    ASSERT_TRUE(fresh.produce(bytes));
    EXPECT_EQ(decode(bytes).data, frame->data) << "frame by frame equals all at once";
}

// In Beats mode nothing triggers and the trigger source cannot be set: readouts measure the Mid,
// as the multi-track lanes do, whatever views are on show.
TEST_F(AnalyzerTest, MeasuresTheMidInBeatsMode)
{
    engine.settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kBeats));
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBeat));
    engine.settings().set(SettingId::kChannelViews, 0b0010); // R only on show.
    analyzer.setColumns(300);
    std::uint64_t written = 0;
    std::optional<DecodedFrame> frame;
    for (int step = 0; step < 80; ++step) {
        writeTone(330.0, 0.01, 0.5);
        engine.timeline().advance({.playing = true,
                                   .positionValid = true,
                                   .quarterNotes = static_cast<double>(written) / 24000.0,
                                   .tempoValid = true,
                                   .tempo = 120.0},
                                  480, kSampleRate);
        written += 480;
        if (auto produced = produce()) {
            frame = std::move(produced);
        }
    }
    ASSERT_TRUE(frame);
    EXPECT_NEAR(frame->peak, 0.375f, 0.01f) << "Mid, (L + R) / 2, not R alone (0.25)";
}

// Every display mode the protocol has.
std::vector<protocol::DisplayMode> everyDisplayMode()
{
    std::vector<protocol::DisplayMode> modes;
    modes.reserve(protocol::kDisplayModeCount);
    for (std::size_t mode = 0; mode < protocol::kDisplayModeCount; ++mode) {
        modes.push_back(static_cast<protocol::DisplayMode>(mode));
    }
    return modes;
}

// Plays half a second of a tone, then `silence` seconds of silence, in Beats mode at 120 BPM with
// frames on the way: the last sweep shown is of the tone, the newest audio silent.
void playBeatsThenSilence(Engine& engine, Analyzer& analyzer, double silence = 1.0)
{
    engine.settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kBeats));
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBeat));
    analyzer.setColumns(300);
    std::vector<std::byte> bytes;
    const auto steps = 50 + static_cast<std::uint64_t>(std::llround(silence * 100.0));
    for (std::uint64_t step = 0; step < steps; ++step) {
        std::vector<float> block(480);
        for (std::size_t i = 0; i < block.size() && step < 50; ++i) {
            const double time = static_cast<double>(step * 480 + i) / kSampleRate;
            block[i] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 330.0 * time));
        }
        std::array<const float*, 2> channels{block.data(), block.data()};
        engine.captureRing().write(channels.data(), 2, 480);
        engine.timeline().advance({.playing = true,
                                   .positionValid = true,
                                   .quarterNotes = static_cast<double>(step * 480) / 24000.0,
                                   .tempoValid = true,
                                   .tempo = 120.0},
                                  480, kSampleRate);
        (void)analyzer.produce(bytes);
    }
}

// An instance the host has not played through has no tempo: in Beats, its clock's 120 BPM is only a
// default, and a sweep placed by it would show bars that are not the project's. Until the host
// reports a tempo, one frame says so and nothing is drawn; then the sweep follows the host's.
// Audio without a tempo, as from a host that reports none, is not placed either. In every display
// mode.
TEST_F(AnalyzerTest, DrawsNoBeatsSweepUntilTheHostReportsATempo)
{
    for (const auto display : everyDisplayMode()) {
        SCOPED_TRACE(::testing::Message() << "display " << static_cast<int>(display));
        Engine fresh;
        fresh.prepare(kSampleRate);
        fresh.settings().set(SettingId::kDisplayMode, static_cast<double>(display));
        Analyzer view(fresh);
        view.setColumns(300);
        std::vector<float> block(480, 0.25f);
        std::array<const float*, 2> channels{block.data(), block.data()};
        const auto play = [&](bool tempoValid, int blocks) {
            for (int step = 0; step < blocks; ++step) {
                fresh.captureRing().write(channels.data(), 2, 480);
                fresh.timeline().advance({.playing = true,
                                          .positionValid = true,
                                          .tempoValid = tempoValid,
                                          .tempo = 90.0},
                                         480, kSampleRate);
            }
        };
        std::vector<std::byte> bytes;
        ASSERT_TRUE(view.produce(bytes)) << "it says why nothing is drawn";
        DecodedFrame frame = decode(bytes);
        EXPECT_TRUE(frame.has(FrameFlag::kSynced));
        EXPECT_TRUE(frame.has(FrameFlag::kNoTempo));
        EXPECT_EQ(frame.pointCount, 0U);
        EXPECT_EQ(get<float>(bytes, FrameHeader::kSpanQuarterNotesOffset), 0.0f)
            << "no bars made up";
        EXPECT_FALSE(view.produce(bytes)) << "once";

        play(false, 20);
        EXPECT_FALSE(view.produce(bytes)) << "audio without a tempo is not placed";

        play(true, 20);
        ASSERT_TRUE(view.produce(bytes));
        frame = decode(bytes);
        EXPECT_FALSE(frame.has(FrameFlag::kNoTempo));
        EXPECT_GT(frame.pointCount, 0U);
        EXPECT_GT(get<float>(bytes, FrameHeader::kSpanQuarterNotesOffset), 0.0f)
            << "bars placed by the host";
    }
}

// Without a host tempo, the frame saying so follows what the user does: frozen and unfrozen, so the
// page's Freeze button and chips follow it. A freeze made before the tempo holds none, so it
// places nothing even once the host reports one; live again, the sweep follows the host. In every
// display mode.
TEST_F(AnalyzerTest, SaysThereIsNoTempoFrozenAndUnfrozen)
{
    for (const auto display : everyDisplayMode()) {
        SCOPED_TRACE(::testing::Message() << "display " << static_cast<int>(display));
        Engine fresh;
        fresh.prepare(kSampleRate);
        fresh.settings().set(SettingId::kDisplayMode, static_cast<double>(display));
        Analyzer view(fresh);
        view.setColumns(300);
        std::vector<float> block(480, 0.25f);
        std::array<const float*, 2> channels{block.data(), block.data()};
        const auto play = [&](bool tempoValid) {
            for (int step = 0; step < 20; ++step) {
                fresh.captureRing().write(channels.data(), 2, 480);
                fresh.timeline().advance({.playing = true,
                                          .positionValid = true,
                                          .tempoValid = tempoValid,
                                          .tempo = 90.0},
                                         480, kSampleRate);
            }
        };
        const auto noTempoFrame = [&](bool frozen) {
            std::vector<std::byte> bytes;
            ASSERT_TRUE(view.produce(bytes));
            const DecodedFrame frame = decode(bytes);
            EXPECT_TRUE(frame.has(FrameFlag::kNoTempo));
            EXPECT_EQ(frame.has(FrameFlag::kFrozen), frozen);
            EXPECT_EQ(frame.pointCount, 0U);
        };
        std::vector<std::byte> bytes;

        noTempoFrame(false);
        play(false);
        view.setFrozen(true);
        noTempoFrame(true);
        play(true); // The host reports a tempo while frozen.
        EXPECT_FALSE(view.produce(bytes)) << "the freeze holds no tempo: still nothing placed";
        view.setFrozen(false);
        ASSERT_TRUE(view.produce(bytes)) << "live, the sweep follows the host";
        EXPECT_FALSE(decode(bytes).has(FrameFlag::kNoTempo));
        EXPECT_GT(decode(bytes).pointCount, 0U);
    }
}

// Switched to Seconds from Beats before the host reported a tempo, with nothing to trigger on,
// Normal and Single show the newest span at once, as after a Beats sweep. In every display mode.
TEST_F(AnalyzerTest, ShowsTheNewestSpanOnSwitchingToSecondsBeforeATempo)
{
    for (const auto display : everyDisplayMode()) {
        for (const auto mode : {protocol::TriggerMode::kNormal, protocol::TriggerMode::kSingle}) {
            SCOPED_TRACE(::testing::Message() << "display " << static_cast<int>(display)
                                              << ", trigger " << static_cast<int>(mode));
            Engine fresh;
            fresh.prepare(kSampleRate);
            fresh.settings().set(SettingId::kDisplayMode, static_cast<double>(display));
            fresh.settings().set(SettingId::kTriggerMode, static_cast<double>(mode));
            fresh.settings().set(SettingId::kTimeSpan, 0.02);
            Analyzer view(fresh);
            view.setColumns(300);
            std::vector<std::byte> bytes;
            ASSERT_TRUE(view.produce(bytes));
            ASSERT_TRUE(decode(bytes).has(FrameFlag::kNoTempo));
            std::vector<float> silence(24000, 0.0f);
            std::array<const float*, 2> channels{silence.data(), silence.data()};
            fresh.captureRing().write(channels.data(), 2, static_cast<int>(silence.size()));
            fresh.settings().set(SettingId::kTimeBase,
                                 static_cast<double>(protocol::TimeBase::kSeconds));
            ASSERT_TRUE(view.produce(bytes)) << "the switch shows at once";
            const DecodedFrame frame = decode(bytes);
            EXPECT_FALSE(frame.has(FrameFlag::kSynced));
            EXPECT_FALSE(frame.has(FrameFlag::kNoTempo));
            EXPECT_FALSE(frame.has(FrameFlag::kTriggered));
        }
    }
}

// Switched from Beats to Seconds with nothing to trigger on, Normal and Single show the newest
// span at once, untriggered, rather than wait on a Beats sweep or show the capture's oldest
// audio as if it were now; then they wait for a trigger as usual. In every display mode.
TEST_F(AnalyzerTest, ShowsTheNewestSpanOnSwitchingFromBeatsToSeconds)
{
    for (const auto display : everyDisplayMode()) {
        for (const auto mode : {protocol::TriggerMode::kNormal, protocol::TriggerMode::kSingle}) {
            SCOPED_TRACE(::testing::Message() << "display " << static_cast<int>(display)
                                              << ", trigger " << static_cast<int>(mode));
            Engine fresh;
            fresh.prepare(kSampleRate);
            fresh.settings().set(SettingId::kDisplayMode, static_cast<double>(display));
            fresh.settings().set(SettingId::kTriggerMode, static_cast<double>(mode));
            fresh.settings().set(SettingId::kTimeSpan, 0.02);
            Analyzer view(fresh);
            playBeatsThenSilence(fresh, view);
            fresh.settings().set(SettingId::kTimeBase,
                                 static_cast<double>(protocol::TimeBase::kSeconds));
            std::vector<std::byte> bytes;
            ASSERT_TRUE(view.produce(bytes)) << "the switch shows at once";
            const DecodedFrame frame = decode(bytes);
            EXPECT_FALSE(frame.has(FrameFlag::kSynced));
            EXPECT_FALSE(frame.has(FrameFlag::kTriggered));
            EXPECT_EQ(frame.peak, 0.0f) << "the newest span, silent, not the tone a second ago";
            EXPECT_FALSE(view.produce(bytes)) << "then it waits for a trigger";
        }
    }
}

// Every display mode, at a triggered span and at a rolling one.
std::vector<std::pair<protocol::DisplayMode, double>> displaysAndSpans()
{
    constexpr std::array kSpans{0.02, 2.0};
    std::vector<std::pair<protocol::DisplayMode, double>> cases;
    cases.reserve(protocol::kDisplayModeCount * kSpans.size());
    for (const auto display : everyDisplayMode()) {
        for (const double span : kSpans) {
            cases.emplace_back(display, span);
        }
    }
    return cases;
}

// Frozen in Beats and switched to Seconds, the view opens on the newest audio held, as a freeze
// made in Seconds does, not on the oldest. In every display mode, triggered and rolling.
TEST_F(AnalyzerTest, OpensOnTheNewestHeldAudioWhenFrozenInBeatsThenSeconds)
{
    // A triggered span, and a rolling one, whose view ends at an anchor of its own.
    for (const auto& [display, span] : displaysAndSpans()) {
        SCOPED_TRACE(::testing::Message()
                     << "display " << static_cast<int>(display) << ", span " << span);
        Engine fresh;
        fresh.prepare(kSampleRate);
        fresh.settings().set(SettingId::kDisplayMode, static_cast<double>(display));
        fresh.settings().set(SettingId::kTriggerMode,
                             static_cast<double>(protocol::TriggerMode::kNormal));
        fresh.settings().set(SettingId::kTimeSpan, span);
        Analyzer view(fresh);
        playBeatsThenSilence(fresh, view, 3.0); // Longer than the rolling span.
        view.setFrozen(true);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(view.produce(bytes));
        fresh.settings().set(SettingId::kTimeBase,
                             static_cast<double>(protocol::TimeBase::kSeconds));
        ASSERT_TRUE(view.produce(bytes));
        const DecodedFrame frame = decode(bytes);
        EXPECT_TRUE(frame.has(FrameFlag::kFrozen));
        EXPECT_FALSE(frame.has(FrameFlag::kSynced));
        EXPECT_EQ(frame.peak, 0.0f) << "the silence before the freeze, not the tone before it";
        EXPECT_GT(get<float>(bytes, FrameHeader::kPanReachOffset), 1.0f)
            << "and can pan back into the tone";
    }
}

// Plays a 330 Hz tone of `amplitude` for `seconds` in Seconds mode, with a frame at the end.
void playSecondsTone(Engine& engine, Analyzer& analyzer, double seconds, double amplitude)
{
    engine.settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kSeconds));
    std::vector<float> tone(static_cast<std::size_t>(seconds * kSampleRate));
    const std::uint64_t start = engine.captureRing().writeIndex();
    for (std::size_t i = 0; i < tone.size(); ++i) {
        const double time = static_cast<double>(start + i) / kSampleRate;
        tone[i] = static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * 330.0 * time));
    }
    std::array<const float*, 2> channels{tone.data(), tone.data()};
    engine.captureRing().write(channels.data(), 2, static_cast<int>(tone.size()));
    std::vector<std::byte> bytes;
    (void)analyzer.produce(bytes);
}

// Back in Beats after Seconds, before the host has reported a tempo, the page is told again: it
// shows the Seconds frame until then.
TEST_F(AnalyzerTest, SaysThereIsNoTempoAgainBackFromSeconds)
{
    for (const auto display : everyDisplayMode()) {
        SCOPED_TRACE(::testing::Message() << "display " << static_cast<int>(display));
        Engine fresh;
        fresh.prepare(kSampleRate);
        fresh.settings().set(SettingId::kDisplayMode, static_cast<double>(display));
        Analyzer view(fresh);
        view.setColumns(300);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(view.produce(bytes));
        ASSERT_TRUE(decode(bytes).has(FrameFlag::kNoTempo));
        // A page reloaded after its process crashed asks for a redraw: it is told again.
        view.redraw();
        ASSERT_TRUE(view.produce(bytes)) << "told the reloaded page";
        EXPECT_TRUE(decode(bytes).has(FrameFlag::kNoTempo));
        playSecondsTone(fresh, view, 0.5, 0.25); // Seconds, a frame drawn.
        fresh.settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kBeats));
        ASSERT_TRUE(view.produce(bytes)) << "told again";
        EXPECT_TRUE(decode(bytes).has(FrameFlag::kNoTempo));
        EXPECT_TRUE(decode(bytes).has(FrameFlag::kSynced));
    }
}

// Frozen in Seconds, a look at Beats and back keeps the view where it was frozen, on its trigger
// point, in every display mode.
TEST_F(AnalyzerTest, KeepsTheFrozenSecondsViewAcrossATripToBeats)
{
    for (const auto display : everyDisplayMode()) {
        SCOPED_TRACE(::testing::Message() << "display " << static_cast<int>(display));
        Engine fresh;
        fresh.prepare(kSampleRate);
        fresh.settings().set(SettingId::kDisplayMode, static_cast<double>(display));
        fresh.settings().set(SettingId::kTriggerMode,
                             static_cast<double>(protocol::TriggerMode::kNormal));
        fresh.settings().set(SettingId::kTimeSpan, 0.02);
        Analyzer view(fresh);
        view.setColumns(300);
        playSecondsTone(fresh, view, 1.5, 0.25);
        view.setFrozen(true);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(view.produce(bytes));
        const DecodedFrame frozen = decode(bytes);
        ASSERT_TRUE(frozen.has(FrameFlag::kTriggered));

        fresh.settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kBeats));
        ASSERT_TRUE(view.produce(bytes));
        fresh.settings().set(SettingId::kTimeBase,
                             static_cast<double>(protocol::TimeBase::kSeconds));
        ASSERT_TRUE(view.produce(bytes));
        const DecodedFrame back = decode(bytes);
        EXPECT_TRUE(back.has(FrameFlag::kTriggered)) << "still on its trigger point";
        EXPECT_EQ(back.data, frozen.data) << "the same window as when frozen";
    }
}

// Live in Seconds, then a while in Beats, then frozen and back to Seconds: the view opens on the
// newest audio held, not on the Seconds window shown before the Beats spell. In every display
// mode.
TEST_F(AnalyzerTest, ForgetsTheSecondsWindowShownBeforeASpellInBeats)
{
    for (const auto display : everyDisplayMode()) {
        SCOPED_TRACE(::testing::Message() << "display " << static_cast<int>(display));
        Engine fresh;
        fresh.prepare(kSampleRate);
        fresh.settings().set(SettingId::kDisplayMode, static_cast<double>(display));
        fresh.settings().set(SettingId::kTriggerMode,
                             static_cast<double>(protocol::TriggerMode::kNormal));
        fresh.settings().set(SettingId::kTimeSpan, 0.02);
        Analyzer view(fresh);
        view.setColumns(300);
        playSecondsTone(fresh, view, 0.5, 0.25); // A Seconds window of a tone.
        playBeatsThenSilence(fresh, view);       // Then Beats, ending in silence.
        view.setFrozen(true);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(view.produce(bytes));
        fresh.settings().set(SettingId::kTimeBase,
                             static_cast<double>(protocol::TimeBase::kSeconds));
        ASSERT_TRUE(view.produce(bytes));
        EXPECT_EQ(decode(bytes).peak, 0.0f) << "the silence before the freeze";
    }
}

TEST_F(AnalyzerTest, ProducesNothingUntilTheWidthIsKnown)
{
    writeTone(440.0, 0.5, 0.5);
    EXPECT_FALSE(produce());
    analyzer.setColumns(400);
    EXPECT_TRUE(produce());
}

TEST_F(AnalyzerTest, ProducesATriggeredMinMaxFrameWithReadouts)
{
    writeTone(440.0, 0.5, 0.5);
    analyzer.setColumns(400);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->mode, FrameMode::kMinMax);
    EXPECT_TRUE(frame->has(FrameFlag::kTriggered));
    EXPECT_FALSE(frame->has(FrameFlag::kFreeRunning));
    EXPECT_EQ(frame->views, 0b0011) << "default views: L + R";
    EXPECT_EQ(frame->pointCount, 400u);
    EXPECT_EQ(frame->data.size(), 2u * 400u * 2u) << "two views of min/max pairs";

    // Default trigger source is Mid = (L + R) / 2 = 0.75 L, so readouts see amplitude 0.375.
    EXPECT_NEAR(frame->peak, 0.375f, 1e-3f);
    // 20 ms holds 8.8 cycles of 440 Hz; RMS over a partial cycle deviates by about 1%.
    EXPECT_NEAR(frame->rms, 0.375f / std::numbers::sqrt2_v<float>, 0.02f * 0.265f);
    EXPECT_NEAR(frame->frequency, 440.0f, 1.0f);
}

TEST_F(AnalyzerTest, AlignsTheTriggerAtTenPercentWithSubSampleAccuracy)
{
    writeTone(440.0, 0.5, 0.5);
    engine.settings().set(SettingId::kChannelViews, 0b0001);
    engine.settings().set(SettingId::kTimeSpan, 0.0005); // 24 samples: raw points.
    analyzer.setColumns(400);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    ASSERT_EQ(frame->pointCount, 24u);
    // The trigger (rising zero crossing) sits 2.4 samples into the span; interpolating the
    // raw points there must give the trigger level.
    const double position = frame->startFraction + kTriggerPosition * 24.0;
    const auto index = static_cast<std::size_t>(position);
    const double fraction = position - static_cast<double>(index);
    const double value =
        frame->data[index] + fraction * (frame->data[index + 1] - frame->data[index]);
    EXPECT_NEAR(value, 0.0, 2e-3);
    EXPECT_LT(frame->data[index], frame->data[index + 2]) << "rising";
}

TEST_F(AnalyzerTest, HoldsUntilDataOrSettingsChange)
{
    writeTone(440.0, 0.5, 0.5);
    engine.settings().set(SettingId::kTriggerMode, 1.0); // Normal.
    analyzer.setColumns(400);
    ASSERT_TRUE(produce());
    EXPECT_FALSE(produce()) << "no new trigger and nothing changed";

    engine.settings().set(SettingId::kChannelViews, 0b1100);
    const auto redrawn = produce();
    ASSERT_TRUE(redrawn) << "views changed: redraw the same window";
    EXPECT_EQ(redrawn->views, 0b1100);
}

TEST_F(AnalyzerTest, RollsSpansAboveOneSecond)
{
    writeTone(5.0, 3.0, 0.5);
    engine.settings().set(SettingId::kTimeSpan, 2.0);
    analyzer.setColumns(500);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->has(FrameFlag::kFreeRunning));
    EXPECT_FALSE(frame->has(FrameFlag::kTriggered));
}

// Just after the plugin starts, a rolling span has less audio than one of its columns: there is
// nothing to draw yet, which must not be taken for a window to crop.
TEST_F(AnalyzerTest, RollsFromNothingBeforeAColumnOfAudioArrives)
{
    engine.settings().set(SettingId::kTimeSpan, 2.0);
    analyzer.setColumns(500); // 192 samples a column at 48 kHz.
    writeSilence(100.0 / kSampleRate);
    if (const auto frame = produce()) {
        EXPECT_TRUE(frame->has(FrameFlag::kFreeRunning));
        EXPECT_EQ(frame->pointCount, 0U) << "no column is complete";
    }
    writeTone(5.0, 1.0, 0.5);
    const auto rolling = produce();
    ASSERT_TRUE(rolling);
    EXPECT_GT(rolling->pointCount, 0U);
}

// Free and spans above the threshold are one free-running window. Its columns sit on a grid fixed
// to sample indices: a frame later, the same columns are shown shifted, whatever the span and the
// column count (here neither divides the other), so peaks do not regroup and flicker. At 1 s in
// Free, and at 1.3 s in Normal.
TEST_F(AnalyzerTest, FreeRunningFramesShowTheSameColumnsShifted)
{
    for (const auto [span, mode] : {std::pair{1.0, protocol::TriggerMode::kFree},
                                    std::pair{1.3, protocol::TriggerMode::kNormal}}) {
        SCOPED_TRACE(span);
        setTriggerMode(mode);
        engine.settings().set(SettingId::kTimeSpan, span);
        engine.settings().set(SettingId::kChannelViews, 0b0001);
        analyzer.setColumns(1234);
        std::uint64_t state = 12345;
        const auto writeNoise = [&](std::size_t count) {
            std::vector<float> noise(count);
            for (float& sample : noise) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                sample = static_cast<float>(static_cast<double>(state >> 40U) / 16777216.0 - 0.5);
            }
            std::array<const float*, 2> channels{noise.data(), noise.data()};
            engine.captureRing().write(channels.data(), 2, static_cast<int>(count));
        };
        writeNoise(static_cast<std::size_t>(2.0 * span * kSampleRate));
        const auto first = produce();
        ASSERT_TRUE(first);
        EXPECT_TRUE(first->has(FrameFlag::kFreeRunning));
        EXPECT_FALSE(first->has(FrameFlag::kTriggered));
        ASSERT_EQ(first->mode, FrameMode::kMinMax);
        writeNoise(512); // Not a whole number of columns.
        const auto later = produce();
        ASSERT_TRUE(later);
        const std::size_t columns = first->pointCount;
        // The shift that maps the later frame's columns onto the first's, if any.
        std::optional<std::size_t> shift;
        for (std::size_t candidate = 1; candidate < 20 && !shift; ++candidate) {
            bool same = true;
            for (std::size_t column = 0; column + candidate < columns && same; ++column) {
                same = later->data[column * 2] == first->data[(column + candidate) * 2] &&
                       later->data[column * 2 + 1] == first->data[(column + candidate) * 2 + 1];
            }
            if (same) {
                shift = candidate;
            }
        }
        EXPECT_TRUE(shift) << "the columns were regrouped";
        EXPECT_NEAR(later->visibleSamples, std::round(span * kSampleRate), 0.5)
            << "exactly the span";
    }
}

// Switched from a free-running window to a triggered mode that holds the last trace until a
// trigger, the newest span is shown, untriggered, rather than the free-running frame staying on
// screen with its labels and chip, or a span from where the free-running window started, 2 s
// before. Every trigger mode; only Free still runs free.
TEST_F(AnalyzerTest, ShowsTheNewestSpanAfterAFreeRunningWindow)
{
    for (std::size_t index = 0; index < protocol::kTriggerModeCount; ++index) {
        const auto mode = static_cast<protocol::TriggerMode>(index);
        SCOPED_TRACE(protocol::kTriggerModeNames.at(index));
        Analyzer fresh(engine);
        fresh.setColumns(500);
        setTriggerMode(mode);
        engine.settings().set(SettingId::kTimeSpan, 2.0);
        // A falling ramp, so no rising edge to trigger on, and each sample says which it is.
        const auto count = static_cast<std::size_t>(2.5 * kSampleRate);
        std::vector<float> ramp(count);
        const std::uint64_t start = engine.captureRing().writeIndex();
        for (std::size_t i = 0; i < count; ++i) {
            ramp[i] = -static_cast<float>(start + i);
        }
        std::array<const float*, 2> channels{ramp.data(), ramp.data()};
        engine.captureRing().write(channels.data(), 2, static_cast<int>(count));
        const auto newest = static_cast<float>(engine.captureRing().writeIndex() - 1);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(fresh.produce(bytes));
        ASSERT_TRUE(decode(bytes).has(FrameFlag::kFreeRunning));
        engine.settings().set(SettingId::kTimeSpan, 0.02);
        ASSERT_TRUE(fresh.produce(bytes)) << "the span changed: a frame of the new span";
        const DecodedFrame frame = decode(bytes);
        EXPECT_EQ(frame.has(FrameFlag::kFreeRunning), mode == protocol::TriggerMode::kFree);
        EXPECT_FALSE(frame.has(FrameFlag::kTriggered));
        EXPECT_NEAR(frame.spanSeconds, 0.02, 1e-6);
        ASSERT_FALSE(frame.data.empty());
        EXPECT_NEAR(-std::ranges::min(frame.data), newest, 0.02 * kSampleRate) << "the newest span";
    }
}

// Zoomed in until a free-running window's view is drawn as raw samples, a frame reads only the
// samples in view, with one beyond each edge so the trace reaches past the screen's edges, not
// the whole span: the edge moves every frame, and the span is 4 s here (192,000 samples) for 750
// in view.
TEST_F(AnalyzerTest, ReadsOnlyTheViewOfAZoomedFreeRunningWindow)
{
    engine.settings().set(SettingId::kTimeSpan, 4.0);
    engine.settings().set(SettingId::kChannelViews, 0b0001);
    analyzer.setColumns(2500);
    const double viewStart = 0.5;
    const double viewEnd = viewStart + 1.0 / 256.0;
    analyzer.setView(viewStart, viewEnd);
    writeRamp(250'000);
    ASSERT_TRUE(produce());
    writeRamp(512);
    const std::uint64_t before = analyzer.work().samplesRead;
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->has(FrameFlag::kFreeRunning));
    ASSERT_EQ(frame->mode, protocol::FrameMode::kRaw);
    const double span = 4.0 * kSampleRate;
    EXPECT_LT(analyzer.work().samplesRead - before, 2 * 750U) << "the whole span was read";
    // The view's samples [from, to), reaching past each edge by a sample at most.
    const auto end = static_cast<double>(engine.captureRing().writeIndex());
    const double from = end - span + viewStart * span;
    const double to = end - span + viewEnd * span;
    EXPECT_LE(frame->data.front(), from);
    EXPECT_GE(frame->data.front(), from - 1.0);
    EXPECT_GE(frame->data.back(), to);
    EXPECT_LE(frame->data.back(), to + 1.0);
    EXPECT_NEAR(frame->visibleSamples, 750.0F, 1.0F);
}

// Frozen while zoomed into a free-running window, the view stays on the samples it showed: the
// freeze anchors on the whole span's right edge, not on the zoomed part's start plus the span
// (which was off by the part left of the view, 24000 samples here, and only the hold's end hid
// it while no audio came between the last live frame and the freeze). Live, the zoomed part ends on
// its column grid, so the two agree to within a column (48 samples).
TEST_F(AnalyzerTest, FreezesAZoomedFreeRunningWindowWhereItWas)
{
    engine.settings().set(SettingId::kTimeSpan, 2.0);
    engine.settings().set(SettingId::kChannelViews, 0b0001);
    analyzer.setColumns(500);
    writeRamp(200'000);
    analyzer.setView(0.25, 0.5);
    const auto live = produce();
    ASSERT_TRUE(live);
    ASSERT_TRUE(live->has(FrameFlag::kFreeRunning));
    writeRamp(14'400); // 0.3 s more before the freeze: the hold ends later than the view.
    analyzer.setFrozen(true);
    const auto frozen = produce();
    ASSERT_TRUE(frozen);
    ASSERT_TRUE(frozen->has(FrameFlag::kFrozen));
    constexpr float kColumn = 48.0f; // 24000 samples shown over 500 columns.
    EXPECT_NEAR(frozen->data.front(), live->data.front(), kColumn) << "the first sample shown";
    EXPECT_NEAR(frozen->data.back(), live->data.back(), kColumn) << "the last sample shown";
}

// A host that changes the sample rate while processing (the VST3 rules allow it only while
// inactive) leaves the capture sized for the old rate: at 384 kHz, 2^20 samples can be read for
// 2.4 s. A
// longer span shows what is kept, live and frozen, and says it is limited, rather than drawing a
// flat trace or no frame.
TEST_F(AnalyzerTest, ShowsWhatTheCaptureKeepsOfASpanLongerThanIt)
{
    engine.prepare(192000.0);
    engine.setSampleRate(384000.0);
    engine.settings().set(SettingId::kTimeSpan, 4.0);
    analyzer.setColumns(500);
    writeTone(5.0, 30.0, 0.5); // More than the capture holds.
    const auto live = produce();
    ASSERT_TRUE(live);
    EXPECT_TRUE(live->has(FrameFlag::kFreeRunning));
    EXPECT_TRUE(live->has(FrameFlag::kLimited));
    EXPECT_GT(*std::ranges::max_element(live->data), 0.4f) << "the tone, not a flat line";

    analyzer.setFrozen(true);
    const auto frozen = produce();
    ASSERT_TRUE(frozen) << "frozen, the display goes on";
    EXPECT_TRUE(frozen->has(FrameFlag::kLimited));
    EXPECT_GT(*std::ranges::max_element(frozen->data), 0.4f);
}

// Frozen before a span of audio arrived, the freeze holds less than the span: it shows what is
// held, limited, rather than no frame.
TEST_F(AnalyzerTest, ShowsWhatAFreezeHoldsOfALongerSpan)
{
    engine.settings().set(SettingId::kTimeSpan, 4.0);
    analyzer.setColumns(500);
    writeTone(5.0, 1.0, 0.5);
    ASSERT_TRUE(produce());
    analyzer.setFrozen(true);
    const auto frozen = produce();
    ASSERT_TRUE(frozen);
    EXPECT_TRUE(frozen->has(FrameFlag::kLimited));
    EXPECT_GT(*std::ranges::max_element(frozen->data), 0.4f);
}

// Frozen at 384 kHz, the freeze holds the whole 4 s span. The host preparing again at 48 kHz
// shrinks the capture, not what was held: the frozen picture stays whole.
TEST_F(AnalyzerTest, KeepsAFreezeWholeWhenTheHostLowersTheRate)
{
    engine.prepare(384000.0);
    engine.settings().set(SettingId::kTimeSpan, 4.0);
    analyzer.setColumns(500);
    writeTone(5.0, 40.0, 0.5); // 1.92 M samples: more than the span.
    ASSERT_TRUE(produce());
    analyzer.setFrozen(true);
    const auto frozen = produce();
    ASSERT_TRUE(frozen);
    ASSERT_FALSE(frozen->has(FrameFlag::kLimited));

    engine.prepare(48000.0);
    analyzer.setColumns(400); // Something to draw again for.
    const auto after = produce();
    ASSERT_TRUE(after);
    EXPECT_TRUE(after->has(FrameFlag::kFrozen));
    EXPECT_FALSE(after->has(FrameFlag::kLimited));
    EXPECT_NEAR(after->visibleSamples, 4.0 * 384000.0, 400.0);
}

// A host may run at a rate that is not a whole number, 44.1 kHz pulled up by 1.001 for one. A
// freeze holds a whole number of samples, a fraction short of the longest span's length, and it is
// still the whole span.
TEST_F(AnalyzerTest, DoesNotLimitAWholeFreezeAtARateThatIsNotAWholeNumber)
{
    for (const double rate : {44100.0 * 1.001, 48000.0 / 1.001}) {
        SCOPED_TRACE(rate);
        Engine pulled;
        pulled.prepare(rate);
        pulled.settings().set(SettingId::kTimeBase,
                              static_cast<double>(protocol::TimeBase::kSeconds));
        pulled.settings().set(SettingId::kTimeSpan, 4.0);
        std::vector<float> tone(static_cast<std::size_t>(5.0 * rate), 0.25f);
        std::array<const float*, 2> channels{tone.data(), tone.data()};
        pulled.captureRing().write(channels.data(), 2, static_cast<int>(tone.size()));
        Analyzer view{pulled};
        view.setColumns(500);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(view.produce(bytes));
        view.setFrozen(true);
        ASSERT_TRUE(view.produce(bytes));
        const DecodedFrame frozen = decode(bytes);
        EXPECT_TRUE(frozen.has(FrameFlag::kFrozen));
        EXPECT_FALSE(frozen.has(FrameFlag::kLimited));
    }
}

// A span the capture keeps is not limited.
TEST_F(AnalyzerTest, DoesNotLimitASpanTheCaptureKeeps)
{
    engine.settings().set(SettingId::kTimeSpan, 4.0);
    analyzer.setColumns(500);
    writeTone(5.0, 5.0, 0.5);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_FALSE(frame->has(FrameFlag::kLimited));
}

TEST_F(AnalyzerTest, FreezeKeepsShowingHeldDataWhileViewsStillChange)
{
    writeTone(440.0, 0.5, 0.5);
    analyzer.setColumns(400);
    ASSERT_TRUE(produce());
    analyzer.setFrozen(true);
    const auto frozen = produce();
    ASSERT_TRUE(frozen);
    EXPECT_TRUE(frozen->has(FrameFlag::kFrozen));

    writeSilence(0.5); // Live signal goes silent; the frozen view must not.
    EXPECT_FALSE(produce()) << "nothing changed while frozen";
    engine.settings().set(SettingId::kTimeSpan, 0.05);
    const auto rescaled = produce();
    ASSERT_TRUE(rescaled);
    EXPECT_TRUE(rescaled->has(FrameFlag::kFrozen));
    EXPECT_NEAR(rescaled->peak, 0.375f, 1e-3f) << "readouts work on held data";
    EXPECT_NEAR(rescaled->frequency, 440.0f, 1.0f);

    analyzer.setFrozen(false);
    writeSilence(0.5); // Longer than the trigger search region, so no edge remains in it.
    const auto live = produce();
    ASSERT_TRUE(live);
    EXPECT_FALSE(live->has(FrameFlag::kFrozen));
    EXPECT_NEAR(live->peak, 0.0f, 1e-6f);
}

TEST_F(AnalyzerTest, PanMovesTheFrozenViewBackInTime)
{
    writeTone(440.0, 0.25, 0.5);
    writeSilence(0.25);
    engine.settings().set(SettingId::kTriggerMode, 2.0); // Free: the newest span (silence).
    analyzer.setColumns(400);
    analyzer.setFrozen(true);
    const auto frozen = produce();
    ASSERT_TRUE(frozen);
    EXPECT_NEAR(frozen->peak, 0.0f, 1e-6f);

    analyzer.setPan(0.3); // Back into the tone.
    const auto panned = produce();
    ASSERT_TRUE(panned);
    EXPECT_NEAR(panned->peak, 0.375f, 1e-3f);
}

// 20 ms is 960 samples at 48 kHz: with more columns than that, a frame carries raw points.
constexpr std::size_t kRawColumns = 2000;
constexpr double kSpanSamples = 960.0;

// A pan of 0.1 s moves the frozen view back by 0.1 s of audio, not by some other amount.
TEST_F(AnalyzerTest, PanMovesTheFrozenViewBackByTheTimeAsked)
{
    setTriggerMode(protocol::TriggerMode::kFree);
    analyzer.setColumns(kRawColumns);
    writeRamp(48000);
    analyzer.setFrozen(true);
    const auto frozen = produce();
    ASSERT_TRUE(frozen);
    analyzer.setPan(0.1);
    const auto panned = produce();
    ASSERT_TRUE(panned);
    EXPECT_EQ(frozen->data.front() - panned->data.front(), 4800.0f);
}

// The UI clears its own pan when the display goes live but sends no pan of 0, so the engine must
// drop the pan itself: the next freeze opens on the newest audio, not 0.1 s before it.
TEST_F(AnalyzerTest, RefreezingAfterAPanShowsTheNewestAudio)
{
    setTriggerMode(protocol::TriggerMode::kFree);
    analyzer.setColumns(kRawColumns);
    writeRamp(48000);
    analyzer.setFrozen(true);
    ASSERT_TRUE(produce());
    analyzer.setPan(0.1);
    ASSERT_TRUE(produce());

    analyzer.setFrozen(false);
    writeRamp(4800);
    ASSERT_TRUE(produce());
    analyzer.setFrozen(true);
    const auto refrozen = produce();
    ASSERT_TRUE(refrozen);
    EXPECT_EQ(refrozen->data.back(), 52799.0f) << "the newest sample before the freeze";
}

// Frozen before any live frame, the view ends at the newest sample held and can pan back to the
// oldest: the reach is the held audio before the view, so the UI can pan to its first sample.
TEST_F(AnalyzerTest, PansBackToTheOldestAudioHeld)
{
    setTriggerMode(protocol::TriggerMode::kFree);
    analyzer.setColumns(kRawColumns);
    writeRamp(240000); // 5 s: more than the 4 s a freeze holds.
    analyzer.setFrozen(true);
    const float reach = producePanReach();
    EXPECT_NEAR(reach, protocol::kFrozenHoldSeconds - kSpanSamples / kSampleRate, 1e-4);
    analyzer.setPan(reach);
    const auto oldest = produce();
    ASSERT_TRUE(oldest);
    EXPECT_NEAR(oldest->data.front(), 240000.0 - protocol::kFrozenHoldSeconds * kSampleRate, 1.0)
        << "the first sample held";
}

// Normal mode with nothing to trigger on keeps the last triggered trace, and when the views
// change it redraws that same trace, not an untriggered window of whatever is there now.
TEST_F(AnalyzerTest, RedrawsTheLastTraceWhileWaitingForATrigger)
{
    setTriggerMode(protocol::TriggerMode::kNormal);
    analyzer.setColumns(400);
    writeSilence(0.5);
    writeTone(440.0, 0.5, 0.5);
    const auto triggered = produce();
    ASSERT_TRUE(triggered);
    ASSERT_TRUE(triggered->has(FrameFlag::kTriggered));

    writeSilence(0.5); // Longer than the trigger search region: no edge is left in it.
    EXPECT_FALSE(produce()) << "nothing new to show";
    engine.settings().set(SettingId::kChannelViews, 0b0101); // L and M.
    const auto redrawn = produce();
    ASSERT_TRUE(redrawn);
    EXPECT_TRUE(redrawn->has(FrameFlag::kTriggered));
    EXPECT_NEAR(redrawn->peak, 0.375f, 1e-3f) << "the tone, as last shown";
}

// The hold-off setting is in seconds: at 50 ms, a 440 Hz tone (an edge every 2.3 ms) triggers
// about once every 50 ms, so half a second of frames every 10 ms shows about ten new traces.
TEST_F(AnalyzerTest, TriggersNoMoreOftenThanTheHoldOff)
{
    setTriggerMode(protocol::TriggerMode::kNormal);
    engine.settings().set(SettingId::kTriggerHoldOff, 0.05);
    analyzer.setColumns(400);
    writeTone(440.0, 0.5, 0.5);
    ASSERT_TRUE(produce());
    int triggers = 0;
    for (int step = 0; step < 50; ++step) {
        writeTone(440.0, 0.01, 0.5);
        triggers += produce().has_value() ? 1 : 0;
    }
    EXPECT_GE(triggers, 8);
    EXPECT_LE(triggers, 11);
}

// Auto triggers on every edge it finds, and only shows an untriggered trace after the timeout
// without one: a 40 Hz tone has an edge every 25 ms, so every frame of it is triggered, even
// those made between two edges.
TEST_F(AnalyzerTest, AutoKeepsTriggeringOnALowTone)
{
    analyzer.setColumns(400);
    writeTone(40.0, 0.5, 0.5);
    int frames = 0;
    for (int step = 0; step < 50; ++step) {
        writeTone(40.0, 0.01, 0.5);
        if (const auto frame = produce()) {
            ++frames;
            EXPECT_TRUE(frame->has(FrameFlag::kTriggered)) << "after " << step * 10 << " ms";
        }
    }
    EXPECT_GT(frames, 10);
}

// Single mode captures once per Arm: making the display live again with Freeze does not capture
// again until Arm is pressed. Each capture here is of a quieter tone, so its peak says which.
TEST_F(AnalyzerTest, SingleCapturesAgainOnlyWhenArmed)
{
    setTriggerMode(protocol::TriggerMode::kSingle);
    analyzer.setColumns(400);
    writeTone(440.0, 0.5, 0.5);
    ASSERT_TRUE(produce());
    analyzer.arm();
    analyzer.setFrozen(false);
    writeTone(440.0, 0.2, 0.25);
    const auto rearmed = produce();
    ASSERT_TRUE(rearmed);
    EXPECT_NEAR(rearmed->peak, 0.1875f, 1e-3f) << "armed: captured again (Mid of 0.25 and 0.125)";

    analyzer.setFrozen(false);
    int frames = 0;
    for (int step = 0; step < 5; ++step) {
        writeTone(440.0, 0.1, 0.125);
        if (const auto frame = produce()) {
            ++frames;
            EXPECT_NEAR(frame->peak, 0.1875f, 1e-3f) << "not armed: still the last capture";
        }
    }
    EXPECT_GT(frames, 0) << "live again, it shows the last capture";
}

// A Single capture holds as much audio as a freeze does, so the captured trace can be panned
// back past its own span: here 0.65 s back, into the silence before the tone.
TEST_F(AnalyzerTest, SingleCaptureHoldsEnoughToPanBack)
{
    setTriggerMode(protocol::TriggerMode::kSingle);
    analyzer.setColumns(400);
    writeSilence(1.0);
    writeTone(440.0, 0.5, 0.5);
    const auto captured = produce();
    ASSERT_TRUE(captured);
    ASSERT_TRUE(captured->has(FrameFlag::kTriggered));
    analyzer.setPan(0.6);
    EXPECT_GT(producePanReach(), 1.4f);
    analyzer.setPan(0.65);
    const auto panned = produce();
    ASSERT_TRUE(panned);
    EXPECT_TRUE(panned->has(FrameFlag::kFrozen));
    EXPECT_NEAR(panned->peak, 0.0f, 1e-6f) << "the silence before the tone";
}

TEST_F(AnalyzerTest, SingleFreezesOnItsTriggerUntilRearmed)
{
    writeTone(440.0, 0.5, 0.5);
    engine.settings().set(SettingId::kTriggerMode, 3.0); // Single.
    analyzer.setColumns(400);
    const auto captured = produce();
    ASSERT_TRUE(captured);
    EXPECT_TRUE(captured->has(FrameFlag::kTriggered));

    writeTone(440.0, 0.1, 0.5);
    const auto held = produce();
    EXPECT_FALSE(held) << "frozen after the single capture";

    analyzer.arm();
    analyzer.setFrozen(false);
    writeTone(440.0, 0.1, 0.5);
    const auto rearmed = produce();
    ASSERT_TRUE(rearmed);
    EXPECT_TRUE(rearmed->has(FrameFlag::kTriggered));
}

TEST_F(AnalyzerTest, MonoInputProducesASingleView)
{
    engine.setInputChannels(1);
    writeTone(440.0, 0.5, 0.5);
    engine.settings().set(SettingId::kChannelViews, 0b1111);
    analyzer.setColumns(400);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->views, 0b0001);
    EXPECT_NEAR(frame->peak, 0.5f, 1e-3f) << "the trigger source is the mono channel itself";
}

// View zoom: the left channel holds each sample's own index, so a zoomed frame's
// contents can be checked exactly.
class ZoomTest : public AnalyzerTest
{
protected:
    void SetUp() override
    {
        AnalyzerTest::SetUp();
        engine.settings().set(SettingId::kTriggerMode, 2.0); // Free: the newest span.
        engine.settings().set(SettingId::kChannelViews, 0b0001);
    }
};

TEST_F(ZoomTest, RendersOnlyTheVisiblePartAsRawSamples)
{
    writeRamp(48000);
    analyzer.setColumns(8000);
    analyzer.setView(0.25, 0.5); // 20 ms span = 960 samples; samples 240 to 480 of the window.
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->mode, FrameMode::kRaw);
    const float windowStart = 48000.0f - 960.0f;
    EXPECT_EQ(frame->data.front(), windowStart + 240.0f);
    EXPECT_EQ(frame->data.back(), windowStart + 480.0f) << "one point past the right edge";
    EXPECT_EQ(frame->viewStart, 0.25f);
    EXPECT_EQ(frame->viewEnd, 0.5f);
    EXPECT_EQ(frame->visibleSamples, 240.0f);
    EXPECT_EQ(frame->startFraction, 0.0f);
}

TEST_F(ZoomTest, ReducesTheVisiblePartAtFullResolution)
{
    writeRamp(48000);
    engine.settings().set(SettingId::kTimeSpan, 0.1); // 4800 samples over 400 columns: 12 each.
    analyzer.setColumns(400);
    analyzer.setView(0.0, 0.1); // 480 visible samples: 1.2 per column.
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->mode, FrameMode::kMinMax);
    ASSERT_EQ(frame->pointCount, 400u);
    EXPECT_EQ(frame->data[0], 48000.0f - 4800.0f) << "first column starts at the window start";
    EXPECT_LE(frame->data[1] - frame->data[0], 1.0f) << "a column holds one or two samples";
    EXPECT_EQ(frame->data[2 * 399 + 1], 48000.0f - 4800.0f + 480.0f - 1.0f)
        << "the last column ends with the visible part's last sample";
}

TEST_F(ZoomTest, KeepsReadoutsOverTheWholeSpan)
{
    writeTone(440.0, 0.5, 0.5);
    engine.settings().set(SettingId::kChannelViews, 0b0011);
    analyzer.setColumns(400);
    const auto whole = produce();
    analyzer.setView(0.0, 0.05);
    writeTone(440.0, 0.01, 0.5);
    const auto zoomed = produce();
    ASSERT_TRUE(whole && zoomed);
    EXPECT_NEAR(zoomed->rms, whole->rms, 0.01f);
    EXPECT_NEAR(zoomed->frequency, 440.0f, 1.0f);
}

TEST_F(ZoomTest, RedrawsWhenOnlyTheViewChanges)
{
    writeRamp(48000);
    engine.settings().set(SettingId::kTriggerMode, 1.0); // Normal: holds without new edges.
    analyzer.setColumns(8000);
    EXPECT_FALSE(produce()) << "a ramp never crosses the trigger level";
    engine.settings().set(SettingId::kTriggerMode, 2.0);
    ASSERT_TRUE(produce());
    analyzer.setView(0.5, 1.0);
    const auto frame = produce();
    ASSERT_TRUE(frame) << "a new view is drawn without waiting for new audio";
    EXPECT_EQ(frame->viewStart, 0.5f);
}

TEST_F(ZoomTest, KeepsRollColumnsOnFixedBoundaries)
{
    writeRamp(200000);
    engine.settings().set(SettingId::kTimeSpan, 2.0); // 96000 samples.
    analyzer.setColumns(500);
    analyzer.setView(0.5, 1.0); // The newest 48000 samples: 96 per column.
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->has(FrameFlag::kFreeRunning));
    const float firstColumnMinimum = frame->data[0];
    EXPECT_EQ(static_cast<std::uint64_t>(firstColumnMinimum) % 96, 0u);
    EXPECT_EQ(frame->data[2] - firstColumnMinimum, 96.0f) << "each column holds 96 samples";
}

TEST_F(ZoomTest, IgnoresInvalidViews)
{
    writeRamp(48000);
    analyzer.setColumns(8000);
    analyzer.setView(0.6, 0.4);
    analyzer.setView(-0.1, 0.5);
    analyzer.setView(0.5, 1.5);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->viewStart, 0.0f);
    EXPECT_EQ(frame->viewEnd, 1.0f);
}

// Tempo sync: audio and the host clock advance together at 120 BPM, 4/4.
class SweepTest : public AnalyzerTest
{
protected:
    void SetUp() override
    {
        AnalyzerTest::SetUp();
        engine.settings().set(SettingId::kTimeBase, 1.0); // Beats.
        engine.settings().set(
            SettingId::kBeatSpan,
            static_cast<double>(protocol::BeatSpan::kQuarterBeat)); // 6000 samples.
        engine.settings().set(SettingId::kChannelViews, 0b0001);
        analyzer.setColumns(8000); // More columns than samples: raw points, exact values.
    }

    // Writes `count` samples whose left channel holds their own sample index.
    // `offset` moves the host position, in quarter notes, as a relocation of the transport does.
    void play(std::uint64_t count, bool playing = true, double offset = 0.0)
    {
        constexpr std::uint64_t kBlock = 480;
        for (std::uint64_t done = 0; done < count; done += kBlock) {
            const std::uint64_t start = engine.captureRing().writeIndex();
            std::vector<float> ramp(kBlock);
            for (std::uint64_t i = 0; i < kBlock; ++i) {
                ramp[i] = static_cast<float>(start + i);
            }
            std::array<const float*, 2> channels{ramp.data(), ramp.data()};
            engine.captureRing().write(channels.data(), 2, static_cast<int>(kBlock));
            engine.timeline().advance(
                {.playing = playing,
                 .positionValid = true,
                 .quarterNotes = static_cast<double>(start) * tempo / (60.0 * kSampleRate) + offset,
                 .tempoValid = true,
                 .tempo = tempo,
                 .timeSignatureValid = true,
                 .numerator = numerator,
                 .denominator = denominator},
                static_cast<int>(kBlock), kSampleRate);
        }
    }

    // Plays 0.1 s into the second bar of `barSamples` and checks that the one-bar sweep shows it.
    void expectBarSweep(std::uint64_t barSamples)
    {
        engine.settings().set(SettingId::kBeatSpan,
                              static_cast<double>(protocol::BeatSpan::kOneBar));
        analyzer.setColumns(96000); // Raw points.
        play(barSamples + 4800);
        const auto frame = produce();
        ASSERT_TRUE(frame);
        EXPECT_FLOAT_EQ(frame->visibleSamples, static_cast<float>(barSamples));
        EXPECT_NEAR(frame->cursor, 4800.0 / static_cast<double>(barSamples), 1e-6);
        EXPECT_EQ(frame->data.front(), static_cast<float>(barSamples))
            << "the second bar starts the sweep";
    }

    double tempo = 120.0;
    int numerator = 4;
    int denominator = 4;
};

// At 20 BPM four bars are 48 s: more than the capture holds. The sweep starts from what the
// capture holds and fills in over its first pass, and the frame says so meanwhile.
TEST_F(SweepTest, FillsASweepLongerThanTheCaptureAsItPlays)
{
    tempo = 20.0;
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kFourBars));
    analyzer.setColumns(1000); // 2304 samples per column.
    play(1200000);             // 25 s: the capture holds the newest 21.8 s.
    const auto filling = produce();
    ASSERT_TRUE(filling);
    EXPECT_TRUE(filling->has(FrameFlag::kPartial));
    EXPECT_FALSE(filling->has(FrameFlag::kLimited));
    ASSERT_EQ(filling->mode, FrameMode::kMinMax);
    ASSERT_EQ(filling->pointCount, 1000u);
    EXPECT_NEAR(filling->cursor, 1200000.0f / 2304000.0f, 1e-4f);
    EXPECT_EQ(filling->data[0], 0.0f) << "not captured: silent";
    EXPECT_EQ(filling->data[1], 0.0f);
    EXPECT_EQ(filling->data[200], 230400.0f) << "column 100 holds what the capture held";
    EXPECT_EQ(filling->data[2 * 520 + 1], 1199999.0f) << "the newest sample, left of the cursor";

    std::optional<DecodedFrame> filled;
    for (int chunk = 0; chunk < 50; ++chunk) { // 50 s with frames: more than a whole pass.
        play(48000);
        filled = produce();
    }
    ASSERT_TRUE(filled);
    EXPECT_FALSE(filled->has(FrameFlag::kPartial));
    EXPECT_EQ(filled->data[0], 2304000.0f) << "the second cycle's first sample";
}

// Without frames (the page hidden), the worker still places the audio as it arrives, so a sweep
// longer than the capture misses nothing.
TEST_F(SweepTest, AdvancesTheSweepBetweenFrames)
{
    tempo = 20.0;
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kFourBars));
    analyzer.setColumns(1000);
    for (int chunk = 0; chunk < 60; ++chunk) { // 60 s: more than a pass.
        play(48000);
        analyzer.advance();
    }
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_FALSE(frame->has(FrameFlag::kPartial)) << "every sample was placed as it arrived";
    EXPECT_EQ(frame->data[0], 2304000.0f);
}

// A bar span sweeps one of the host's bars: in 6/8 three quarter notes, in 7/8 three and a half,
// so the sweep restarts on the host's downbeats rather than every four quarter notes.
TEST_F(SweepTest, SweepsTheHostsBarInSixEight)
{
    numerator = 6;
    denominator = 8;
    expectBarSweep(72000); // Three quarter notes at 2 per second.
}

TEST_F(SweepTest, SweepsTheHostsBarInSevenEight)
{
    numerator = 7;
    denominator = 8;
    expectBarSweep(84000);
}

TEST_F(SweepTest, CropsAShorterSpanFromAFrozenSweepAndRebuildsALongerOne)
{
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBar));
    analyzer.setColumns(96000); // Raw points.
    play(120000);               // 1.25 bars: bar 2 began at sample 96000.
    ASSERT_TRUE(produce());
    analyzer.setFrozen(true);
    ASSERT_TRUE(produce());
    play(48000); // After the freeze: must not show.

    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kTwoBeats));
    const auto cropped = produce();
    ASSERT_TRUE(cropped);
    EXPECT_TRUE(cropped->has(FrameFlag::kFrozen));
    EXPECT_FALSE(cropped->has(FrameFlag::kLimited));
    ASSERT_EQ(cropped->pointCount, 48001u) << "two beats cropped from the bar held, plus an edge";
    EXPECT_EQ(cropped->data.front(), 96000.0f) << "the cycle of quarter notes 4 to 6";
    EXPECT_EQ(cropped->data[23999], 119999.0f) << "the newest sample before the freeze";
    EXPECT_EQ(cropped->data[24000], 24000.0f) << "the previous cycle, as drawn in the bar";
    EXPECT_NEAR(cropped->cursor, 0.5f, 1e-6f);

    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kTwoBars));
    analyzer.setColumns(192000);
    const auto longer = produce();
    ASSERT_TRUE(longer);
    ASSERT_EQ(longer->pointCount, 192000u) << "two bars, from the audio held at the freeze";
    EXPECT_EQ(longer->data.front(), 0.0f);
    EXPECT_EQ(longer->data[119999], 119999.0f);
    EXPECT_EQ(longer->data[120000], 0.0f) << "not captured before the freeze";
    EXPECT_TRUE(longer->has(FrameFlag::kPartial));
    EXPECT_NEAR(longer->cursor, 0.625f, 1e-6f);
}

TEST_F(SweepTest, StartsTheScreenAtTheCycleStart)
{
    play(9600); // 0.4 beats: the quarter-beat cycle started at sample 6000.
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->has(FrameFlag::kSynced));
    EXPECT_FALSE(frame->has(FrameFlag::kTriggered));
    ASSERT_EQ(frame->pointCount, 6000u);
    EXPECT_EQ(frame->data.front(), 6000.0f) << "cycle start at the left edge";
    EXPECT_EQ(frame->data[3599], 9599.0f) << "newest sample just left of the cursor";
    EXPECT_EQ(frame->data[3600], 3600.0f) << "previous cycle continues right of the cursor";
    EXPECT_EQ(frame->data.back(), 5999.0f);
    EXPECT_NEAR(frame->cursor, 0.6f, 1e-6f);
}

TEST_F(SweepTest, KeepsSweepingWithTheTransportStopped)
{
    play(9600);
    ASSERT_TRUE(produce());
    play(1440, false); // Three blocks.
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_NEAR(frame->cursor, 0.84f, 1e-6f) << "the cursor advanced while stopped";
}

// Stopping and playing again makes Live restart the clock from the play position. What was
// already drawn stays where it was; only new audio follows the new position.
TEST_F(SweepTest, KeepsEarlierDrawingWhenTheTransportJumps)
{
    play(9600); // Cursor at 3600: this cycle's 6000 to 9599, then the previous one's 3600 to 5999.
    ASSERT_TRUE(produce());
    play(480, true, 0.6); // Sample 9600 is now at 1 quarter note: a new cycle starts there.
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->pointCount, 6000u);
    EXPECT_EQ(frame->data[0], 9600.0f) << "the new cycle at the left edge";
    EXPECT_EQ(frame->data[479], 10079.0f);
    EXPECT_EQ(frame->data[480], 6480.0f) << "earlier drawing did not shift";
    EXPECT_EQ(frame->data[3599], 9599.0f);
    EXPECT_EQ(frame->data[3600], 3600.0f);
    EXPECT_NEAR(frame->cursor, 0.08f, 1e-6f);
}

// Freezing at 1 bar, then choosing a longer span, still renders the held audio.
TEST_F(SweepTest, ShowsALongerSpanChosenWhileFrozen)
{
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBar));
    play(500160); // More than four bars (384000 samples at 120 BPM).
    ASSERT_TRUE(produce());
    analyzer.setFrozen(true);
    ASSERT_TRUE(produce());
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kFourBars));
    const auto frame = produce();
    ASSERT_TRUE(frame) << "the frozen sweep must not go blank";
    EXPECT_EQ(frame->visibleSamples, 384000.0f);
    EXPECT_FALSE(frame->has(FrameFlag::kLimited));
}

// When less audio was held than a newly chosen span needs, the longest span that fits is shown.
TEST_F(SweepTest, ShowsALongerSpanChosenWhileFrozenAsFarAsHeld)
{
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBar));
    play(200160); // About two bars in total.
    ASSERT_TRUE(produce());
    analyzer.setFrozen(true);
    ASSERT_TRUE(produce());
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kFourBars));
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_FALSE(frame->has(FrameFlag::kLimited));
    EXPECT_TRUE(frame->has(FrameFlag::kPartial)) << "the held two bars fill half of the four";
    EXPECT_EQ(frame->visibleSamples, 384000.0f) << "four bars";
}

TEST_F(SweepTest, ZoomsIntoTheSweep)
{
    play(9600);
    analyzer.setView(0.0, 0.5); // The first half of the cycle, from its start.
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->data.front(), 6000.0f) << "still the cycle start at the left edge";
    EXPECT_EQ(frame->visibleSamples, 3000.0f);
    EXPECT_NEAR(frame->cursor, 0.6f, 1e-6f) << "the cursor stays a fraction of the whole cycle";
}

// Auto-freeze follows the transport. A frozen sweep sends no new frames by itself, so each check
// changes the width to force one.
class AutoFreezeTest : public SweepTest
{
protected:
    void SetUp() override
    {
        SweepTest::SetUp();
        engine.settings().set(SettingId::kAutoFreeze, static_cast<double>(protocol::Toggle::kOn));
    }

    bool frozen()
    {
        columns = columns == 8000 ? 7999 : 8000;
        analyzer.setColumns(columns);
        const auto frame = produce();
        EXPECT_TRUE(frame);
        return frame && frame->has(FrameFlag::kFrozen);
    }

    std::size_t columns = 8000;
};

TEST_F(AutoFreezeTest, FreezesOnStopAndResumesOnPlay)
{
    play(9600);
    EXPECT_FALSE(frozen());
    play(480, false);
    EXPECT_TRUE(frozen()) << "the stop froze the display";
    play(1440, false);
    EXPECT_TRUE(frozen()) << "still frozen while stopped";
    play(480);
    EXPECT_FALSE(frozen()) << "playing again resumed it";
    play(480);
    EXPECT_FALSE(frozen()) << "and it stays live";
}

TEST_F(AutoFreezeTest, KeepsAFreezeByTheUser)
{
    play(9600);
    EXPECT_FALSE(frozen());
    analyzer.setFrozen(true);
    EXPECT_TRUE(frozen());
    play(480, false);
    EXPECT_TRUE(frozen());
    play(480);
    EXPECT_TRUE(frozen()) << "playing does not end a freeze the user made";
}

// Auto-freeze, then the user resumes and freezes again while stopped: that freeze is the user's,
// so playing does not end it.
TEST_F(AutoFreezeTest, KeepsAFreezeByTheUserAfterAnAutoFreeze)
{
    play(9600);
    EXPECT_FALSE(frozen());
    play(480, false);
    EXPECT_TRUE(frozen()) << "the stop froze the display";
    analyzer.setFrozen(false);
    EXPECT_FALSE(frozen());
    analyzer.setFrozen(true);
    play(480, false);
    EXPECT_TRUE(frozen());
    play(480);
    EXPECT_TRUE(frozen()) << "playing does not end a freeze the user made";
}

TEST_F(AutoFreezeTest, LeavesTheDisplayAloneWhenOff)
{
    engine.settings().set(SettingId::kAutoFreeze, static_cast<double>(protocol::Toggle::kOff));
    play(9600);
    EXPECT_FALSE(frozen());
    play(480, false);
    EXPECT_FALSE(frozen());
}

TEST_F(AutoFreezeTest, StaysLiveWhenResumedByTheUserWhileStopped)
{
    play(9600);
    EXPECT_FALSE(frozen());
    play(480, false);
    EXPECT_TRUE(frozen());
    analyzer.setFrozen(false);
    play(480, false);
    EXPECT_FALSE(frozen()) << "no new stop, no new freeze";
}

TEST_F(SweepTest, ProducesNothingNewWithoutNewAudio)
{
    play(9600);
    ASSERT_TRUE(produce());
    EXPECT_FALSE(produce());
}

TEST_F(SweepTest, FreezesTheWholeSweep)
{
    play(9600);
    ASSERT_TRUE(produce());
    analyzer.setFrozen(true);
    const auto frozen = produce();
    ASSERT_TRUE(frozen);
    EXPECT_TRUE(frozen->has(FrameFlag::kFrozen));
    play(48000);
    engine.settings().set(SettingId::kChannelViews, 0b0011);
    const auto redrawn = produce();
    ASSERT_TRUE(redrawn);
    EXPECT_EQ(redrawn->data.front(), 6000.0f) << "still the sweep held at freeze time";
    EXPECT_NEAR(redrawn->cursor, 0.6f, 1e-6f);
}

// The multi-track view: several engines on one host clock, at 120 BPM in 4/4, with a quarter-
// beat sweep (6000 samples). Each engine's lane is its Mid.
class MultiTrackTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        own = join(1.0f);
        own->settings().set(SettingId::kDisplayMode,
                            static_cast<double>(protocol::DisplayMode::kMulti));
        own->settings().set(SettingId::kBeatSpan,
                            static_cast<double>(protocol::BeatSpan::kQuarterBeat));
        analyzer = std::make_unique<Analyzer>(*own, registry);
        analyzer->setColumns(8000); // More columns than samples: raw points, exact values.
    }

    // A track's audio, as a function of the host time in samples.
    using Signal = std::function<float(std::uint64_t)>;
    static float hostTime(std::uint64_t sample) { return static_cast<float>(sample); }

    // A new instance whose audio is `value` everywhere, or the host time when `value` is unset.
    std::shared_ptr<Engine> join(std::optional<float> value = std::nullopt,
                                 protocol::Group group = protocol::Group::kA)
    {
        return joinWith(value ? Signal([level = *value](std::uint64_t) { return level; })
                              : Signal(hostTime),
                        group);
    }

    std::shared_ptr<Engine> joinWith(Signal signal, protocol::Group group = protocol::Group::kA)
    {
        auto engine = std::make_shared<Engine>();
        engine->prepare(kSampleRate);
        engine->settings().set(SettingId::kGroup, static_cast<double>(group));
        registry.add(engine);
        tracks.emplace_back(engine, std::move(signal));
        return engine;
    }

    // Advances the host by `count` samples; every engine records its block on the same clock.
    void play(std::uint64_t count)
    {
        constexpr std::uint64_t kBlock = 480;
        for (std::uint64_t done = 0; done < count; done += kBlock) {
            for (const auto& [engine, signal] : tracks) {
                std::vector<float> block(kBlock);
                for (std::uint64_t i = 0; i < kBlock; ++i) {
                    block[i] = signal(hostSample + i);
                }
                std::array<const float*, 2> channels{block.data(), block.data()};
                engine->captureRing().write(channels.data(), 2, static_cast<int>(kBlock));
                engine->timeline().advance({.playing = true,
                                            .positionValid = true,
                                            .quarterNotes = quarterNotes,
                                            .tempoValid = true,
                                            .tempo = tempo},
                                           static_cast<int>(kBlock), kSampleRate);
            }
            hostSample += kBlock;
            quarterNotes += static_cast<double>(kBlock) * tempo / (60.0 * kSampleRate);
        }
    }

    std::optional<DecodedFrame> produce()
    {
        if (!analyzer->produce(analyzerBytes)) {
            return std::nullopt;
        }
        return decode(analyzerBytes);
    }

    static std::vector<float> lane(const DecodedFrame& frame, std::size_t index)
    {
        const auto begin =
            frame.data.begin() + static_cast<std::ptrdiff_t>(index * frame.pointCount);
        return {begin, begin + static_cast<std::ptrdiff_t>(frame.pointCount)};
    }

    InstanceRegistry registry;
    std::vector<std::pair<std::shared_ptr<Engine>, Signal>> tracks;
    std::shared_ptr<Engine> own;
    std::unique_ptr<Analyzer> analyzer;
    std::vector<std::byte> analyzerBytes; // The last frame, for header fields the decoder skips.
    std::uint64_t hostSample = 0;
    double tempo = 120.0;
    double quarterNotes = 0.0;
};

TEST_F(MultiTrackTest, DrawsALaneThatJoinsAfterASmallTempoChange)
{
    play(9600);
    ASSERT_TRUE(produce());
    // 0.05% faster: the own sweep keeps its length; a lane joining now would measure a slightly
    // different one.
    tempo = 120.06;
    join(2.0f);
    play(9600);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 2);
    const std::vector<float> joined = lane(*frame, 1);
    EXPECT_GT(std::ranges::count(joined, 2.0f), 0) << "drawn, not left silent";
}

TEST_F(MultiTrackTest, KeepsReducedLanesInStepWithAFreshReduction)
{
    joinWith([](std::uint64_t sample) { return std::sin(static_cast<float>(sample) * 0.01f); });
    analyzer->setColumns(300); // Fewer columns than samples: min/max pairs.
    std::optional<DecodedFrame> frame;
    for (int step = 0; step < 40; ++step) {
        play(480);
        if (auto produced = produce()) {
            frame = std::move(produced);
        }
    }
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->mode, FrameMode::kMinMax);
    Analyzer fresh(*own, registry);
    fresh.setColumns(300);
    std::vector<std::byte> bytes;
    ASSERT_TRUE(fresh.produce(bytes));
    EXPECT_EQ(decode(bytes).data, frame->data) << "frame by frame equals all at once";
}

// Free-running in Seconds, each lane keeps its columns on the same grid from where its part of the
// window starts, also for a track whose capture started later (an offset of 1440 samples, not a
// whole number of columns) and a span that does not divide into the columns: frame by frame, the
// lanes (and Compare's sum) equal a fresh reduction.
TEST_F(MultiTrackTest, KeepsFreeRunningLanesInStepWithAFreshReduction)
{
    for (const auto mode : {protocol::DisplayMode::kMulti, protocol::DisplayMode::kCompare}) {
        SCOPED_TRACE(protocol::kDisplayModeNames.at(static_cast<std::size_t>(mode)));
        own->settings().set(SettingId::kDisplayMode, static_cast<double>(mode));
        own->settings().set(SettingId::kTimeBase,
                            static_cast<double>(protocol::TimeBase::kSeconds));
        own->settings().set(SettingId::kTimeSpan, 2.0);
        own->settings().set(SettingId::kTriggerMode,
                            static_cast<double>(protocol::TriggerMode::kNormal));
        tracks.front().second = [](std::uint64_t sample) {
            return std::sin(static_cast<float>(sample) * 0.013f);
        };
        play(1440);
        const auto later = joinWith(
            [](std::uint64_t sample) { return std::sin(static_cast<float>(sample) * 0.0071f); });
        analyzer = std::make_unique<Analyzer>(*own, registry);
        analyzer->setColumns(1234);
        std::optional<DecodedFrame> frame;
        play(std::uint64_t{96000} * 2);
        for (int step = 0; step < 40; ++step) {
            play(480);
            if (auto produced = produce()) {
                frame = std::move(produced);
            }
        }
        ASSERT_TRUE(frame);
        ASSERT_TRUE(frame->has(FrameFlag::kFreeRunning));
        ASSERT_EQ(frame->mode, FrameMode::kMinMax);
        Analyzer fresh(*own, registry);
        fresh.setColumns(1234);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(fresh.produce(bytes));
        const DecodedFrame expected = decode(bytes);
        const std::size_t laneValues = std::size_t{frame->laneCount} * frame->pointCount * 2;
        ASSERT_EQ(expected.laneCount, frame->laneCount);
        EXPECT_EQ(
            std::vector<float>(frame->data.begin(),
                               frame->data.begin() + static_cast<std::ptrdiff_t>(laneValues)),
            std::vector<float>(expected.data.begin(),
                               expected.data.begin() + static_cast<std::ptrdiff_t>(laneValues)))
            << "frame by frame equals all at once";
        registry.remove(*later);
        tracks.pop_back();
    }
}

TEST_F(MultiTrackTest, DrawsOneMidLanePerTrackOfTheGroup)
{
    join(2.0f);
    join(9.0f, protocol::Group::kB);
    join(3.0f);
    play(9600);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->views, 0);
    ASSERT_EQ(frame->laneCount, 3) << "group B's track is not drawn";
    ASSERT_EQ(frame->pointCount, 6000u);
    EXPECT_TRUE(frame->has(FrameFlag::kSynced));
    EXPECT_EQ(lane(*frame, 0), std::vector<float>(6000, 1.0f)) << "creation order";
    EXPECT_EQ(lane(*frame, 1), std::vector<float>(6000, 2.0f));
    EXPECT_EQ(lane(*frame, 2), std::vector<float>(6000, 3.0f));

    const LaneSet lanes = analyzer->laneSet();
    EXPECT_EQ(frame->laneSet, lanes.id);
    ASSERT_EQ(lanes.lanes.size(), 3u);
    EXPECT_TRUE(lanes.lanes[0].own);
    EXPECT_FALSE(lanes.lanes[1].own);
    EXPECT_EQ(lanes.hidden, 0u);
}

TEST_F(MultiTrackTest, AlignsTracksThatStartedAtDifferentTimesOnTheClock)
{
    tracks.front().second = hostTime; // The own track records the host time too.
    play(3360);
    join(); // Its capture starts 3360 samples later, at a different index.
    play(9600);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 2);
    EXPECT_EQ(lane(*frame, 0), lane(*frame, 1)) << "the same host moment at every position";
    EXPECT_EQ(lane(*frame, 0).front(), 12000.0f) << "the cycle starts on the beat grid";
}

TEST_F(MultiTrackTest, KeepsTheOwnTrackWithinTheLaneLimit)
{
    InstanceRegistry crowded;
    for (std::size_t track = 0; track < kMaxLanes + 1; ++track) {
        auto engine = std::make_shared<Engine>();
        crowded.add(engine);
        tracks.emplace_back(
            engine, [level = static_cast<float>(track + 10)](std::uint64_t) { return level; });
    }
    crowded.add(own); // Created last: it would be the tenth lane.
    Analyzer lanes(*own, crowded);
    lanes.setColumns(8000);
    play(9600);
    std::vector<std::byte> bytes;
    ASSERT_TRUE(lanes.produce(bytes));
    const DecodedFrame frame = decode(bytes);
    ASSERT_EQ(frame.laneCount, kMaxLanes);
    EXPECT_EQ(lane(frame, kMaxLanes - 1).front(), 1.0f) << "the own track takes the last lane";
    EXPECT_EQ(lanes.laneSet().hidden, 2u);
    EXPECT_TRUE(lanes.laneSet().lanes.back().own);
}

TEST_F(MultiTrackTest, StartsANewLaneSetWhenTracksChange)
{
    play(9600);
    ASSERT_TRUE(produce());
    const std::uint32_t first = analyzer->laneSet().id;
    play(480);
    ASSERT_TRUE(produce());
    EXPECT_EQ(analyzer->laneSet().id, first) << "nothing changed";

    const auto bass = join(2.0f);
    play(480);
    ASSERT_TRUE(produce());
    const std::uint32_t joined = analyzer->laneSet().id;
    EXPECT_GT(joined, first);

    bass->setTrackInfo({.name = "Bass", .colour = 0x3060e0, .index = std::nullopt});
    play(480);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_GT(analyzer->laneSet().id, joined);
    EXPECT_EQ(frame->laneSet, analyzer->laneSet().id);
    EXPECT_EQ(analyzer->laneSet().lanes[1].name, "Bass");
    EXPECT_EQ(analyzer->laneSet().lanes[1].colour, 0x3060e0u);
}

TEST_F(MultiTrackTest, ShowsTheOwnTrackAloneWhenNotRegistered)
{
    join(2.0f);
    const InstanceRegistry empty;
    Analyzer alone(*own, empty);
    alone.setColumns(8000);
    play(9600);
    std::vector<std::byte> bytes;
    ASSERT_TRUE(alone.produce(bytes));
    EXPECT_EQ(decode(bytes).laneCount, 1);
    EXPECT_TRUE(alone.laneSet().lanes.front().own);
}

TEST_F(MultiTrackTest, DrawsChannelViewsInSingleMode)
{
    join(2.0f);
    own->settings().set(SettingId::kDisplayMode,
                        static_cast<double>(protocol::DisplayMode::kSingle));
    play(9600);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->laneCount, 0);
    EXPECT_NE(frame->views, 0);
}

TEST_F(MultiTrackTest, HoldsEveryLaneWhileFrozen)
{
    tracks.front().second = hostTime;
    join();
    play(9600);
    const auto live = produce();
    ASSERT_TRUE(live);
    analyzer->setFrozen(true);
    ASSERT_TRUE(produce());
    play(2400);
    analyzer->setColumns(7999); // Forces a frame.
    const auto frozen = produce();
    ASSERT_TRUE(frozen);
    EXPECT_TRUE(frozen->has(FrameFlag::kFrozen));
    ASSERT_EQ(frozen->laneCount, 2);
    EXPECT_EQ(lane(*frozen, 1), lane(*frozen, 0)) << "both lanes hold the same moment";
    EXPECT_EQ(frozen->cursor, live->cursor) << "the sweep stopped";
}

TEST_F(MultiTrackTest, ZoomsEveryLaneAtFullResolution)
{
    tracks.front().second = hostTime;
    join();
    play(9600);
    analyzer->setView(0.25, 0.5);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 2);
    EXPECT_EQ(frame->pointCount, 1501u) << "the visible quarter, with one point beyond an edge";
    EXPECT_EQ(lane(*frame, 0), lane(*frame, 1));
    EXPECT_EQ(lane(*frame, 0).front(), 7500.0f) << "a quarter into the cycle starting at 6000";
    EXPECT_FLOAT_EQ(frame->viewStart, 0.25f);
}

TEST_F(MultiTrackTest, CropsAShorterSpanFromFrozenLanes)
{
    tracks.front().second = hostTime;
    join();
    own->settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBeat));
    play(26880); // 1.12 beats: the beat began at sample 24000.
    ASSERT_TRUE(produce());
    analyzer->setFrozen(true);
    ASSERT_TRUE(produce());
    play(4800);
    own->settings().set(SettingId::kBeatSpan,
                        static_cast<double>(protocol::BeatSpan::kQuarterBeat));
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 2);
    ASSERT_EQ(frame->pointCount, 6001u) << "a quarter beat cropped from the beat held";
    EXPECT_EQ(lane(*frame, 0), lane(*frame, 1));
    const std::vector<float> held = lane(*frame, 0);
    EXPECT_EQ(held.front(), 24000.0f) << "the quarter-beat cycle that held the freeze";
    EXPECT_EQ(held[2879], 26879.0f) << "the newest sample before the freeze";
    EXPECT_EQ(held[2880], 2880.0f) << "the previous cycle, as drawn in the beat";
    EXPECT_NEAR(frame->cursor, 0.48f, 1e-6f);
}

// Freezing holds every lane's audio, so a longer beat span chosen while frozen shows it.
TEST_F(MultiTrackTest, ShowsALongerSpanChosenWhileFrozenInEveryLane)
{
    tracks.front().second = hostTime;
    join();
    play(30000);
    const auto frozenAt = static_cast<float>(hostSample);
    analyzer->setFrozen(true);
    ASSERT_TRUE(produce());
    play(4800); // Audio after the freeze must not appear.
    own->settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kHalfBeat));
    analyzer->setColumns(12000); // Raw points for the longer span too.
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 2);
    ASSERT_EQ(frame->pointCount, 12000u) << "half a beat";
    EXPECT_EQ(lane(*frame, 0), lane(*frame, 1)) << "both lanes from their held audio";
    const std::vector<float> held = lane(*frame, 0);
    EXPECT_EQ(held.front(), 24000.0f) << "the half-beat cycle that held the freeze";
    EXPECT_LT(*std::ranges::max_element(held), frozenAt) << "nothing after the freeze";
}

// Readouts of a tempo-synced sweep come from levels kept per block, so a long sweep is never
// scanned whole for a reading: frame by frame they equal a fresh measurement, in Multi mode (the
// own lane's Mid) and in Single mode (the trigger view).
TEST_F(MultiTrackTest, KeepsSweepReadoutsInStepWithAFreshMeasurement)
{
    constexpr float kRadiansPerSample = 0.1f;
    tracks.front().second = [](std::uint64_t sample) {
        return 0.5f * std::sin(static_cast<float>(sample) * kRadiansPerSample);
    };
    for (const auto mode : {protocol::DisplayMode::kMulti, protocol::DisplayMode::kSingle}) {
        own->settings().set(SettingId::kDisplayMode, static_cast<double>(mode));
        for (int step = 0; step < 40; ++step) {
            play(480);
            produce();
        }
        own->settings().set(SettingId::kVerticalScale, mode == protocol::DisplayMode::kMulti
                                                           ? -6.0
                                                           : -12.0); // Measures again now.
        const auto frame = produce();
        ASSERT_TRUE(frame);
        Analyzer fresh(*own, registry);
        fresh.setColumns(8000);
        std::vector<std::byte> bytes;
        ASSERT_TRUE(fresh.produce(bytes));
        const DecodedFrame expected = decode(bytes);
        EXPECT_EQ(frame->peak, expected.peak) << "mode " << static_cast<int>(mode);
        EXPECT_EQ(frame->rms, expected.rms) << "mode " << static_cast<int>(mode);
        EXPECT_NEAR(frame->peak, 0.5f, 1e-3f);
        EXPECT_NEAR(frame->rms, 0.5f / std::numbers::sqrt2_v<float>, 0.01f);
        EXPECT_NEAR(frame->frequency,
                    kRadiansPerSample * static_cast<float>(kSampleRate) /
                        (2.0f * std::numbers::pi_v<float>),
                    1.0f);
    }
}

// Seconds mode: every lane shows the own track's window at the same host moments.
class SecondsLanesTest : public MultiTrackTest
{
protected:
    void SetUp() override
    {
        MultiTrackTest::SetUp();
        own->settings().set(SettingId::kTimeBase,
                            static_cast<double>(protocol::TimeBase::kSeconds));
        own->settings().set(SettingId::kTimeSpan, 0.1); // 4800 samples.
        own->settings().set(SettingId::kTriggerMode,
                            static_cast<double>(protocol::TriggerMode::kFree));
        tracks.front().second = hostTime; // Every track records the host time.
    }
};

TEST_F(SecondsLanesTest, ShowsTheSameMomentsInEveryLane)
{
    play(3360);
    join(); // Its capture starts 3360 samples later.
    play(9600);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 2);
    EXPECT_FALSE(frame->has(FrameFlag::kSynced));
    ASSERT_GE(frame->pointCount, 4800u);
    EXPECT_EQ(lane(*frame, 1), lane(*frame, 0)) << "the same host moment at every position";
    EXPECT_EQ(lane(*frame, 0).back(), static_cast<float>(hostSample - 1))
        << "Free mode shows the newest span";
}

TEST_F(SecondsLanesTest, HoldsEveryLaneAtTheFreeze)
{
    join();
    play(9600);
    const auto frozenAt = static_cast<float>(hostSample);
    analyzer->setFrozen(true);
    ASSERT_TRUE(produce());
    play(4800);
    analyzer->setColumns(7999); // Forces a frame.
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
    ASSERT_EQ(frame->laneCount, 2);
    EXPECT_EQ(lane(*frame, 1), lane(*frame, 0));
    const std::vector<float> held = lane(*frame, 0);
    EXPECT_EQ(held.back(), frozenAt - 1.0f) << "the newest sample before the freeze";
}

// Frozen in the multi-track view, then Single mode, live and frozen again, then back: the lanes
// show the second freeze, not what they held at the first.
TEST_F(SecondsLanesTest, ShowsTheLatestFreezeAfterASingleModeInterlude)
{
    join();
    play(9600);
    analyzer->setFrozen(true);
    ASSERT_TRUE(produce());
    own->settings().set(SettingId::kDisplayMode,
                        static_cast<double>(protocol::DisplayMode::kSingle));
    ASSERT_TRUE(produce());
    analyzer->setFrozen(false);
    play(4800);
    ASSERT_TRUE(produce());
    const auto frozenAt = static_cast<float>(hostSample);
    analyzer->setFrozen(true);
    ASSERT_TRUE(produce());
    play(4800);
    own->settings().set(SettingId::kDisplayMode,
                        static_cast<double>(protocol::DisplayMode::kMulti));
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
    ASSERT_EQ(frame->laneCount, 2);
    EXPECT_EQ(lane(*frame, 0).back(), frozenAt - 1.0f) << "the newest sample before this freeze";
    EXPECT_EQ(lane(*frame, 1), lane(*frame, 0));
}

// The Compare view: the group's tracks, with the sum of the compared ones. Tracks carry
// deterministic noise of the host time, so a track can be this one's signal inverted, delayed, or
// unrelated.
float noiseAt(std::uint64_t sample, std::uint64_t seed)
{
    std::uint64_t x = sample * 0x9E3779B97F4A7C15ULL + seed * 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 30U;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27U;
    return static_cast<float>(x >> 40U) / static_cast<float>(1U << 24U) - 0.5f;
}

class CompareTest : public MultiTrackTest
{
protected:
    void SetUp() override
    {
        MultiTrackTest::SetUp();
        own->settings().set(SettingId::kDisplayMode,
                            static_cast<double>(protocol::DisplayMode::kCompare));
        tracks.front().second = [](std::uint64_t sample) { return noiseAt(sample, 1); };
    }

    std::uint64_t idOf(const std::shared_ptr<Engine>& engine) const
    {
        for (const GroupMember& member : registry.members(protocol::Group::kA)) {
            if (member.engine == engine) {
                return member.serial;
            }
        }
        return 0;
    }

    struct Match
    {
        float correlation = 0.0f;
        float offset = 0.0f;
        float polarity = 0.0f;
        [[nodiscard]] bool inverted() const { return polarity < 0.0f; }
    };

    // A track's match against this track, after the lanes (the tracks and their sum).
    static Match match(const DecodedFrame& frame, std::size_t track)
    {
        const std::size_t perPoint = frame.mode == FrameMode::kMinMax ? 2 : 1;
        const std::size_t at = std::size_t{frame.laneCount} * frame.pointCount * perPoint +
                               track * protocol::kMatchValues;
        return {.correlation = frame.data.at(at),
                .offset = frame.data.at(at + 1),
                .polarity = frame.data.at(at + 2)};
    }

    float sumGain() const { return get<float>(analyzerBytes, FrameHeader::kSumGainOffset); }

    static float loudest(const std::vector<float>& values)
    {
        float result = 0.0f;
        for (const float value : values) {
            result = std::max(result, std::abs(value));
        }
        return result;
    }
};

TEST_F(CompareTest, ComparesThisTrackWithTheFirstOtherTrackUntilAsked)
{
    const auto inverted = joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); });
    const auto other = joinWith([](std::uint64_t sample) { return noiseAt(sample, 2); });
    play(9600);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 4) << "the three tracks and the sum";
    ASSERT_EQ(frame->data.size(), std::size_t{4} * frame->pointCount + 3 * protocol::kMatchValues);
    EXPECT_TRUE(frame->has(FrameFlag::kCompared));
    EXPECT_LT(sumGain(), -60.0f) << "an inverted copy cancels";
    EXPECT_LT(loudest(lane(*frame, 3)), 1e-6f);
    EXPECT_TRUE(match(*frame, 1).inverted());
    EXPECT_NEAR(match(*frame, 1).correlation, -1.0f, 1e-4f);
    EXPECT_TRUE(std::isnan(match(*frame, 2).correlation)) << "not compared: no value";
    EXPECT_TRUE(std::isnan(match(*frame, 2).offset)) << "not compared: no value";
    EXPECT_EQ(match(*frame, 2).polarity, 0.0f) << "not compared: not related";

    const LaneSet lanes = analyzer->laneSet();
    ASSERT_EQ(lanes.lanes.size(), 3u);
    EXPECT_TRUE(lanes.lanes[0].own);
    EXPECT_TRUE(lanes.lanes[0].compared);
    EXPECT_EQ(lanes.lanes[1].id, idOf(inverted));
    EXPECT_TRUE(lanes.lanes[1].compared);
    EXPECT_EQ(lanes.lanes[2].id, idOf(other));
    EXPECT_FALSE(lanes.lanes[2].compared);
}

TEST_F(CompareTest, SumsEveryComparedTrack)
{
    const auto copy = joinWith([](std::uint64_t sample) { return noiseAt(sample, 1); });
    const auto unrelated = joinWith([](std::uint64_t sample) { return noiseAt(sample, 2); });
    joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); });
    play(9600);
    analyzer->setCompared({idOf(copy), idOf(unrelated)});
    // One match is searched per frame: a frame each.
    ASSERT_TRUE(produce());
    play(480);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 5);
    const std::vector<float> ownTrack = lane(*frame, 0);
    const std::vector<float> other = lane(*frame, 2);
    const std::vector<float> sum = lane(*frame, 4);
    for (std::size_t i = 0; i < sum.size(); ++i) {
        ASSERT_NEAR(sum[i], 2.0f * ownTrack[i] + other[i], 1e-5f)
            << "the inverted track is left out";
    }
    // Three equal levels, two of them alike: 10 log10(5 / 3).
    EXPECT_NEAR(sumGain(), 2.2f, 0.2f);
    EXPECT_NEAR(match(*frame, 1).correlation, 1.0f, 1e-4f);
    EXPECT_EQ(match(*frame, 1).polarity, 1.0f);
    EXPECT_NEAR(match(*frame, 2).correlation, 0.0f, 0.05f);
    EXPECT_EQ(match(*frame, 2).polarity, 0.0f) << "unrelated: no clear match";
    EXPECT_TRUE(std::isnan(match(*frame, 2).offset)) << "unrelated: no offset";
    EXPECT_TRUE(std::isnan(match(*frame, 3).correlation)) << "not compared: no value";
}

TEST_F(CompareTest, SumsThisTrackAloneWhenNothingElseIsChecked)
{
    joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); });
    play(9600);
    analyzer->setCompared({});
    auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 3) << "the sum lane stays";
    EXPECT_TRUE(frame->has(FrameFlag::kCompared));
    EXPECT_EQ(lane(*frame, 2), lane(*frame, 0)) << "the sum is this track";
    EXPECT_NEAR(sumGain(), 0.0f, 1e-3f);
    EXPECT_TRUE(std::isnan(match(*frame, 1).correlation)) << "not compared: no value";
    const LaneSet lanes = analyzer->laneSet();
    EXPECT_TRUE(lanes.lanes[0].compared);
    EXPECT_FALSE(lanes.lanes[1].compared);

    analyzer->setCompared({12345}); // No such track.
    play(480);
    frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->laneCount, 3);
}

// The checks are the instance's, not its editor's: closing and reopening the editor (a new
// analyzer on the same instance) keeps them.
TEST_F(CompareTest, KeepsTheCheckedTracksWhenTheEditorReopens)
{
    const auto first = joinWith([](std::uint64_t sample) { return noiseAt(sample, 2); });
    const auto second = joinWith([](std::uint64_t sample) { return noiseAt(sample, 3); });
    const auto third = joinWith([](std::uint64_t sample) { return noiseAt(sample, 4); });
    play(9600);
    ASSERT_TRUE(produce());
    analyzer->setCompared({idOf(second), idOf(third)});
    ASSERT_TRUE(produce());

    analyzer.reset();
    Analyzer reopened(*own, registry);
    reopened.setColumns(8000);
    std::vector<std::byte> bytes;
    play(480);
    ASSERT_TRUE(reopened.produce(bytes));
    const LaneSet lanes = reopened.laneSet();
    ASSERT_EQ(lanes.lanes.size(), 4u);
    EXPECT_EQ(lanes.lanes[1].id, idOf(first));
    EXPECT_FALSE(lanes.lanes[1].compared) << "the first other track, which the default compares";
    EXPECT_TRUE(lanes.lanes[2].compared);
    EXPECT_TRUE(lanes.lanes[3].compared);
}

// A checked track that leaves the group is not compared while away, and is again when it comes
// back, though checks changed meanwhile: the page only knows the tracks it draws.
TEST_F(CompareTest, KeepsATrackCheckedWhileItIsInAnotherGroup)
{
    const auto first = joinWith([](std::uint64_t sample) { return noiseAt(sample, 2); });
    const auto away = joinWith([](std::uint64_t sample) { return noiseAt(sample, 3); });
    play(9600);
    ASSERT_TRUE(produce());
    analyzer->setCompared({idOf(first), idOf(away)});
    ASSERT_TRUE(produce());

    away->settings().set(SettingId::kGroup, static_cast<double>(protocol::Group::kB));
    play(480);
    ASSERT_TRUE(produce());
    ASSERT_EQ(analyzer->laneSet().lanes.size(), 2u);
    analyzer->setCompared({}); // First unchecked while the other is away.
    play(480);
    ASSERT_TRUE(produce());
    EXPECT_FALSE(analyzer->laneSet().lanes[1].compared);

    away->settings().set(SettingId::kGroup, static_cast<double>(protocol::Group::kA));
    play(480);
    ASSERT_TRUE(produce());
    const LaneSet lanes = analyzer->laneSet();
    ASSERT_EQ(lanes.lanes.size(), 3u);
    EXPECT_FALSE(lanes.lanes[1].compared);
    EXPECT_EQ(lanes.lanes[2].id, idOf(away));
    EXPECT_TRUE(lanes.lanes[2].compared);
}

// Sixteen bars at 120 BPM are a 32 s sweep. The other track is this one inverted for the first
// 14 s, then as it is: the match follows the newest 8 s before the playhead, not the whole view.
TEST_F(CompareTest, SearchesTheMatchOverTheNewestPartOfTheView)
{
    constexpr std::uint64_t kFlipAt = std::uint64_t{14} * 48000;
    joinWith([](std::uint64_t sample) {
        return sample < kFlipAt ? -noiseAt(sample, 1) : noiseAt(sample, 1);
    });
    own->settings().set(SettingId::kBeatSpan,
                        static_cast<double>(protocol::BeatSpan::kSixteenBars));
    analyzer->setColumns(1000);
    play(std::uint64_t{24} * 48000);
    // The frames that follow with no more audio, until the match search (spread over a few
    // frames, kMatchSearchWork) has ended.
    auto frame = produce();
    for (int step = 0; step < 16; ++step) {
        if (auto next = produce()) {
            frame = std::move(next);
        }
    }
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 3);
    EXPECT_NEAR(frame->cursor, 0.75f, 1e-3f) << "the playhead at 24 of 32 s";
    EXPECT_EQ(match(*frame, 1).offset, 0.0f);
    EXPECT_NE(match(*frame, 1).polarity, 0.0f) << "related";
    EXPECT_FALSE(match(*frame, 1).inverted());
    EXPECT_NEAR(match(*frame, 1).correlation, 1.0f, 1e-3f);
}

// A change of the view makes every match due again; until each is searched again, one per
// frame, it stays on show.
TEST_F(CompareTest, KeepsMatchesOnShowWhileTheViewChanges)
{
    const auto copy = joinWith([](std::uint64_t sample) { return noiseAt(sample, 1); });
    const auto inverted = joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); });
    analyzer->setCompared({idOf(copy), idOf(inverted)});
    play(9600);
    for (int step = 0; step < 3; ++step) {
        play(480);
        ASSERT_TRUE(produce());
    }
    analyzer->setView(0.25, 0.75);
    play(480);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(match(*frame, 1).polarity, 1.0f) << "kept on show while due again";
    EXPECT_TRUE(match(*frame, 2).inverted());
}

// The summing gains come from levels kept per block of each lane and of the sums, so a long view
// is never scanned whole: frame by frame they equal a fresh measurement, also over a zoomed part
// that starts and ends inside blocks.
TEST_F(CompareTest, KeepsGainsInStepWithAFreshMeasurement)
{
    const auto copy = joinWith([](std::uint64_t sample) { return noiseAt(sample, 1); });
    const auto quiet = joinWith([](std::uint64_t sample) { return 0.5f * noiseAt(sample, 2); });
    const auto inverted = joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); });
    const std::vector<std::uint64_t> compared{idOf(copy), idOf(quiet), idOf(inverted)};
    analyzer->setCompared(compared);
    analyzer->setView(0.3, 0.6);
    // Frames start before the sweep is fully captured, and a match searched on a sweep still
    // filling is searched again half a second later: play past that, then 100 ms more, so the
    // last frame measures the gains again.
    for (int step = 0; step < 80; ++step) {
        play(480);
        produce();
    }
    // A fresh analyzer searches one match per frame: it gets a few frames, on the same audio,
    // then both measure the gains again after 100 ms more.
    Analyzer fresh(*own, registry);
    fresh.setColumns(8000);
    fresh.setCompared(compared);
    fresh.setView(0.3, 0.6);
    std::vector<std::byte> bytes;
    for (int step = 0; step < 4; ++step) {
        play(480);
        produce();
        ASSERT_TRUE(fresh.produce(bytes));
    }
    play(4800);
    ASSERT_TRUE(produce());
    ASSERT_TRUE(fresh.produce(bytes));
    const float incremental = sumGain();
    const auto incrementalFixed = get<float>(analyzerBytes, FrameHeader::kFixedGainOffset);
    EXPECT_NEAR(incremental, get<float>(bytes, FrameHeader::kSumGainOffset), 1e-4f);
    EXPECT_NEAR(incrementalFixed, get<float>(bytes, FrameHeader::kFixedGainOffset), 1e-4f);
    // The copy and the inverted copy cancel: the sum is this track plus the quiet one, against
    // four parts of 1, 1, 1/4 and 1 energy. With the fix, the inverted copy adds instead.
    EXPECT_NEAR(incremental, 10.0f * std::log10(1.25f / 3.25f), 0.3f);
    EXPECT_NEAR(incrementalFixed, 10.0f * std::log10(9.25f / 3.25f), 0.3f);
}

TEST_F(CompareTest, PreviewsTheFixForALateTrack)
{
    joinWith([](std::uint64_t sample) { return sample < 37 ? 0.0f : noiseAt(sample - 37, 1); });
    play(9600);
    auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_FLOAT_EQ(match(*frame, 1).offset, 37.0f) << "37 late";
    EXPECT_FALSE(match(*frame, 1).inverted());
    EXPECT_NEAR(get<float>(analyzerBytes, FrameHeader::kFixedGainOffset), 3.0f, 0.1f);

    analyzer->setPreviewFix(true);
    frame = produce();
    ASSERT_TRUE(frame);
    const std::vector<float> a = lane(*frame, 0);
    const std::vector<float> sum = lane(*frame, 2);
    // The sweep's seam at the playhead joins two cycles; a shifted track crosses it there.
    const auto seam = static_cast<std::ptrdiff_t>(frame->cursor * static_cast<float>(a.size()));
    for (std::size_t i = 0; i + 37 < a.size(); ++i) {
        if (std::abs(static_cast<std::ptrdiff_t>(i) - seam) <= 37) {
            continue;
        }
        ASSERT_NEAR(sum[i], 2.0f * a[i], 1e-5f) << "lined up, the track doubles this one";
    }
}

// Frozen with tracks that joined just before, whose captures hold too little to search a match
// in: each failed search is tried again later, not on every step, so a still view goes idle.
TEST_F(CompareTest, GoesIdleWhenFrozenTracksHoldTooLittleToMatch)
{
    own->settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kSeconds));
    own->settings().set(SettingId::kTimeSpan, 0.1);
    own->settings().set(SettingId::kTriggerMode, static_cast<double>(protocol::TriggerMode::kFree));
    play(9600);
    joinWith([](std::uint64_t sample) { return noiseAt(sample, 1); });
    joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); });
    play(960); // 20 ms: less than the 0.1 s span the match is searched over.
    analyzer->setCompared({idOf(own), idOf(tracks[1].first), idOf(tracks[2].first)});
    analyzer->setFrozen(true);
    for (int step = 0; step < 5; ++step) {
        (void)produce(); // The freeze, and a failed search of each track.
    }
    for (int step = 5; step < 20; ++step) {
        EXPECT_FALSE(produce()) << "nothing changes, so frames stop, at step " << step;
    }
}

TEST_F(CompareTest, RefreshesAMatchWhilePlaying)
{
    // In step until host sample 12000, inverted after.
    joinWith([](std::uint64_t sample) {
        return sample < 12000 ? noiseAt(sample, 1) : -noiseAt(sample, 1);
    });
    play(9600);
    auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(match(*frame, 1).polarity, 1.0f);
    play(12000); // The whole quarter-beat sweep is now past the change, but too soon to search.
    frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(match(*frame, 1).polarity, 1.0f) << "not searched again within 0.5 s";
    play(12480);
    frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(match(*frame, 1).polarity, -1.0f) << "searched again after 0.5 s of audio";
}

TEST_F(CompareTest, KeepsTheSumLaneInStepWithAFreshSum)
{
    joinWith([](std::uint64_t sample) { return sample < 37 ? 0.0f : noiseAt(sample - 37, 1); });
    const auto other = joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); });
    analyzer->setColumns(300); // Min/max columns, as on a real screen.
    for (const bool preview : {false, true}) {
        analyzer->setCompared({idOf(tracks[1].first), idOf(other)});
        analyzer->setPreviewFix(preview);
        // Frames start before the sweep is captured whole, and a match found then is searched
        // again half a second later: play past that. A fresh analyzer searches one match per
        // frame, so it gets a few frames, on the same audio.
        for (int step = 0; step < 70; ++step) {
            play(480);
            produce();
        }
        Analyzer fresh(*own, registry);
        fresh.setColumns(300);
        fresh.setCompared({idOf(tracks[1].first), idOf(other)});
        fresh.setPreviewFix(preview);
        std::optional<DecodedFrame> frame;
        std::vector<std::byte> bytes;
        for (int step = 0; step < 3; ++step) {
            play(480);
            frame = produce();
            ASSERT_TRUE(fresh.produce(bytes));
        }
        ASSERT_TRUE(frame);
        ASSERT_EQ(frame->mode, FrameMode::kMinMax);
        const DecodedFrame expected = decode(bytes);
        const std::size_t laneValues = std::size_t{frame->laneCount} * frame->pointCount * 2;
        ASSERT_EQ(expected.laneCount, frame->laneCount);
        EXPECT_EQ(
            std::vector<float>(frame->data.begin(),
                               frame->data.begin() + static_cast<std::ptrdiff_t>(laneValues)),
            std::vector<float>(expected.data.begin(),
                               expected.data.begin() + static_cast<std::ptrdiff_t>(laneValues)))
            << "lanes and sum, frame by frame, equal all at once; preview " << preview;
    }
}

TEST_F(CompareTest, ShowsThisTrackAloneWithoutOtherTracks)
{
    play(9600);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->laneCount, 1);
    EXPECT_FALSE(frame->has(FrameFlag::kCompared));
}

TEST_F(CompareTest, LeavesALaneForTheSum)
{
    for (std::size_t i = 0; i < kMaxLanes; ++i) {
        join(static_cast<float>(i));
    }
    play(9600);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->laneCount, kMaxLanes);
    const LaneSet lanes = analyzer->laneSet();
    EXPECT_EQ(lanes.lanes.size(), kMaxLanes - 1);
    EXPECT_EQ(lanes.hidden, 2u);
}

// Rolling in Seconds mode, the Compare frame keeps each lane's and the sum's columns and reduces
// only those that scrolled in: frame by frame they equal a fresh reduction, with and without the
// fix.
TEST_F(CompareTest, KeepsTheSecondsSumLaneInStepWithAFreshSum)
{
    own->settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kSeconds));
    own->settings().set(SettingId::kTimeSpan, 2.0); // 96000 samples: 320 per column.
    own->settings().set(SettingId::kTriggerMode, static_cast<double>(protocol::TriggerMode::kFree));
    joinWith([](std::uint64_t sample) { return sample < 37 ? 0.0f : noiseAt(sample - 37, 1); });
    const auto other = joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); });
    analyzer->setColumns(300);
    for (const bool preview : {false, true}) {
        analyzer->setCompared({idOf(tracks[1].first), idOf(other)});
        analyzer->setPreviewFix(preview);
        for (int step = 0; step < 70; ++step) {
            play(480);
            produce();
        }
        Analyzer fresh(*own, registry);
        fresh.setColumns(300);
        fresh.setCompared({idOf(tracks[1].first), idOf(other)});
        fresh.setPreviewFix(preview);
        std::optional<DecodedFrame> frame;
        std::vector<std::byte> bytes;
        for (int step = 0; step < 3; ++step) {
            play(480);
            frame = produce();
            ASSERT_TRUE(fresh.produce(bytes));
        }
        ASSERT_TRUE(frame);
        ASSERT_TRUE(frame->has(FrameFlag::kFreeRunning));
        ASSERT_EQ(frame->mode, FrameMode::kMinMax);
        const DecodedFrame expected = decode(bytes);
        const std::size_t laneValues = std::size_t{frame->laneCount} * frame->pointCount * 2;
        ASSERT_EQ(expected.laneCount, frame->laneCount);
        EXPECT_EQ(
            std::vector<float>(frame->data.begin(),
                               frame->data.begin() + static_cast<std::ptrdiff_t>(laneValues)),
            std::vector<float>(expected.data.begin(),
                               expected.data.begin() + static_cast<std::ptrdiff_t>(laneValues)))
            << "lanes and sum, frame by frame, equal all at once; preview " << preview;
        EXPECT_EQ(match(*frame, 2).polarity, -1.0f) << "the inverted track";
    }
}

// A late track's match comes out alike in both time bases, live and frozen: its offset, its
// polarity and a measured correlation (at no offset, so near 0 for noise this late), as Beats and
// Seconds pack the matches the same way.
TEST_F(CompareTest, MeasuresALateTrackAlikeInBothTimeBases)
{
    joinWith([](std::uint64_t sample) { return sample < 37 ? 0.0f : noiseAt(sample - 37, 1); });
    own->settings().set(SettingId::kTimeSpan, 0.1);
    own->settings().set(SettingId::kTriggerMode, static_cast<double>(protocol::TriggerMode::kFree));
    for (const auto timeBase : {protocol::TimeBase::kBeats, protocol::TimeBase::kSeconds}) {
        for (const bool frozen : {false, true}) {
            SCOPED_TRACE(std::string(timeBase == protocol::TimeBase::kBeats ? "Beats" : "Seconds") +
                         (frozen ? ", frozen" : ", live"));
            analyzer->setFrozen(false);
            own->settings().set(SettingId::kTimeBase, static_cast<double>(timeBase));
            play(9600);
            if (frozen) {
                ASSERT_TRUE(produce()) << "measured live before the freeze";
                analyzer->setFrozen(true);
                play(480);
            }
            const auto frame = produce();
            ASSERT_TRUE(frame);
            EXPECT_FLOAT_EQ(match(*frame, 1).offset, 37.0f) << "37 late";
            const float correlation = match(*frame, 1).correlation;
            EXPECT_TRUE(std::isfinite(correlation)) << "measured";
            EXPECT_LE(std::abs(correlation), 1.0f);
            EXPECT_EQ(match(*frame, 1).polarity, 1.0f) << "related, not inverted";
        }
    }
}

// This track, and a track not compared, have no match in either time base, live and frozen: no
// correlation or offset (NaN) and polarity 0, never a correlation or offset of 0.
TEST_F(CompareTest, SendsNoMatchForThisTrackAndTracksNotCompared)
{
    joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); }); // Compared.
    joinWith([](std::uint64_t sample) { return noiseAt(sample, 2); });  // Not compared.
    own->settings().set(SettingId::kTimeSpan, 0.1);
    own->settings().set(SettingId::kTriggerMode, static_cast<double>(protocol::TriggerMode::kFree));
    for (const auto timeBase : {protocol::TimeBase::kBeats, protocol::TimeBase::kSeconds}) {
        for (const bool frozen : {false, true}) {
            SCOPED_TRACE(std::string(timeBase == protocol::TimeBase::kBeats ? "Beats" : "Seconds") +
                         (frozen ? ", frozen" : ", live"));
            analyzer->setFrozen(false);
            own->settings().set(SettingId::kTimeBase, static_cast<double>(timeBase));
            play(9600);
            if (frozen) {
                analyzer->setFrozen(true);
                play(480);
            }
            const auto frame = produce();
            ASSERT_TRUE(frame);
            ASSERT_EQ(frame->laneCount, 4);
            EXPECT_TRUE(match(*frame, 1).inverted()) << "the compared track is matched";
            for (const std::size_t track : {std::size_t{0}, std::size_t{2}}) {
                EXPECT_TRUE(std::isnan(match(*frame, track).correlation)) << "track " << track;
                EXPECT_TRUE(std::isnan(match(*frame, track).offset)) << "track " << track;
                EXPECT_EQ(match(*frame, track).polarity, 0.0f) << "track " << track;
            }
        }
    }
}

TEST_F(CompareTest, ComparesInSecondsModeToo)
{
    own->settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kSeconds));
    own->settings().set(SettingId::kTimeSpan, 0.1);
    own->settings().set(SettingId::kTriggerMode, static_cast<double>(protocol::TriggerMode::kFree));
    play(3360);
    joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); }); // Starts later.
    play(9600);
    const auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 3);
    EXPECT_FALSE(frame->has(FrameFlag::kSynced));
    EXPECT_TRUE(match(*frame, 1).inverted());
    EXPECT_LT(sumGain(), -60.0f);
}

// A track that joins has no audio of the view yet: its first search fails, and it is searched again
// soon after its capture holds the view rather than at the next refresh, half a second later.
TEST_F(CompareTest, MeasuresATrackThatJoinsSoonAfterItsAudioArrives)
{
    own->settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kSeconds));
    own->settings().set(SettingId::kTimeSpan, 0.1);
    own->settings().set(SettingId::kTriggerMode, static_cast<double>(protocol::TriggerMode::kFree));
    play(9600);
    produce();
    joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); });
    play(960);
    auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_FALSE(match(*frame, 1).inverted()) << "its capture does not hold the view yet";
    for (int step = 0; step < 12; ++step) { // 0.12 s more.
        play(480);
        if (auto produced = produce()) {
            frame = std::move(produced);
        }
    }
    EXPECT_TRUE(match(*frame, 1).inverted()) << "searched again soon";
    EXPECT_LT(sumGain(), -60.0f) << "an inverted copy cancels";
}

TEST_F(CompareTest, MeasuresTheFixOnRealSamplesInSecondsMode)
{
    // A copy 5 ms late in a 20 ms view: fixed, it sums as identical signals do (+3 dB). At the
    // right edge its shift reaches past the newest audio; summed with that silence it read 2.7 dB.
    own->settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kSeconds));
    own->settings().set(SettingId::kTimeSpan, 0.02);
    own->settings().set(SettingId::kTriggerMode, static_cast<double>(protocol::TriggerMode::kFree));
    joinWith([](std::uint64_t sample) { return sample < 240 ? 0.0f : noiseAt(sample - 240, 1); });
    analyzer->setPreviewFix(true);
    play(9600);
    std::optional<DecodedFrame> frame;
    for (int step = 0; step < 80; ++step) {
        play(480);
        if (auto produced = produce()) {
            frame = std::move(produced);
        }
    }
    ASSERT_TRUE(frame);
    EXPECT_EQ(match(*frame, 1).offset, 240.0f);
    EXPECT_NEAR(get<float>(analyzerBytes, FrameHeader::kFixedGainOffset), 3.01f, 0.2f);
}

// In Beats mode, a late copy's fixed gain follows its shift as the delay changes, to 37 samples
// late, in step, and late again: each shift's levels are measured again (over frames, which go on
// until done), none stay pending while the copy is in step, and once frozen the frames stop.
TEST_F(CompareTest, FollowsTheFixAsTheDelayChanges)
{
    std::uint64_t delay = 37;
    joinWith([&delay](std::uint64_t sample) {
        return sample < delay ? 0.0f : noiseAt(sample - delay, 1);
    });
    const double identical = 10.0 * std::log10(2.0);
    for (const std::uint64_t step : {37U, 0U, 37U}) {
        SCOPED_TRACE(step);
        delay = step;
        std::optional<DecodedFrame> frame;
        for (int frames = 0; frames < 100; ++frames) { // A second, a frame every 10 ms.
            play(480);
            if (auto produced = produce()) {
                frame = std::move(produced);
            }
        }
        ASSERT_TRUE(frame);
        EXPECT_EQ(match(*frame, 1).offset, static_cast<float>(step));
        EXPECT_NEAR(get<float>(analyzerBytes, FrameHeader::kFixedGainOffset), identical, 0.01);
    }
    analyzer->setFrozen(true);
    int frames = 0;
    while (produce() && frames < 10) {
        ++frames;
    }
    EXPECT_LT(frames, 10) << "frames stop once frozen and measured";
}

// On a sweep longer than the audio between two matches, the shifted track's levels are measured
// again as soon as its shift changes, not as the playhead rewrites the sweep: the fixed gain
// agrees with an analyzer measuring everything afresh on the same audio. The copy plays 37 samples
// late, then in step at a quarter of the level, then late again; the levels from before the
// in-step part would otherwise stand for it. Both tracks sound in alternate 20 ms bursts, the other
// ones while in step, so that each block's level depends on the part of the song it holds.
TEST_F(CompareTest, MeasuresAShiftedTrackAgainWhenItsShiftChanges)
{
    own->settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kFourBars));
    std::uint64_t delay = 37;
    std::uint64_t phase = 0;
    const auto burst = [&phase](std::uint64_t sample) {
        return (sample / 960 + phase) % 2 == 0 ? noiseAt(sample, 1) : 0.0f;
    };
    tracks.front().second = burst;
    joinWith([&](std::uint64_t sample) { return sample < delay ? 0.0f : burst(sample - delay); });
    const std::vector<std::uint64_t> compared{idOf(tracks[1].first)};
    analyzer->setCompared(compared);
    analyzer->setPreviewFix(true);
    // A frame every 100 ms: one sweep is 8 s.
    const auto playFor = [&](double seconds) {
        std::optional<DecodedFrame> frame;
        for (int step = 0; step < static_cast<int>(seconds * 10.0); ++step) {
            play(4800);
            if (auto produced = produce()) {
                frame = std::move(produced);
            }
        }
        return frame;
    };
    auto frame = playFor(10.0);
    ASSERT_TRUE(frame);
    ASSERT_EQ(match(*frame, 1).offset, 37.0f);
    delay = 0;
    phase = 1;
    frame = playFor(6.0);
    ASSERT_TRUE(frame);
    ASSERT_EQ(match(*frame, 1).offset, 0.0f) << "the in-step part leads the sweep";
    delay = 37;
    phase = 0;
    frame = playFor(6.0);
    ASSERT_TRUE(frame);
    ASSERT_EQ(match(*frame, 1).offset, 37.0f) << "late again over most of the sweep";

    // A fresh analyzer searches one match per frame: it gets a few frames, on the same audio,
    // then both measure the gains again after 100 ms more.
    Analyzer fresh(*own, registry);
    fresh.setColumns(8000);
    fresh.setCompared(compared);
    fresh.setPreviewFix(true);
    std::vector<std::byte> bytes;
    for (int step = 0; step < 4; ++step) {
        play(480);
        produce();
        ASSERT_TRUE(fresh.produce(bytes));
    }
    play(4800);
    ASSERT_TRUE(produce());
    ASSERT_TRUE(fresh.produce(bytes));
    ASSERT_EQ(match(decode(bytes), 1).offset, 37.0f);
    EXPECT_NEAR(get<float>(analyzerBytes, FrameHeader::kFixedGainOffset),
                get<float>(bytes, FrameHeader::kFixedGainOffset), 1e-3f);
}

TEST_F(CompareTest, RedrawsAFrozenSecondsFrameWhenTheComparisonChanges)
{
    own->settings().set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kSeconds));
    own->settings().set(SettingId::kTimeSpan, 0.1);
    own->settings().set(SettingId::kTriggerMode, static_cast<double>(protocol::TriggerMode::kFree));
    joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); });
    play(9600);
    analyzer->setFrozen(true);
    ASSERT_TRUE(produce());
    // Freezing makes the match due again: its search goes on over a few frames.
    for (int step = 0; step < 16 && produce(); ++step) {
    }
    ASSERT_FALSE(produce()) << "nothing new while frozen";

    analyzer->setPreviewFix(true);
    ASSERT_TRUE(produce());
    analyzer->setCompared({});
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->has(FrameFlag::kFrozen));
    EXPECT_NEAR(sumGain(), 0.0f, 1e-3f) << "this track alone";
}

// Just after the playhead wraps to the left edge, the match search continues at the right edge,
// where the end of the previous pass is: a snare on beats 2 and 4 and its exact copy stay related
// all round the bar, instead of reading as unrelated while the bar's first beat has no snare.
TEST_F(CompareTest, KeepsAMatchThroughTheCycleStart)
{
    const auto snare = [](std::uint64_t sample) {
        const std::uint64_t beat = (sample / 24000) % 4;
        return (beat == 1 || beat == 3) && sample % 24000 < 4800 ? noiseAt(sample, 1) : 0.0f;
    };
    tracks.front().second = snare;
    const auto copy = joinWith(snare);
    analyzer->setCompared({idOf(copy)});
    own->settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBar));
    analyzer->setColumns(1000);
    play(std::uint64_t{4} * 48000);
    int frames = 0;
    for (int step = 0; step < 400; ++step) { // Two bars, a frame every 10 ms.
        play(480);
        const auto frame = produce();
        if (!frame) {
            continue;
        }
        ++frames;
        ASSERT_EQ(match(*frame, 1).polarity, 1.0f) << "at playhead " << frame->cursor;
    }
    EXPECT_GT(frames, 300);
}

// When the parts of the sum change, here by the fix previewed while frozen, a long sweep is summed
// again over a few frames, which keep coming until it is done. Thirty-two seconds of two tracks
// take two frames.
TEST_F(CompareTest, SumsALongSweepAgainOverFrames)
{
    joinWith([](std::uint64_t sample) { return -noiseAt(sample, 1); });
    own->settings().set(SettingId::kBeatSpan,
                        static_cast<double>(protocol::BeatSpan::kSixteenBars));
    analyzer->setColumns(1000);
    play(std::uint64_t{33} * 48000);
    for (int step = 0; step < 3; ++step) {
        play(480);
        produce();
    }
    analyzer->setFrozen(true);
    ASSERT_TRUE(produce());
    // Freezing makes the match due again: its search goes on over a few frames.
    for (int step = 0; step < 16 && produce(); ++step) {
    }
    ASSERT_FALSE(produce()) << "nothing new while frozen";

    // Columns of lane `index`: (min, max) pairs.
    const auto columnsOf = [](const DecodedFrame& frame, std::size_t index) {
        const std::size_t values = std::size_t{2} * frame.pointCount;
        const auto begin = frame.data.begin() + static_cast<std::ptrdiff_t>(index * values);
        return std::vector<float>(begin, begin + static_cast<std::ptrdiff_t>(values));
    };
    // The copy is inverted: fixed, the sum is twice this track.
    const auto fixedEverywhere = [&](const DecodedFrame& frame) {
        const std::vector<float> track = columnsOf(frame, 0);
        const std::vector<float> sum = columnsOf(frame, 2);
        for (std::size_t i = 0; i < track.size(); ++i) {
            if (sum[i] != 2.0f * track[i]) {
                return false;
            }
        }
        return true;
    };
    analyzer->setPreviewFix(true);
    auto frame = produce();
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame->laneCount, 3);
    EXPECT_FALSE(fixedEverywhere(*frame)) << "partly summed again";
    frame = produce();
    ASSERT_TRUE(frame) << "frames go on while the sum is redone";
    EXPECT_TRUE(fixedEverywhere(*frame));
    EXPECT_FALSE(produce()) << "and stop once it is done";
}

} // namespace
} // namespace orizon::engine
