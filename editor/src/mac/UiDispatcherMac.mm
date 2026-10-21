#include "editor/UiDispatcher.h"
#include "mac/UiDispatcherMacTesting.h"

#include <atomic>
#include <dispatch/dispatch.h>
#include <memory>
#include <utility>
#include <vector>

namespace orizon::editor {
namespace {

// The timers created and not yet freed: libdispatch calls a source's finalizer when it frees it.
std::atomic<int> liveTimers{0};

void timerFreed(void* /*context*/)
{
    liveTimers.fetch_sub(1, std::memory_order_relaxed);
}

class UiDispatcherMac final : public UiDispatcher
{
public:
    ~UiDispatcherMac() override
    {
        *alive = false; // Destroyed on the UI thread, so queued work sees this before running.
        for (dispatch_source_t timer : timers) {
            dispatch_source_cancel(timer);
        }
    }

    void post(std::function<void()> work) override
    {
        auto token = alive;
        auto task = std::make_shared<std::function<void()>>(std::move(work));
        dispatch_async(dispatch_get_main_queue(), ^{
          if (*token) {
              (*task)();
          }
        });
    }

    void every(std::chrono::milliseconds interval, std::function<void()> work) override
    {
        auto token = alive;
        auto task = std::make_shared<std::function<void()>>(std::move(work));
        dispatch_source_t timer =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        liveTimers.fetch_add(1, std::memory_order_relaxed);
        dispatch_set_finalizer_f(timer, timerFreed); // A finalizer runs only with a context.
        dispatch_set_context(timer, &liveTimers);
        const auto nanoseconds = static_cast<uint64_t>(std::chrono::nanoseconds(interval).count());
        dispatch_source_set_timer(
            timer, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(nanoseconds)), nanoseconds,
            nanoseconds / 10);
        dispatch_source_set_event_handler(timer, ^{
          if (*token) {
              (*task)();
          }
        });
        dispatch_resume(timer);
        timers.push_back(timer);
    }

private:
    // Shared with queued blocks; only read and written on the UI thread.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    // Owned (the file builds with ARC): released after they are cancelled.
    std::vector<dispatch_source_t> timers;
};

} // namespace

int liveTimersForTests()
{
    return liveTimers.load(std::memory_order_relaxed);
}

std::unique_ptr<UiDispatcher> UiDispatcher::create()
{
    return std::make_unique<UiDispatcherMac>();
}

} // namespace orizon::editor
