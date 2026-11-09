#include "editor/FrameFlow.h"

#include <algorithm>
#include <utility>

namespace orizon::editor {

FrameFlow::FrameFlow(engine::Analyzer& analyzer, UiDispatcher& dispatcher, Deliver deliver)
    : analyzer(analyzer), dispatcher(dispatcher), deliver(std::move(deliver))
{}

void FrameFlow::grantCredit()
{
    const std::scoped_lock lock(mutex);
    hasCredit = true;
}

void FrameFlow::setNative(bool isNative)
{
    const std::scoped_lock lock(mutex);
    native = isNative;
    displayRequest = false;
    requestedAt.reset();
}

void FrameFlow::measureDisplayFrames(std::function<void(std::uint64_t nanos)> made,
                                     std::function<std::uint64_t()> clockNanos)
{
    const std::scoped_lock lock(mutex);
    madeForDisplay = std::move(made);
    clock = std::move(clockNanos);
}

void FrameFlow::requestForDisplay(std::uint64_t lateNanos)
{
    const std::scoped_lock lock(mutex);
    if (!displayRequest) {
        const std::uint64_t now = clock();
        requestedAt = now - std::min(now, lateNanos);
    }
    displayRequest = true;
}

bool FrameFlow::takeDisplayFrame(std::vector<std::byte>& into)
{
    const std::scoped_lock lock(mutex);
    if (!displayReady) {
        return false;
    }
    into.swap(displayFrame);
    displayReady = false;
    return true;
}

void FrameFlow::step()
{
    bool credit = false;
    bool display = false;
    {
        const std::scoped_lock lock(mutex);
        credit = hasCredit;
        display = native;
        if (display) {
            credit = displayRequest;
        }
    }
    if (display) {
        std::optional<std::uint64_t> made;
        if (!credit) {
            analyzer.advance(); // No refresh waits for a frame: keep the sweeps current.
        } else if (analyzer.produce(producing)) {
            const std::scoped_lock lock(mutex);
            displayRequest = false;
            displayFrame.swap(producing);
            displayReady = true;
            // The page's copy of the frame on screen, for its open fetch or the next one: frozen,
            // no newer frame may ever come.
            pending = displayFrame;
            pageBehind = true;
            if (requestedAt && madeForDisplay) {
                const std::uint64_t ready = clock();
                made = ready - std::min(ready, *requestedAt);
            }
            requestedAt.reset();
        } else {
            const std::scoped_lock lock(mutex);
            requestedAt.reset(); // Waiting for something new to show, not for the making.
        }
        if (made) {
            madeForDisplay(*made);
        }
        bool give = false;
        {
            const std::scoped_lock lock(mutex);
            if (hasCredit && pageBehind) {
                hasCredit = false;
                give = true;
            }
        }
        if (give) {
            dispatcher.post([this] { deliverPending(); });
        }
        return; // With nothing new, the display's request stays for the next step.
    }
    if (!credit) {
        analyzer.advance(); // No frame wanted: keep the sweeps current meanwhile.
        return;
    }
    if (!analyzer.produce(producing)) {
        return; // Nothing new: keep the credit for the next step.
    }
    {
        const std::scoped_lock lock(mutex);
        hasCredit = false;
        pending.swap(producing);
    }
    dispatcher.post([this] { deliverPending(); });
}

void FrameFlow::deliverPending()
{
    {
        const std::scoped_lock lock(mutex);
        delivering.swap(pending);
        pending.clear();
        pageBehind = false;
    }
    if (!delivering.empty()) {
        deliver(delivering);
        delivering.clear();
    }
}

} // namespace orizon::editor
