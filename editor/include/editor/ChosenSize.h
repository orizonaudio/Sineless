#pragma once

#include "engine/State.h"

#include <optional>

namespace orizon::editor {

// A size in the host's window units: CSS pixels times the display scale, rounded up.
struct WindowSize
{
    int width = 0;
    int height = 0;

    bool operator==(const WindowSize&) const = default;
};

// The size in window units that `cssPixels` comes to at `scale`. Rounded up, so that the page
// always has its CSS size; the margin absorbs the error of a size divided by the scale and
// multiplied back, so that a window size taken as CSS pixels comes back the same.
[[nodiscard]] int toWindowUnits(double cssPixels, double scale);

// The editor's size as the user chose it, in CSS pixels, apart from the size the window shows:
// the window is the chosen size, made taller while the stacked lanes need more (their minimum
// height), and returns to it when they no longer do. Only the user's own resizes change it: a
// drag of the host's window edge or of the editor's grip, never the editor's requests for lanes or
// a display scale, nor the host's own placement of the window as it opens. Saved with the project
// (docs/DESIGN.md, 8.4).
class ChosenSize
{
public:
    static constexpr int kMinimumWidth = protocol::kMinimumEditorWidth;
    static constexpr int kMinimumHeight = protocol::kMinimumEditorHeight;
    static constexpr int kDefaultWidth = protocol::kDefaultEditorWidth;
    static constexpr int kDefaultHeight = 580;
    // The largest size on either side, in CSS pixels: beyond any display, and small enough that
    // the size in window units stays well within an int.
    static constexpr double kLargestSize = 16384.0;

    // The size saved, raised to the minimum and lowered to the largest; the default without one.
    explicit ChosenSize(std::optional<engine::EditorSize> saved);

    [[nodiscard]] double width() const { return chosenWidth; }
    [[nodiscard]] double height() const { return chosenHeight; }

    // The size the window should have, at `scale` with the lanes' `minimumHeight` (CSS pixels).
    [[nodiscard]] WindowSize target(double scale, int minimumHeight) const;

    // The editor asks the host for `size`: a reply of that size is not the user's, unless the
    // request came from the user's drag of the grip. Called before asking, as hosts reply within.
    void requested(WindowSize size, bool fromGrip);
    // The display scale changed from `previous`: sizes from the host in its units are not the
    // user's either (some hosts send them late).
    void scaleChanged(double previous) { previousScale = previous; }
    // The window has opened: sizes from the host from now on may be the user's. Before, they are
    // the host's own placement of the window, which may hold an old forced height.
    void open() { isOpen = true; }
    void close() { isOpen = false; }
    [[nodiscard]] bool opened() const { return isOpen; }

    // The host gave the window `size` at `scale`, with the lanes' `minimumHeight`: takes it as the
    // chosen size if it is the user's. True if the chosen size changed.
    bool take(WindowSize size, double scale, int minimumHeight);

private:
    struct Request
    {
        WindowSize size;
        bool fromGrip = false;
    };

    // The user's size, at least the minimum: the width, and the height above the lanes' minimum.
    // At the minimum (or below it, which an Audio Unit host allows during a drag) the user went as
    // low as they could: the chosen height comes down to the minimum, and never rises to it.
    bool record(WindowSize size, double scale, int minimumHeight);

    double chosenWidth = kDefaultWidth;
    double chosenHeight = kDefaultHeight;
    std::optional<Request> lastRequest;
    std::optional<double> previousScale;
    bool isOpen = false;
};

} // namespace orizon::editor
