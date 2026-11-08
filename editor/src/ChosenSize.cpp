#include "editor/ChosenSize.h"

#include <algorithm>
#include <cmath>

namespace orizon::editor {

int toWindowUnits(double cssPixels, double scale)
{
    return static_cast<int>(std::ceil(cssPixels * scale - 1e-6));
}

ChosenSize::ChosenSize(std::optional<engine::EditorSize> saved)
{
    if (saved) {
        chosenWidth = std::clamp(saved->width, static_cast<double>(kMinimumWidth), kLargestSize);
        chosenHeight = std::clamp(saved->height, static_cast<double>(kMinimumHeight), kLargestSize);
    }
}

WindowSize ChosenSize::target(double scale, int minimumHeight) const
{
    return {.width = toWindowUnits(chosenWidth, scale),
            .height =
                toWindowUnits(std::max(chosenHeight, static_cast<double>(minimumHeight)), scale)};
}

void ChosenSize::requested(WindowSize size, bool fromGrip)
{
    lastRequest = Request{.size = size, .fromGrip = fromGrip};
}

bool ChosenSize::take(WindowSize size, double scale, int minimumHeight)
{
    // The reply to the editor's own request, however often the host sends it.
    if (lastRequest && size == lastRequest->size) {
        return lastRequest->fromGrip && record(size, scale, minimumHeight);
    }
    // The window at the size it should have, sent again: as it opens, after a display change the
    // host followed itself, or late in the units of the display before.
    if (size == target(scale, minimumHeight) ||
        (previousScale && size == target(*previousScale, minimumHeight))) {
        return false;
    }
    if (!isOpen) {
        return false;
    }
    lastRequest.reset(); // The host is past any reply.
    return record(size, scale, minimumHeight);
}

bool ChosenSize::record(WindowSize size, double scale, int minimumHeight)
{
    const double width =
        std::max(static_cast<double>(size.width) / scale, static_cast<double>(kMinimumWidth));
    const double height = size.height > toWindowUnits(minimumHeight, scale)
                              ? static_cast<double>(size.height) / scale
                              : std::min(chosenHeight, static_cast<double>(minimumHeight));
    if (width == chosenWidth && height == chosenHeight) {
        return false;
    }
    chosenWidth = width;
    chosenHeight = height;
    return true;
}

} // namespace orizon::editor
