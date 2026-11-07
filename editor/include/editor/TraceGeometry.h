#pragma once

#include "editor/FrameView.h"
#include "protocol/Protocol.h"

#include <cstddef>
#include <vector>

namespace orizon::editor {

// Where the macOS editor draws what surrounds the traces, ported from the page so both draw the
// same picture (ui/src/render/trace-geometry.ts, ui/src/view/grid.ts, musical-grid.ts and
// axis-labels.ts); protocol/tests/data/trace-geometry.json holds cases both read. Pure functions.

// The part of the span on screen (fractions); the whole span by default.
struct ViewWindow
{
    double start = 0.0;
    double end = 1.0;
};

// One view's or lane's vertical placement in clip space: overlaid views share the screen, stacked
// ones get a lane each.
struct Lane
{
    double center = 0.0;
    double halfHeight = 1.0;
};
[[nodiscard]] std::vector<Lane> lanes(std::size_t count, protocol::ViewLayout layout);

// Span positions (0 to 1) of a min/max column edge and of a raw point.
[[nodiscard]] double columnPosition(const FrameView& frame, double column);
[[nodiscard]] double pointPosition(const FrameView& frame, double index);
// Clip-space x of a span position under the view window.
[[nodiscard]] double screenX(double position, ViewWindow view);

// A shaded span of the screen, in clip space.
struct Band
{
    double left = 0.0;
    double right = 0.0;
    double bottom = -1.0;
    double top = 1.0;
};
// Compare's cancellation shading over the sum's lane (protocol/src/limits.ts, cancellationDb).
// `compared` marks the compared tracks in lane order.
[[nodiscard]] std::vector<Band> cancellationBands(const FrameView& frame,
                                                  protocol::ViewLayout layout,
                                                  const std::vector<bool>& compared,
                                                  ViewWindow view);

// The level steps below a lane's top (0 is the top) that fit a half-lane `halfLane` CSS pixels
// tall with neighbours `minimumGap` apart.
[[nodiscard]] std::vector<int> levelSteps(double halfLane, double minimumGap);

// A level line `step` dB below a lane's top; strong lines take the axis colour.
struct LevelLine
{
    double step = 0.0;
    bool strong = false;
};
// The level lines of a half-lane `halfLane` CSS pixels tall at a Level step, top first: Medium's
// 3 dB lines as levelSteps fits them; Coarse's 6 dB lines, else the top only, else none; Fine's,
// Medium's lines, strong, with the 1 dB lines between neighbouring 3 dB lines that keep
// kGridFineLineGap.
[[nodiscard]] std::vector<LevelLine> levelLines(double halfLane, protocol::GridDensity density);

// The grid in device pixels: lines `lineWidth` wide centred at `x` or `y` from the top left;
// `strong` and `axis` lines take the axis colour (the zero lines, and Fine's 3 dB lines).
struct GridLines
{
    double lineWidth = 1.0;
    struct Vertical
    {
        double x = 0.0;
        bool strong = false;
    };
    struct Horizontal
    {
        double y = 0.0;
        bool axis = false;
    };
    std::vector<Vertical> vertical;
    std::vector<Horizontal> horizontal;
};
// A vertical line at a fraction of the width (a beat, a bar).
struct VerticalLine
{
    double position = 0.0;
    bool strong = false;
};
// The equal divisions of the width at a Time step.
[[nodiscard]] std::size_t gridColumns(protocol::GridDensity time);
// What to draw: the Level step, the number of equal time divisions, and Beats' lines in their
// place when `vertical` is not null.
struct GridOptions
{
    protocol::GridDensity level = protocol::GridDensity::kMedium;
    std::size_t columns = protocol::kGridColumnsMedium;
    const std::vector<VerticalLine>* vertical = nullptr;
};
[[nodiscard]] GridLines gridLines(double width, double height, std::size_t laneCount,
                                  double pixelRatio, const GridOptions& options);

// Which Beats grid to draw: the Time step and Beat division, and the screen's width in CSS pixels,
// which Fine's finer step must keep kGridFineBeatLineGap apart on.
struct BeatGrid
{
    protocol::GridDensity time = protocol::GridDensity::kMedium;
    protocol::BeatDivision division = protocol::BeatDivision::kRegular;
    double widthCss = 0.0;
};
// A tempo-synced sweep's grid lines over the part of it on screen: bars, beats or fractions of a
// beat, as the view narrows, one level coarser or finer by the Time step, in thirds of the beat in
// Triplets (the page's beatSteps).
[[nodiscard]] std::vector<VerticalLine> musicalGridLines(double cycleStart, double span,
                                                         double barQuarterNotes, ViewWindow view,
                                                         const BeatGrid& grid);

// The playhead in device pixels, at its exact position (AD-30: drawn natively it moves by
// fractions of a pixel, with coverage): one CSS pixel wide as CSS snaps a border (at least one
// device pixel, else rounded down), centred on `position` (0 to 1) across `width` pixels.
struct PixelBand
{
    double left = 0.0;
    double right = 0.0;
};
[[nodiscard]] PixelBand playheadBand(double position, double width, double pixelRatio);
// How much of the pixel column [pixel, pixel + 1) a band covers, 0 to 1.
[[nodiscard]] double bandCoverage(double pixel, PixelBand band);

} // namespace orizon::editor
