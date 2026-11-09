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
    void runFree()
    {
        engine.settings().set(protocol::SettingId::kTimeBase,
                              static_cast<double>(protocol::TimeBase::kFree));
        engine.settings().set(protocol::SettingId::kFreeSpan, 1.0);
    }

    // Trigger in Normal, which holds on silence.
    void holdOnSilence()
    {
        engine.settings().set(protocol::SettingId::kTimeBase,
                              static_cast<double>(protocol::TimeBase::kTrigger));
        engine.settings().set(protocol::SettingId::kTriggerMode,
                              static_cast<double>(protocol::TriggerMode::kNormal));
    }

    void SetUp() override
    {
        engine.prepare(48000.0);
        // These tests run free at 1 s (always a frame); Beats is the default.
        runFree();
        writeSamples(48000);
        analyzer.setColumns(200);
    }

    void writeSamples(int count, float level = 0.25f)
    {
        std::vector<float> samples(static_cast<std::size_t>(count), level);
        std::array<const float*, 2> channels{samples.data(), samples.data()};
        engine.captureRing().write(channels.data(), 2, count);
    }

    engine::Engine engine;
    engine::Analyzer analyzer{engine};
    ManualDispatcher dispatcher;
    int delivered = 0;
    std::uint8_t lastFlags = 0;
    std::vector<std::byte> lastDelivered;
    FrameFlow flow{analyzer, dispatcher, [this](std::span<const std::byte> frame) {
                       EXPECT_GE(frame.size(), protocol::FrameHeader::kSize);
                       lastDelivered.assign(frame.begin(), frame.end());
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
    holdOnSilence();
    std::vector<float> silence(48000, 0.0f);
    std::array<const float*, 2> channels{silence.data(), silence.data()};
    engine.captureRing().write(channels.data(), 2, 48000);

    flow.grantCredit();
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 0);

    runFree();
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 1) << "the unused credit still allows the next frame";
}

// ----- macOS native drawing (AD-30) -----

// The display link's request makes one frame, which its thread takes once; the page gets nothing
// it did not ask for.
TEST_F(FrameFlowTest, MakesOneFramePerDisplayRequestForTheDisplayToTake)
{
    flow.setNative(true);
    std::vector<std::byte> taken;
    EXPECT_FALSE(flow.takeDisplayFrame(taken)) << "nothing asked for yet";
    flow.requestForDisplay();
    flow.step();
    ASSERT_TRUE(flow.takeDisplayFrame(taken));
    EXPECT_GE(taken.size(), protocol::FrameHeader::kSize);
    EXPECT_FALSE(flow.takeDisplayFrame(taken)) << "taken once";
    writeSamples(480);
    flow.step();
    EXPECT_FALSE(flow.takeDisplayFrame(taken)) << "one frame per request";
    dispatcher.runAll();
    EXPECT_EQ(delivered, 0);
}

// The page's fetch in native mode makes no frame of its own: it is answered with a copy of the
// next frame made for the display, so readouts and hover read the frame on screen.
TEST_F(FrameFlowTest, AnswersThePagesFetchWithACopyOfTheDisplaysFrame)
{
    flow.setNative(true);
    flow.grantCredit();
    for (int step = 0; step < 10; ++step) {
        writeSamples(480);
        flow.step();
    }
    dispatcher.runAll();
    EXPECT_EQ(delivered, 0) << "the page's fetch alone makes no frame";
    std::vector<std::byte> copied;
    FrameFlow copying{analyzer, dispatcher, [&copied](std::span<const std::byte> frame) {
                          copied.assign(frame.begin(), frame.end());
                      }};
    copying.setNative(true);
    copying.grantCredit();
    copying.requestForDisplay();
    copying.step();
    std::vector<std::byte> taken;
    ASSERT_TRUE(copying.takeDisplayFrame(taken));
    dispatcher.runAll();
    EXPECT_EQ(copied, taken);
}

