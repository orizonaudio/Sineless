#include "engine/AnalysisWorker.h"

#include <utility>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace orizon::engine {

AnalysisWorker::AnalysisWorker(std::function<void()> task, std::chrono::microseconds interval)
    : task(std::move(task)), interval(interval), thread([this] { run(); })
{}

AnalysisWorker::~AnalysisWorker()
{
    {
        const std::scoped_lock lock(mutex);
        stopping = true;
    }
    condition.notify_one();
    thread.join();
}

void AnalysisWorker::wake()
{
    {
        const std::scoped_lock lock(mutex);
        woken = true;
    }
    condition.notify_one();
}

void AnalysisWorker::run()
{
#if defined(__APPLE__)
    pthread_setname_np("O-Scope analysis"); // Named for profilers and the benchmark.
#endif
    std::unique_lock lock(mutex);
    while (!stopping) {
        woken = false;
        lock.unlock();
        task();
        lock.lock();
        condition.wait_for(lock, interval, [this] { return stopping || woken; });
    }
}

} // namespace orizon::engine
