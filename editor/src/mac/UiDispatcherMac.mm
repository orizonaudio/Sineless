#include "editor/UiDispatcher.h"

#include <dispatch/dispatch.h>
#include <memory>
#include <utility>
#include <vector>

namespace orizon::editor {
namespace {

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

std::unique_ptr<UiDispatcher> UiDispatcher::create()
{
    return std::make_unique<UiDispatcherMac>();
}

} // namespace orizon::editor