// One fetch, one copy: once answered, the frames the display draws next queue nothing for the page,
// whose next fetch gets the next one. A page that stops asking (hidden, busy) costs the host's main
// thread nothing per refresh.
TEST_F(FrameFlowTest, AnswersEachOfThePagesFetchesWithOneCopy)
{
    flow.setNative(true);
    flow.grantCredit();
    std::vector<std::byte> taken;
    const auto drawFrame = [&] {
        writeSamples(480);
        flow.requestForDisplay();
        flow.step();
        ASSERT_TRUE(flow.takeDisplayFrame(taken));
    };
    drawFrame();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 1);
    for (int refresh = 0; refresh < 10; ++refresh) {
        drawFrame();
        ASSERT_TRUE(dispatcher.queue.empty()) << "refresh " << refresh;
    }
    EXPECT_EQ(delivered, 1);
    flow.grantCredit();
    drawFrame();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 2);
}

// A frame made while the page's fetch is closed reaches the page once it asks again, though no
// newer frame comes: frozen, or Single after its capture, the engine makes one frame and then
// none, and the page's Freeze button and readouts follow that frame alone.
TEST_F(FrameFlowTest, GivesThePageTheFrameItMissedWhenItAsksAgain)
{
    flow.setNative(true);
    writeSamples(480);
    flow.requestForDisplay();
    flow.step();
    std::vector<std::byte> taken;
    ASSERT_TRUE(flow.takeDisplayFrame(taken));
    flow.grantCredit();
    for (int step = 0; step < 3; ++step) {
        flow.requestForDisplay();
        flow.step(); // Nothing new.
    }
    dispatcher.runAll();
    ASSERT_EQ(delivered, 1);
    EXPECT_EQ(lastDelivered, taken);
    flow.grantCredit();
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 1) << "the missed frame is given once";
}

// Of several frames drawn while the page's fetch is closed, it gets the newest; and a frame it
// missed never comes after a newer one it was given: frozen, the newer is the last frame there is.
TEST_F(FrameFlowTest, GivesThePageOnlyTheNewestFrameItMissed)
{
    flow.setNative(true);
    std::vector<std::byte> taken;
    float level = 0.1f; // Each frame its own level, so the frames differ.
    const auto drawFrame = [&] {
        level += 0.1f;
        writeSamples(48000, level);
        flow.requestForDisplay();
        flow.step();
        EXPECT_TRUE(flow.takeDisplayFrame(taken));
        return taken;
    };
    const auto oldest = drawFrame();
    drawFrame();
    const auto newest = drawFrame();
    ASSERT_NE(oldest, newest);
    flow.grantCredit();
    flow.step();
    dispatcher.runAll();
    ASSERT_EQ(delivered, 1);
    EXPECT_EQ(lastDelivered, newest) << "the newest of the frames missed";

    const auto missed = drawFrame();
    flow.grantCredit();
    const auto given = drawFrame();
    dispatcher.runAll();
    ASSERT_EQ(delivered, 2);
    EXPECT_EQ(lastDelivered, given);
    flow.grantCredit();
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 2) << "the older frame missed comes after the newer one given";
    EXPECT_NE(lastDelivered, missed);
}

// In native mode the page's open fetch is not a credit: with no request from the display link the
// worker advances the sweeps, so a sweep longer than the capture is complete when drawn (FR-9.5).
TEST_F(FrameFlowTest, AdvancesSweepsWhileOnlyThePagesFetchIsOpen)
{
    flow.setNative(true);
    flow.grantCredit();
    engine.settings().set(protocol::SettingId::kTimeBase,
                          static_cast<double>(protocol::TimeBase::kBeats));
    engine.settings().set(protocol::SettingId::kBeatSpan,
                          static_cast<double>(protocol::BeatSpan::kFourBars));
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
    flow.requestForDisplay();
    flow.step();
    std::vector<std::byte> taken;
    ASSERT_TRUE(flow.takeDisplayFrame(taken));
    EXPECT_EQ(static_cast<std::uint8_t>(taken[protocol::FrameHeader::kFlagsOffset]) &
                  static_cast<std::uint8_t>(protocol::FrameFlag::kPartial),
              0)
        << "the whole sweep was placed while only the page's fetch was open";
}

