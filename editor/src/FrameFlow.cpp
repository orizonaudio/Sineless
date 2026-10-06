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

void FrameFlow::step()
{
    {
        const std::scoped_lock lock(mutex);
        if (!hasCredit) {
            return;
        }
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
