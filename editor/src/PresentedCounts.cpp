#include "editor/PresentedCounts.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

void PresentedDelay::add(double seconds)
{
    const std::scoped_lock lock(mutex);
    delays[next] = seconds;
    next = (next + 1) % kPictures;
    count = std::min(count + 1, kPictures);
}

void PresentedDelay::restart()
{
    const std::scoped_lock lock(mutex);
    count = 0;
    next = 0;
}

double PresentedDelay::medianSeconds() const
{
    std::array<double, kPictures> sorted{};
    std::size_t held = 0;
    {
        const std::scoped_lock lock(mutex);
        sorted = delays;
        held = count;
    }
    if (held == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto end = sorted.begin() + static_cast<std::ptrdiff_t>(held);
    std::sort(sorted.begin(), end);
    // The middle one, or the mean of the middle two.
    const std::size_t upper = held / 2;
    return held % 2 == 1 ? sorted[upper] : (sorted[upper - 1] + sorted[upper]) / 2.0;
}

} // namespace orizon::editor
