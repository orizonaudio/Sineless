#include "editor/TraceView.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace orizon::editor {

namespace {

// Far more lane sets than the page keeps (the newest four), and palette entries than it has
// (sixteen): a message beyond these is malformed.
constexpr std::size_t kMostLaneSets = 16;
constexpr std::size_t kMostFallbackColours = 64;
// The widest trace line a page sends: 1.5 CSS pixels at the highest display scale, with room.
constexpr double kMostLineWidth = 64.0;

std::optional<double> finite(const nlohmann::json& message, const char* name)
{
    const auto found = message.find(name);
    if (found == message.end() || !found->is_number()) {
        return std::nullopt;
    }
    const double value = found->get<double>();
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<Rgba> colour(const nlohmann::json& value)
{
    if (!value.is_array() || value.size() != 4) {
        return std::nullopt;
    }
    Rgba out{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (!value[channel].is_number()) {
            return std::nullopt;
        }
        const double part = value[channel].get<double>();
        if (!std::isfinite(part) || part < 0.0 || part > 1.0) {
            return std::nullopt;
        }
        out[channel] = static_cast<float>(part);
    }
    return out;
}

std::optional<Rgba> colourField(const nlohmann::json& message, const char* name)
{
    const auto found = message.find(name);
    return found == message.end() ? std::nullopt : colour(*found);
}

} // namespace

std::optional<TraceView> TraceView::parse(const nlohmann::json& message)
{
    if (!message.is_object()) {
        return std::nullopt;
    }
    TraceView view;
    const auto start = finite(message, "viewStart");
    const auto end = finite(message, "viewEnd");
    const auto scale = finite(message, "verticalScaleDb");
    const auto lineWidth = finite(message, "lineWidth");
    if (!start || !end || !scale || !lineWidth || *start < 0.0 || *end > 1.0 || *start >= *end ||
        *lineWidth <= 0.0 || *lineWidth > kMostLineWidth) {
        return std::nullopt;
    }
    view.view = {.start = *start, .end = *end};
    view.verticalScaleDb = *scale;
    view.lineWidth = *lineWidth;
    const auto layout = message.find("layout");
    const auto dim = message.find("dim");
    if (layout == message.end() || !layout->is_string() || dim == message.end() ||
        !dim->is_boolean()) {
        return std::nullopt;
    }
    if (*layout == "overlaid") {
        view.layout = protocol::ViewLayout::kOverlaid;
    } else if (*layout == "stacked") {
        view.layout = protocol::ViewLayout::kStacked;
    } else {
        return std::nullopt;
    }
    view.dim = dim->get<bool>();

    const auto viewColours = message.find("viewColours");
    if (viewColours == message.end() || !viewColours->is_array() ||
        viewColours->size() != protocol::kChannelViewCount) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < protocol::kChannelViewCount; ++index) {
        const auto parsed = colour((*viewColours)[index]);
        if (!parsed) {
            return std::nullopt;
        }
        view.viewColours[index] = *parsed;
    }

    const auto laneSets = message.find("laneSets");
    if (laneSets == message.end() || !laneSets->is_array() || laneSets->size() > kMostLaneSets) {
        return std::nullopt;
    }
    for (const nlohmann::json& set : *laneSets) {
        const auto laneSet = set.is_object() ? set.find("laneSet") : set.end();
        const auto lanes = set.is_object() ? set.find("lanes") : set.end();
        if (laneSet == set.end() || !laneSet->is_number_unsigned() || lanes == set.end() ||
            !lanes->is_array() || lanes->size() > protocol::kMaxLanes + 1) {
            return std::nullopt;
        }
        LaneSet parsed{.laneSet = laneSet->get<std::uint32_t>()};
        for (const nlohmann::json& lane : *lanes) {
            const auto id = lane.is_object() ? lane.find("id") : lane.end();
            const auto compared = lane.is_object() ? lane.find("compared") : lane.end();
            const auto laneColour = lane.is_object() ? colourField(lane, "colour") : std::nullopt;
            if (id == lane.end() || !id->is_number_integer() || compared == lane.end() ||
                !compared->is_boolean() || !laneColour) {
                return std::nullopt;
            }
            parsed.lanes.push_back({.id = id->get<std::int64_t>(),
                                    .colour = *laneColour,
                                    .compared = compared->get<bool>()});
        }
        view.laneSets.push_back(std::move(parsed));
    }

