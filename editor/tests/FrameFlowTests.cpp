#include "editor/FrameFlow.h"

#include <array>
#include <deque>
#include <gtest/gtest.h>
#include <vector>

namespace orizon::editor {
namespace {

// Runs posted work only when the test says so, like a busy UI thread.
class ManualDispatcher final : public UiDispatcher
{
public:
    void post(std::function<void()> work) override { queue.push_back(std::move(work)); }
    void every(std::chrono::milliseconds /*interval*/, std::function<void()> /*work*/) override {}

    void runAll()
    {
        while (!queue.empty()) {
            auto work = std::move(queue.front());
            queue.pop_front();
            work();
        }
    }

    std::deque<std::function<void()>> queue;
};

class FrameFlowTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        engine.setSampleRate(48000.0);
        // These tests use the seconds time base; Beats is the default.
        engine.settings().set(protocol::SettingId::kTimeBase,
                              static_cast<double>(protocol::TimeBase::kSeconds));
        engine.settings().set(protocol::SettingId::kTriggerMode, 2.0); // Free: always a frame.
        writeSamples(48000);
        analyzer.setColumns(200);
    }

    void writeSamples(int count)
    {
        std::vector<float> samples(static_cast<std::size_t>(count), 0.25f);
        std::array<const float*, 2> channels{samples.data(), samples.data()};
        engine.captureRing().write(channels.data(), 2, count);
    }

    engine::Engine engine;
    engine::Analyzer analyzer{engine};
    ManualDispatcher dispatcher;
    int delivered = 0;
    std::uint8_t lastFlags = 0;
    FrameFlow flow{analyzer, dispatcher, [this](std::span<const std::byte> frame) {
                       EXPECT_GE(frame.size(), protocol::FrameHeader::kSize);
                       lastFlags =
                           static_cast<std::uint8_t>(frame[protocol::FrameHeader::kFlagsOffset]);
                       ++delivered;
                   }};
};

// Without a credit (the page hidden), the worker still advances tempo-synced sweeps, so a sweep
// longer than the capture misses nothing meanwhile.
TEST_F(FrameFlowTest, AdvancesSweepsWithoutACredit)
{
    engine.settings().set(protocol::SettingId::kTimeBase,
                          static_cast<double>(protocol::TimeBase::kBeats));
    engine.settings().set(protocol::SettingId::kBeatSpan,
                          static_cast<double>(protocol::BeatSpan::kFourBars));
    // At 20 BPM four bars are 48 s, more than the capture holds; play 60 s in 10 ms blocks.
    std::uint64_t sample = 0;
    for (int block = 0; block < 6000; ++block) {
        writeSamples(480);
        engine.timeline().advance({.playing = true,
                                   .positionValid = true,
                                   .quarterNotes = static_cast<double>(sample) / 144000.0,
                                   .tempoValid = true,
                                   .tempo = 20.0},
                                  480, 48000.0);
        sample += 480;
        if (block % 100 == 0) {
            flow.step();
        }
    }
    EXPECT_EQ(delivered, 0);
    flow.grantCredit();
    flow.step();
    dispatcher.runAll();
    ASSERT_EQ(delivered, 1);
    EXPECT_EQ(lastFlags & static_cast<std::uint8_t>(protocol::FrameFlag::kPartial), 0)
        << "the whole sweep was placed while no frame was wanted";
}

TEST_F(FrameFlowTest, ProducesNothingWithoutACredit)
{
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 0);
}

// Fault injection: a page that never loads never asks for a frame. However long the editor stays
// open, the worker hands the UI thread nothing, so no work or frame queues up for it.
TEST_F(FrameFlowTest, QueuesNothingForAPageThatNeverAsks)
{
    for (int step = 0; step < 1000; ++step) {
        writeSamples(480);
        flow.step();
        ASSERT_TRUE(dispatcher.queue.empty()) << "step " << step;
    }
    EXPECT_EQ(delivered, 0);
    // A page that loads late gets a frame on its first request.
    flow.grantCredit();
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 1);
}

TEST_F(FrameFlowTest, DeliversOneFramePerCreditOnTheUiThread)
{
    flow.grantCredit();
    flow.step();
    EXPECT_EQ(delivered, 0) << "delivery waits for the UI thread";
    dispatcher.runAll();
    EXPECT_EQ(delivered, 1);

    writeSamples(480);
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 1) << "the credit was used";

    flow.grantCredit();
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 2);
}

// A UI that stops returning credits (hidden page) stops production, and nothing piles up.
TEST_F(FrameFlowTest, BuildsNoBacklogWhileTheUiIsBusy)
{
    flow.grantCredit();
    for (int step = 0; step < 50; ++step) {
        writeSamples(480);
        flow.step();
    }
    EXPECT_EQ(dispatcher.queue.size(), 1u);
    dispatcher.runAll();
    EXPECT_EQ(delivered, 1);
}

TEST_F(FrameFlowTest, KeepsTheCreditWhenThereIsNothingNew)
{
    engine.settings().set(protocol::SettingId::kTriggerMode, 1.0); // Normal: holds on silence.
    std::vector<float> silence(48000, 0.0f);
    std::array<const float*, 2> channels{silence.data(), silence.data()};
    engine.captureRing().write(channels.data(), 2, 48000);

    flow.grantCredit();
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 0);

    engine.settings().set(protocol::SettingId::kTriggerMode, 2.0); // Free.
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 1) << "the unused credit still allows the next frame";
}

} // namespace
} // namespace orizon::editor
