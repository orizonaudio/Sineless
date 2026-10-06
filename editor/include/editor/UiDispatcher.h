#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace orizon::editor {

// Runs work on the UI thread, which owns the webview (DESIGN 5.1). Work posted or scheduled
// through a dispatcher never runs after the dispatcher is destroyed, so it may safely refer to
// objects that the editor destroys together with it.
class UiDispatcher
{
public:
    UiDispatcher() = default;
    virtual ~UiDispatcher() = default;
    UiDispatcher(const UiDispatcher&) = delete;
    UiDispatcher& operator=(const UiDispatcher&) = delete;

    // Any thread. Runs `work` once on the UI thread, soon.
    virtual void post(std::function<void()> work) = 0;
    // UI thread. Runs `work` on the UI thread every `interval` until the dispatcher is destroyed.
    virtual void every(std::chrono::milliseconds interval, std::function<void()> work) = 0;

    // The platform's implementation (macOS: the main dispatch queue).
    static std::unique_ptr<UiDispatcher> create();
};

} // namespace orizon::editor
