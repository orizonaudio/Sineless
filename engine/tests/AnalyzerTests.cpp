#include "engine/Analyzer.h"

#include <array>
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
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

struct DecodedFrame
{
    std::uint32_t sequence = 0;
    FrameMode mode = FrameMode::kRaw;
    std::uint8_t flags = 0;
    std::uint8_t views = 0;
    std::uint32_t pointCount = 0;
    float samplesPerPoint = 0.0f;
    float startFraction = 0.0f;
    float peak = 0.0f;
    float rms = 0.0f;
    float frequency = 0.0f;
    float cursor = -1.0f;
    float viewStart = 0.0f;
    float viewEnd = 1.0f;
    float visibleSamples = 0.0f;
    std::vector<float> data;

    [[nodiscard]] bool has(FrameFlag value) const
    {
        return (flags & static_cast<std::uint8_t>(value)) != 0;
    }
};

template <typename Value> Value get(const std::vector<std::byte>& bytes, std::size_t offset)
{
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return value;
}

DecodedFrame decode(const std::vector<std::byte>& bytes)
{
    DecodedFrame frame;
    frame.sequence = get<std::uint32_t>(bytes, FrameHeader::kSequenceOffset);
    frame.mode = static_cast<FrameMode>(get<std::uint8_t>(bytes, FrameHeader::kModeOffset));
    frame.flags = get<std::uint8_t>(bytes, FrameHeader::kFlagsOffset);
    frame.views = get<std::uint8_t>(bytes, FrameHeader::kViewsOffset);
    frame.pointCount = get<std::uint32_t>(bytes, FrameHeader::kPointCountOffset);
    frame.samplesPerPoint = get<float>(bytes, FrameHeader::kSamplesPerPointOffset);
    frame.startFraction = get<float>(bytes, FrameHeader::kStartFractionOffset);
    frame.peak = get<float>(bytes, FrameHeader::kPeakOffset);
    frame.rms = get<float>(bytes, FrameHeader::kRmsOffset);
    frame.frequency = get<float>(bytes, FrameHeader::kFrequencyOffset);
    frame.cursor = get<float>(bytes, FrameHeader::kCursorOffset);
    frame.viewStart = get<float>(bytes, FrameHeader::kViewStartOffset);
    frame.viewEnd = get<float>(bytes, FrameHeader::kViewEndOffset);
    frame.visibleSamples = get<float>(bytes, FrameHeader::kVisibleSamplesOffset);
    frame.data.resize((bytes.size() - FrameHeader::kSize) / sizeof(float));
    std::memcpy(frame.data.data(), bytes.data() + FrameHeader::kSize,
                frame.data.size() * sizeof(float));
    return frame;
}

class AnalyzerTest : public ::testing::Test
{
protected:
    void SetUp() override { engine.setSampleRate(kSampleRate); }

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
    EXPECT_FALSE(frame->has(FrameFlag::kRoll));
    EXPECT_EQ(frame->views, 0b0011) << "default views: L + R";
    EXPECT_EQ(frame->pointCount, 400u);
    EXPECT_EQ(frame->data.size(), 2u * 400u * 2u) << "two views of min/max pairs";
    EXPECT_NEAR(frame->samplesPerPoint, 0.02 * kSampleRate / 400, 1e-3);

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
    EXPECT_TRUE(frame->has(FrameFlag::kRoll));
    EXPECT_FALSE(frame->has(FrameFlag::kTriggered));
    EXPECT_EQ(frame->samplesPerPoint, 192.0f) << "2 s * 48 kHz / 500 columns";
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

    void writeRamp(std::uint64_t count)
    {
        const std::uint64_t start = engine.captureRing().writeIndex();
        std::vector<float> ramp(count);
        for (std::uint64_t i = 0; i < count; ++i) {
            ramp[i] = static_cast<float>(start + i);
        }
        std::array<const float*, 2> channels{ramp.data(), ramp.data()};
        engine.captureRing().write(channels.data(), 2, static_cast<int>(count));
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
    EXPECT_NEAR(frame->samplesPerPoint, 1.2f, 1e-6f);
    EXPECT_EQ(frame->data[0], 48000.0f - 4800.0f) << "first column starts at the window start";
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
    EXPECT_TRUE(frame->has(FrameFlag::kRoll));
    EXPECT_EQ(frame->samplesPerPoint, 96.0f);
    const float firstColumnMinimum = frame->data[0];
    EXPECT_EQ(static_cast<std::uint64_t>(firstColumnMinimum) % 96, 0u);
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
    void play(std::uint64_t count, bool playing = true)
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
            engine.timeline().advance({.playing = playing,
                                       .positionValid = true,
                                       .quarterNotes = static_cast<double>(start) / 24000.0,
                                       .tempoValid = true,
                                       .tempo = 120.0},
                                      static_cast<int>(kBlock), kSampleRate);
        }
    }
};

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
TEST_F(SweepTest, LimitsAFrozenSweepToWhatWasHeld)
{
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBar));
    play(200160); // About two bars in total.
    ASSERT_TRUE(produce());
    analyzer.setFrozen(true);
    ASSERT_TRUE(produce());
    engine.settings().set(SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kFourBars));
    const auto frame = produce();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->has(FrameFlag::kLimited));
    EXPECT_EQ(frame->visibleSamples, 192000.0f) << "two bars";
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

TEST_F(AnalyzerTest, NumbersFramesSequentially)
{
    writeTone(440.0, 0.5, 0.5);
    engine.settings().set(SettingId::kTriggerMode, 2.0); // Free: a frame on every step.
    analyzer.setColumns(400);
    const auto first = produce();
    writeTone(440.0, 0.01, 0.5);
    const auto second = produce();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->sequence, first->sequence + 1);
}

} // namespace
} // namespace orizon::engine
