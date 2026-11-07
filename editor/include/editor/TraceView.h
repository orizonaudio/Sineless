#pragma once

#include "editor/FrameView.h"
#include "editor/TraceGeometry.h"
#include "protocol/Protocol.h"

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

namespace orizon::editor {

// Red, green, blue and alpha, 0 to 1 (sRGB, not premultiplied).
using Rgba = std::array<float, 4>;

// What only the page knows about drawing the traces, from its traceView message
// (protocol/src/messages.ts, TraceViewMessage): the view window on screen, the layout, the vertical
// scale, the Waiting dim, the line width, the colours and the grid the page draws.
struct TraceView
{
    ViewWindow view;
    protocol::ViewLayout layout = protocol::ViewLayout::kOverlaid;
    double verticalScaleDb = 0.0;
    bool dim = false;
    double lineWidth = 1.0;
    // The lanes the page lays out for a frame with no views and no lanes (No tempo).
    std::size_t emptyFrameLanes = 1;
    std::array<Rgba, protocol::kChannelViewCount> viewColours{};
    struct LaneColour
    {
        std::int64_t id = 0;
        Rgba colour{};
        bool compared = false;
    };
    struct LaneSet
    {
        std::uint32_t laneSet = 0;
        std::vector<LaneColour> lanes;
    };
    std::vector<LaneSet> laneSets;
    std::vector<Rgba> fallbackColours;
    Rgba sumColour{};
    Rgba screen{};
    Rgba grid{};
    Rgba axis{};
    Rgba cursor{};
    Rgba previous{};
    Rgba cancellation{};
    // The grid the page draws (its gridChoice): Cross hair arrives only in Trig.
    protocol::GridType gridType = protocol::GridType::kGrid;
    protocol::GridDensity gridTime = protocol::GridDensity::kMedium;
    protocol::GridDensity gridLevel = protocol::GridDensity::kMedium;
    protocol::BeatDivision beatDivision = protocol::BeatDivision::kRegular;

    // A frame's lane colours, as the page picks them: its lane set's (the newest held when the
    // frame's is not), the Sum's last in Compare; by position from the fallback palette when the
    // count does not match; tracks left out of a comparison faint.
    [[nodiscard]] std::vector<Rgba> laneColours(const FrameView& frame) const;
    // Which of a frame's tracks are compared, in lane order (Compare's shading), as picked above.
    [[nodiscard]] std::vector<bool> comparedLanes(const FrameView& frame) const;

    // The message parsed, or nullopt when any field is missing, of the wrong type, not finite or
    // out of range: the editor keeps the last good one (QS-7).
    static std::optional<TraceView> parse(const nlohmann::json& message);
};

// The screen's padding box in the container, in its points from the top left, worked out from the
// container's size and the page's insets (protocol/src/limits.ts): the page lays out at its
// minimum width when a host holds the view narrower, and the height is floored at one point,
// where `visible` is false (a host holding the window shorter than the toolbar and status bar).
struct ScreenRect
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool visible = true;

    static ScreenRect of(double containerWidth, double containerHeight);
};

} // namespace orizon::editor