// A frame that would repeat the last (nothing changed) is not made; the display's request stays
// open for the next step that has something new.
TEST_F(FrameFlowTest, KeepsTheDisplaysRequestWhenThereIsNothingNew)
{
    flow.setNative(true);
    holdOnSilence();
    std::vector<float> silence(48000, 0.0f);
    std::array<const float*, 2> channels{silence.data(), silence.data()};
    engine.captureRing().write(channels.data(), 2, 48000);
    flow.requestForDisplay();
    flow.step();
    std::vector<std::byte> taken;
    EXPECT_FALSE(flow.takeDisplayFrame(taken));
    runFree();
    flow.step();
    EXPECT_TRUE(flow.takeDisplayFrame(taken)) << "the open request allows the next frame";
}

// The pacing asks for frames as early as they take to make (FramePacing::frameMade): each frame
// made for the display reports how long it took from when the request was due, the request's
// timer firing late and the worker's wait included, counted from the first request while it was
// open, as a refresh that asks again does not make the frame any quicker.
TEST_F(FrameFlowTest, ReportsHowLongEachDisplayFrameTookFromItsRequest)
{
    std::uint64_t now = 1000;
    std::vector<std::uint64_t> reported;
    flow.measureDisplayFrames([&](std::uint64_t nanos) { reported.push_back(nanos); },
                              [&] { return now; });
    flow.setNative(true);
    flow.requestForDisplay(700); // Its timer fired 700 ns late.
    now = 1500;
    flow.requestForDisplay(); // The next refresh, before the frame is ready.
    now = 4000;
    flow.step();
    std::vector<std::byte> taken;
    ASSERT_TRUE(flow.takeDisplayFrame(taken));
    EXPECT_EQ(reported, std::vector<std::uint64_t>{3700});
}

// A request kept open because there was nothing new to show (silence held, frozen) waited for
// audio, not for the making: the frame that ends that wait reports nothing, so a frozen spell does
// not make the next frames asked for seconds early. The next request is measured again.
TEST_F(FrameFlowTest, ReportsNothingForAFrameThatWaitedForSomethingNew)
{
    std::uint64_t now = 1000;
    std::vector<std::uint64_t> reported;
    flow.measureDisplayFrames([&](std::uint64_t nanos) { reported.push_back(nanos); },
                              [&] { return now; });
    flow.setNative(true);
    holdOnSilence();
    std::vector<float> silence(48000, 0.0f);
    std::array<const float*, 2> channels{silence.data(), silence.data()};
    engine.captureRing().write(channels.data(), 2, 48000);
    flow.requestForDisplay();
    flow.step();
    std::vector<std::byte> taken;
    ASSERT_FALSE(flow.takeDisplayFrame(taken));
    now = 900000000;
    flow.requestForDisplay(); // Refreshes go on asking meanwhile.
    runFree();
    flow.step();
    ASSERT_TRUE(flow.takeDisplayFrame(taken));
    EXPECT_TRUE(reported.empty()) << "reported the wait for audio as making";
    flow.requestForDisplay();
    now += 2000;
    writeSamples(480);
    flow.step();
    ASSERT_TRUE(flow.takeDisplayFrame(taken));
    EXPECT_EQ(reported, std::vector<std::uint64_t>{2000});
}

// Native drawing lost: the page's fetch makes frames again, as on Windows, so readouts stay live.
TEST_F(FrameFlowTest, MakesFramesForThePageAgainOnceNativeDrawingStops)
{
    flow.setNative(true);
    flow.grantCredit();
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 0);
    flow.setNative(false);
    flow.step();
    dispatcher.runAll();
    EXPECT_EQ(delivered, 1);
}

} // namespace
} // namespace orizon::editor
