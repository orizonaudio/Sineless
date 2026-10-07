#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace orizon::engine {

// The per-instance analysis thread. It exists only while an editor is open:
// construction starts it, destruction stops and joins it. The task runs repeatedly, every
// `interval` or as soon as wake() is called, and is never interrupted mid-run.
class AnalysisWorker
{
public:
    // Short enough to follow new audio closely, long enough to stay idle between blocks.
    static constexpr std::chrono::milliseconds kDefaultInterval{2};

    explicit AnalysisWorker(std::function<void()> task,
                            std::chrono::microseconds interval = kDefaultInterval);
    ~AnalysisWorker();
    AnalysisWorker(const AnalysisWorker&) = delete;
    AnalysisWorker& operator=(const AnalysisWorker&) = delete;

    // Runs the task again without waiting for the interval. Callable from any thread except the
    // audio thread.
    void wake();

private:
    void run();

    std::function<void()> task;
    std::chrono::microseconds interval;
    std::mutex mutex;
    std::condition_variable condition;
    bool stopping = false;
    bool woken = false;
    std::thread thread; // Last member: starts after everything it uses is initialised.
};

} // namespace orizon::engine