    const auto fallback = message.find("fallbackColours");
    if (fallback == message.end() || !fallback->is_array() || fallback->empty() ||
        fallback->size() > kMostFallbackColours) {
        return std::nullopt;
    }
    for (const nlohmann::json& entry : *fallback) {
        const auto parsed = colour(entry);
        if (!parsed) {
            return std::nullopt;
        }
        view.fallbackColours.push_back(*parsed);
    }

    const std::array<std::pair<const char*, Rgba*>, 7> colours{{
        {"sumColour", &view.sumColour},
        {"screen", &view.screen},
        {"grid", &view.grid},
        {"axis", &view.axis},
        {"cursor", &view.cursor},
        {"previous", &view.previous},
        {"cancellation", &view.cancellation},
    }};
    for (const auto& [name, into] : colours) {
        const auto parsed = colourField(message, name);
        if (!parsed) {
            return std::nullopt;
        }
        *into = *parsed;
    }
    return view;
}

namespace {

// The lane set a frame's colours come from: its own, else the newest held; nullptr if none.
const TraceView::LaneSet* setFor(const TraceView& view, const FrameView& frame)
{
    const TraceView::LaneSet* newest = nullptr;
    for (const auto& set : view.laneSets) {
        if (set.laneSet == frame.laneSet) {
            return &set;
        }
        if (newest == nullptr || set.laneSet > newest->laneSet) {
            newest = &set;
        }
    }
    return newest;
}

struct LaneStyle
{
    Rgba colour{};
    bool compared = false;
    bool sum = false;
};

std::vector<LaneStyle> stylesFor(const TraceView& view, const FrameView& frame)
{
    const std::size_t count = frame.lanes.size();
    std::vector<LaneStyle> styles;
    if (const TraceView::LaneSet* set = setFor(view, frame)) {
        for (const auto& lane : set->lanes) {
            styles.push_back({.colour = lane.colour, .compared = lane.compared});
        }
    }
    if (frame.compared() && !styles.empty()) {
        styles.push_back({.colour = view.sumColour, .sum = true});
    }
    if (styles.size() == count) {
        return styles;
    }
    // A set that does not match the frame's lanes yet: colours by position.
    styles.clear();
    for (std::size_t index = 0; index < count; ++index) {
        styles.push_back({.colour = view.fallbackColours[index % view.fallbackColours.size()]});
    }
    return styles;
}

} // namespace

std::vector<Rgba> TraceView::laneColours(const FrameView& frame) const
{
    std::vector<Rgba> out;
    for (const LaneStyle& style : stylesFor(*this, frame)) {
        Rgba colour = style.colour;
        // Tracks left out of a comparison are drawn faint.
        if (frame.compared() && !style.compared && !style.sum) {
            colour[3] *= 0.2f;
        }
        out.push_back(colour);
    }
    return out;
}

std::vector<bool> TraceView::comparedLanes(const FrameView& frame) const
{
    std::vector<bool> out;
    for (const LaneStyle& style : stylesFor(*this, frame)) {
        out.push_back(style.compared);
    }
    return out;
}

ScreenRect ScreenRect::of(double containerWidth, double containerHeight)
{
    const double inset = protocol::kScreenMargin + protocol::kScreenBorder;
    const double width =
        std::max(containerWidth, static_cast<double>(protocol::kMinimumEditorWidth)) - 2.0 * inset;
    const double height = containerHeight - protocol::kToolbarHeight - protocol::kStatusBarHeight -
                          2.0 * protocol::kScreenBorder;
    return {.x = inset,
            .y = static_cast<double>(protocol::kToolbarHeight + protocol::kScreenBorder),
            .width = width,
            .height = std::max(1.0, height),
            .visible = height >= 1.0};
}

} // namespace orizon::editor
