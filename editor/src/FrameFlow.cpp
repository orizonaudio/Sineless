#include "editor/FrameFlow.h"

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
}

void FrameFlow::requestForDisplay()
{
    const std::scoped_lock lock(mutex);
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
        if (!credit) {
            analyzer.advance(); // No refresh waits for a frame: keep the sweeps current.
            return;
        }
        if (!analyzer.produce(producing)) {
            return; // Nothing new: keep the request for the next step.
        }
        bool copy = false;
        {
            const std::scoped_lock lock(mutex);
            displayRequest = false;
            displayFrame.swap(producing);
            displayReady = true;
            // The page's open request takes a copy of the frame on screen.
            if (hasCredit) {
                hasCredit = false;
                pending = displayFrame;
                copy = true;
            }
        }
        if (copy) {
            dispatcher.post([this] { deliverPending(); });
        }
        return;
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
    }
    if (!delivering.empty()) {
        deliver(delivering);
        delivering.clear();
    }
}

} // namespace orizon::editor
