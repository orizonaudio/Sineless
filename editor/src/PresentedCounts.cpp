#include "editor/PresentedCounts.h"

#include <cmath>

namespace orizon::editor {

void PresentedCounts::add(double presentedTime, double periodSeconds)
{
    if (presentedTime <= 0.0) {
        dropped.fetch_add(1);
        return;
    }
    presented.fetch_add(1);
    const double previous = last.exchange(presentedTime);
    if (previous <= 0.0 || periodSeconds <= 0.0 || presentedTime <= previous) {
        return;
    }
    const auto refreshes =
        static_cast<std::uint64_t>(std::floor((presentedTime - previous) / periodSeconds + 0.5));
    if (refreshes > 1) {
        withoutNewFrame.fetch_add(refreshes - 1);
    }
}

PresentedCounts::Totals PresentedCounts::totals() const
{
    return {.presented = presented.load(),
            .dropped = dropped.load(),
            .refreshesWithoutNewFrame = withoutNewFrame.load()};
}

} // namespace orizon::editor
