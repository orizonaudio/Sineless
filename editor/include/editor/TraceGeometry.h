#pragma once

#include "editor/FrameView.h"
#include "protocol/Protocol.h"

#include <cstddef>
#include <optional>
#include <span>
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

// One lane's vertical placement in clip space.
struct Lane
{
    double center = 0.0;
    double halfHeight = 1.0;
};
// How a frame's `count` traces are laid out, as the page lays them out (trace-geometry.ts):
// `compared`, the frame is a Compare frame, whose last trace is Compare's lane. The lanes on the
// screen from the top: stacked, one per trace; overlaid, one for all, and with a Compare frame a
// second, below it, for Compare's lane, never overlaid on the tracks (FR-15.1).
[[nodiscard]] std::vector<Lane> screenLanes(std::size_t count, protocol::ViewLayout layout,
                                            bool compared);
// The lane each of the `count` traces is drawn in, in the frame's order.
[[nodiscard]] std::vector<Lane> traceLanes(std::size_t count, protocol::ViewLayout layout,
                                           bool compared);

// The order a frame's `count` traces are layered in, later over earlier, and which are dimmed,
// with `trace` highlighted (FR-16.4), as the page decides it (trace-geometry.ts, highlightOrder):
// only overlaid, and only a trace in a lane it shares with another, so never Compare's lane. The
// highlighted trace last, in front; the others of its lane dimmed; otherwise every trace in order,
// none dimmed.
struct HighlightOrder
{
    std::vector<std::size_t> order;
    std::vector<bool> dimmed;
};
[[nodiscard]] HighlightOrder highlightOrder(std::size_t count, protocol::ViewLayout layout,
                                            bool compared, std::optional<std::size_t> trace);

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
// The level cancellationBands compares at each point of a trace: a min/max column's peak, or a raw
// point's largest level within a cancellationReachDivisor-th of the frame either side, since raw
// points cross zero every cycle; NaN where the point's value is not finite. In one pass, so its
// cost follows the points, however many a raw frame carries.
[[nodiscard]] std::vector<double> cancellationLevels(const FrameView& frame,
                                                     std::span<const float> values);

// Compare's cancellation shading over the sum's lane (protocol/src/limits.ts, cancellationDb):
// every lane before the sum's is a track summed.
[[nodiscard]] std::vector<Band> cancellationBands(const FrameView& frame,
                                                  protocol::ViewLayout layout, ViewWindow view);

// A difference's lane, filled (ui/src/render/trace-geometry.ts, differenceAreas): each wave taken
// as the area between it and the zero line, the lane holds what this track's area and the others'
// share, what only this track's covers (lost) and what only the others' covers (gained). Five
// blocks of the traces' own kind, one after another, each of pointCount (low, high) pairs with
// NaN where the part is absent: shared, lost above, lost below, gained above, gained below.
// Empty for a frame that shows no difference. Every lane before the difference's is a track
// compared; `own` is this track's lane.
inline constexpr std::size_t kDifferenceParts = 5;
[[nodiscard]] std::vector<float> differenceAreas(const FrameView& frame, std::size_t own);

// The level steps below a lane's top (0 is the top) that fit a half-lane `halfLane` CSS pixels
// tall with neighbours `minimumGap` apart.
[[nodiscard]] std::vector<int> levelSteps(double halfLane, double minimumGap);

// A level line `step` dB below a lane's top; strong lines take the axis colour.
struct LevelLine
{
    double step = 0.0;
    bool strong = false;
};
// The level lines of a half-lane `halfLane` CSS pixels tall at a Density's level step, top first:
// Medium's 3 dB lines as levelSteps fits them; Coarse's 6 dB lines, else the top only, else none;
// Fine's, Medium's lines, strong, with the 1 dB lines between neighbouring 3 dB lines that keep
// kGridFineLineGap.
[[nodiscard]] std::vector<LevelLine> levelLines(double halfLane, protocol::GridDensity density);

// The grid in device pixels: lines `lineWidth` wide centred at `x` or `y` from the top left, so
// that their edges fall on whole pixels (an odd width centred mid-pixel, an even one on a pixel
// boundary) and they are drawn crisp; `strong` and `axis` lines take the axis colour (the zero
// lines, and Fine's 3 dB lines).
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
// The equal divisions of the width at a Density's time step.
[[nodiscard]] std::size_t gridColumns(protocol::GridDensity time);
// What to draw: the type, the Density's level step, the number of equal time divisions, and Beats'
// lines in their place when `vertical` is not null.
struct GridOptions
{
    protocol::GridType type = protocol::GridType::kGrid;
    protocol::GridDensity level = protocol::GridDensity::kMedium;
    std::size_t columns = protocol::kGridColumnsMedium;
    const std::vector<VerticalLine>* vertical = nullptr;
};
[[nodiscard]] GridLines gridLines(double width, double height, std::size_t laneCount,
                                  double pixelRatio, const GridOptions& options);

// Which Beats grid to draw: the Density and Beat division, and the screen's width in CSS pixels,
// which Fine's finer step must keep kGridFineBeatLineGap apart on.
struct BeatGrid
{
    protocol::GridDensity time = protocol::GridDensity::kMedium;
    protocol::BeatDivision division = protocol::BeatDivision::kRegular;
    double widthCss = 0.0;
};
// A tempo-synced sweep's grid lines over the part of it on screen: bars, beats or fractions of a
// beat, as the view narrows, one level coarser or finer by the Density, in thirds of the beat in
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
