#include "engine/AnalysisWorker.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace orizon::engine {
namespace {

using namespace std::chrono_literals;

bool waitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

TEST(AnalysisWorker, RunsTheTaskRepeatedly)
{
    std::atomic<int> runs{0};
    AnalysisWorker worker([&] { ++runs; }, 1ms);
    EXPECT_TRUE(waitUntil([&] { return runs.load() >= 5; }));
}

TEST(AnalysisWorker, RunsOnTheWorkerThread)
{
    std::atomic<std::thread::id> taskThread{};
    {
        AnalysisWorker worker([&] { taskThread = std::this_thread::get_id(); });
        ASSERT_TRUE(waitUntil([&] { return taskThread.load() != std::thread::id{}; }));
    }
    EXPECT_NE(taskThread.load(), std::this_thread::get_id());
}

TEST(AnalysisWorker, StopsRunningWhenDestroyed)
{
    std::atomic<int> runs{0};
    {
        AnalysisWorker worker([&] { ++runs; }, 1ms);
        ASSERT_TRUE(waitUntil([&] { return runs.load() >= 1; }));
    }
    const int afterStop = runs.load();
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(runs.load(), afterStop);
}

TEST(AnalysisWorker, WakeRunsTheTaskWithoutWaitingForTheInterval)
{
    std::atomic<int> runs{0};
    AnalysisWorker worker([&] { ++runs; }, 1h);
    ASSERT_TRUE(waitUntil([&] { return runs.load() == 1; }));
    worker.wake();
    EXPECT_TRUE(waitUntil([&] { return runs.load() == 2; }, 500ms));
}

// Closing the editor while a frame is being produced: destruction waits for the run to finish.
TEST(AnalysisWorker, DestructionWaitsForTheRunningTask)
{
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    {
        AnalysisWorker worker([&] {
            if (started.exchange(true)) {
                return;
            }
            std::this_thread::sleep_for(50ms);
            finished = true;
        });
        ASSERT_TRUE(waitUntil([&] { return started.load(); }));
    }
    EXPECT_TRUE(finished.load());
}

TEST(AnalysisWorker, SurvivesRepeatedStartAndStop)
{
    for (int cycle = 0; cycle < 200; ++cycle) {
        std::atomic<int> runs{0};
        AnalysisWorker worker([&] { ++runs; }, 1ms);
        if (cycle % 2 == 0) {
            ASSERT_TRUE(waitUntil([&] { return runs.load() >= 1; }));
        }
    }
}

} // namespace
} // namespace orizon::engine
