#include "editor/TraceGeometry.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>

namespace orizon::editor {

namespace {

// `count` equal lanes from the top of the screen.
std::vector<Lane> equalLanes(std::size_t count)
{
    const double halfHeight = 1.0 / static_cast<double>(count);
    std::vector<Lane> out;
    out.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        out.push_back({.center = 1.0 - halfHeight * static_cast<double>(2 * index + 1),
                       .halfHeight = halfHeight});
    }
    return out;
}

// Which of screenLanes' lanes the trace at `index` sits in.
std::size_t screenLaneOf(std::size_t count, protocol::ViewLayout layout, bool compared,
                         std::size_t index)
{
    if (layout == protocol::ViewLayout::kStacked) {
        return index;
    }
    return compared && count > 1 && index + 1 == count ? 1 : 0;
}

} // namespace

std::vector<Lane> screenLanes(std::size_t count, protocol::ViewLayout layout, bool compared)
{
    if (layout == protocol::ViewLayout::kStacked) {
        return equalLanes(std::max<std::size_t>(count, 1));
    }
    return equalLanes(compared && count > 1 ? 2 : 1);
}

std::vector<Lane> traceLanes(std::size_t count, protocol::ViewLayout layout, bool compared)
{
    const std::vector<Lane> screen = screenLanes(count, layout, compared);
    std::vector<Lane> out;
    out.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        out.push_back(screen[screenLaneOf(count, layout, compared, index)]);
    }
    return out;
}

double columnPosition(const FrameView& frame, double column)
{
    return frame.viewStart + column / static_cast<double>(frame.pointCount) *
                                 static_cast<double>(frame.viewEnd - frame.viewStart);
}

// Raw point k sits k / pointsPerSample samples after the first sample, which is startFraction
// of a sample before the covered part's start.
double pointPosition(const FrameView& frame, double index)
{
    const double width = frame.viewEnd - frame.viewStart;
    if (frame.visibleSamples <= 0.0f) {
        return frame.viewStart + index / static_cast<double>(frame.pointCount) * width;
    }
    return frame.viewStart + (index / static_cast<double>(frame.pointsPerSample) -
                              static_cast<double>(frame.startFraction)) /
                                 static_cast<double>(frame.visibleSamples) * width;
}

double screenX(double position, ViewWindow view)
{
    return -1.0 + 2.0 * (position - view.start) / (view.end - view.start);
}

// Each point's peak, NaN where a value is not finite; raw points over a short window.
std::vector<double> cancellationLevels(const FrameView& frame, std::span<const float> values)
{
    const std::size_t count = frame.pointCount;
    const bool minMax = frame.mode == protocol::FrameMode::kMinMax;
    std::vector<double> out(count);
    for (std::size_t point = 0; point < count; ++point) {
        out[point] = minMax ? std::max(std::abs(static_cast<double>(values[point * 2])),
                                       std::abs(static_cast<double>(values[point * 2 + 1])))
                            : std::abs(static_cast<double>(values[point]));
    }
    const std::size_t reach =
        minMax ? 0 : std::max<std::size_t>(1, count / protocol::kCancellationReachDivisor);
    // The largest finite level within `reach` points each side, in one pass: indices whose
    // levels fall from the front, so the front is the window's largest (a point not finite counts
    // as silence, which no level is below).
    std::vector<double> smoothed(count);
    std::deque<std::size_t> window;
    const auto level = [&](std::size_t point) {
        return std::isfinite(out[point]) ? out[point] : 0.0;
    };
    std::size_t next = 0; // The next point to enter the window.
    for (std::size_t point = 0; point < count; ++point) {
        const std::size_t to = std::min(count - 1, point + reach);
        for (; next <= to; ++next) {
            while (!window.empty() && level(window.back()) <= level(next)) {
                window.pop_back();
            }
            window.push_back(next);
        }
        while (window.front() + reach < point) {
            window.pop_front();
        }
        smoothed[point] = std::isfinite(out[point]) ? level(window.front())
                                                    : std::numeric_limits<double>::quiet_NaN();
    }
    return smoothed;
}

std::vector<float> differenceAreas(const FrameView& frame, std::size_t own)
{
    if (!frame.compared() || frame.compareLane != protocol::CompareLane::kDifference ||
        frame.lanes.size() < 2 || own + 1 >= frame.lanes.size() || frame.pointCount == 0) {
        return {};
    }
    const std::span<const float> difference = frame.lanes.back();
    const std::span<const float> mine = frame.lanes[own];
    std::vector<std::span<const float>> others;
    for (std::size_t index = 0; index + 1 < frame.lanes.size(); ++index) {
        if (index != own) {
            others.push_back(frame.lanes[index]);
        }
    }
    if (others.empty()) {
        return {};
    }
    const std::size_t count = frame.pointCount;
    const float none = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> all(kDifferenceParts * count * 2, none);
    const auto block = [&](std::size_t part) {
        return std::span(all).subspan(part * count * 2, count * 2);
    };
    const std::span<float> shared = block(0);
    const std::span<float> lostAbove = block(1);
    const std::span<float> lostBelow = block(2);
    const std::span<float> gainedAbove = block(3);
    const std::span<float> gainedBelow = block(4);
    const bool columns = frame.mode == protocol::FrameMode::kMinMax;
    for (std::size_t point = 0; point < count; ++point) {
        // How far each wave reaches from zero at this point: a column's lowest and highest
        // sample, or a raw point's value. The others' wave is their columns added, or, point by
        // point, this track's less the difference. Worked out in double and stored as float, as
        // the page works it out and stores it.
        double low = 0.0;
        double high = 0.0;
        double otherLow = 0.0;
        double otherHigh = 0.0;
        if (columns) {
            low = mine[point * 2];
            high = mine[point * 2 + 1];
            for (const std::span<const float> other : others) {
                otherLow += other[point * 2];
                otherHigh += other[point * 2 + 1];
            }
        } else {
            low = mine[point];
            high = low;
            otherLow = low - difference[point];
            otherHigh = otherLow;
        }
        if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(otherLow) ||
            !std::isfinite(otherHigh)) {
            continue;
        }
        low = std::min(0.0, low);
        high = std::max(0.0, high);
        otherLow = std::min(0.0, otherLow);
        otherHigh = std::max(0.0, otherHigh);
        const double sharedLow = std::max(low, otherLow);
        const double sharedHigh = std::min(high, otherHigh);
        shared[point * 2] = static_cast<float>(sharedLow);
        shared[point * 2 + 1] = static_cast<float>(sharedHigh);
        if (otherHigh != high) {
            const std::span<float> above = otherHigh > high ? gainedAbove : lostAbove;
            above[point * 2] = static_cast<float>(sharedHigh);
            above[point * 2 + 1] = static_cast<float>(std::max(high, otherHigh));
        }
        if (otherLow != low) {
            const std::span<float> below = otherLow < low ? gainedBelow : lostBelow;
            below[point * 2] = static_cast<float>(std::min(low, otherLow));
            below[point * 2 + 1] = static_cast<float>(sharedLow);
        }
    }
    // The tapers: a part absent at a point with a present neighbour ends on the shared edge.
    const auto taper = [&](std::span<float> part, std::size_t edge) {
        const auto present = [&](std::size_t point) {
            return point < count && std::isfinite(part[point * 2]) &&
                   part[point * 2] != part[point * 2 + 1];
        };
        for (std::size_t point = 0; point < count; ++point) {
            const float at = shared[point * 2 + edge];
            // point - 1 wraps past the count at 0, which `present` refuses.
            if (std::isnan(part[point * 2]) && std::isfinite(at) &&
                (present(point - 1) || present(point + 1))) {
                part[point * 2] = at;
                part[point * 2 + 1] = at;
            }
        }
    };
    taper(lostAbove, 1);
    taper(gainedAbove, 1);
    taper(lostBelow, 0);
    taper(gainedBelow, 0);
    return all;
}

std::vector<Band> cancellationBands(const FrameView& frame, protocol::ViewLayout layout,
                                    ViewWindow view)
{
    // The difference is not a sum: nothing in it is a cancellation to mark, as on the page.
    if (!frame.compared() || frame.compareLane == protocol::CompareLane::kDifference ||
        frame.lanes.size() < 3 || frame.pointCount == 0) {
        return {};
    }
    std::vector<std::span<const float>> tracks;
    bool related = false;
    for (std::size_t index = 0; index + 1 < frame.lanes.size(); ++index) {
        tracks.push_back(frame.lanes[index]);
        related = related || (index < frame.related.size() && frame.related[index]);
    }
    if (tracks.size() < 2 || !related) {
        return {};
    }
    const std::vector<double> levelSum = cancellationLevels(frame, frame.lanes.back());
    std::vector<double> louder(frame.pointCount, 0.0);
    for (const auto& track : tracks) {
        const std::vector<double> levels = cancellationLevels(frame, track);
        for (std::size_t point = 0; point < louder.size(); ++point) {
            // NaN propagates as the page's Math.max does.
            louder[point] = std::isnan(levels[point]) || std::isnan(louder[point])
                                ? std::numeric_limits<double>::quiet_NaN()
                                : std::max(louder[point], levels[point]);
        }
    }
    double loudest = 0.0;
    for (const double level : louder) {
        if (std::isfinite(level)) {
            loudest = std::max(loudest, level);
        }
    }
    const double audible = protocol::kCancellationQuietShare * loudest;
    const double ratio = std::pow(10.0, protocol::kCancellationDb / 20.0);
    std::vector<bool> cancelling(louder.size());
    for (std::size_t point = 0; point < louder.size(); ++point) {
        const bool measured = std::isfinite(louder[point]) && std::isfinite(levelSum[point]);
        cancelling[point] =
            measured && louder[point] > audible && levelSum[point] < ratio * louder[point];
    }
    // Short gaps inside a cancelling stretch belong to it.
    for (std::size_t point = 1; point < cancelling.size(); ++point) {
        if (cancelling[point] || !cancelling[point - 1]) {
            continue;
        }
        std::size_t resume = point;
        while (resume < cancelling.size() && !cancelling[resume]) {
            ++resume;
        }
        // Of columns, or of samples, whatever the points drawn between them.
        if (resume < cancelling.size() &&
            resume - point <= protocol::kCancellationBridgedPoints * frame.pointsPerSample) {
            std::fill(cancelling.begin() + static_cast<std::ptrdiff_t>(point),
                      cancelling.begin() + static_cast<std::ptrdiff_t>(resume), true);
        }
    }
    const std::size_t count = frame.lanes.size();
    const Lane lane = traceLanes(count, layout, true).back();
    const auto position = [&](std::size_t point) {
        return frame.mode == protocol::FrameMode::kMinMax
                   ? columnPosition(frame, static_cast<double>(point))
                   : pointPosition(frame, static_cast<double>(point));
    };
    std::vector<Band> out;
    std::optional<std::size_t> start;
    for (std::size_t point = 0; point <= cancelling.size(); ++point) {
        const bool here = point < cancelling.size() && cancelling[point];
        if (here && !start) {
            start = point;
        } else if (!here && start) {
            out.push_back({.left = screenX(position(*start), view),
                           .right = screenX(position(point), view),
                           .bottom = lane.center - lane.halfHeight,
                           .top = lane.center + lane.halfHeight});
            start.reset();
        }
    }
    return out;
}

namespace {

double dbFraction(double db)
{
    return std::pow(10.0, -db / 20.0);
}

double levelFraction(int step)
{
    return dbFraction(protocol::kGridLevelStepDb * step);
}

// The steps of the first stride whose lines all stay `minimumGap` apart, else the top only, else
// none.
std::vector<int> strideSteps(double halfLane, double minimumGap, std::initializer_list<int> strides)
{
    const int below = static_cast<int>(protocol::kGridLevelsBelowTop);
    for (const int stride : strides) {
        std::vector<int> steps;
        for (int step = 0; step <= below; step += stride) {
            steps.push_back(step);
        }
        const bool fits = std::ranges::all_of(steps, [&](int step) {
            const double next = step + stride <= below ? levelFraction(step + stride) : 0.0;
            return (levelFraction(step) - next) * halfLane >= minimumGap;
        });
        if (fits) {
            return steps;
        }
    }
    return halfLane >= minimumGap ? std::vector<int>{0} : std::vector<int>{};
}

} // namespace

std::vector<int> levelSteps(double halfLane, double minimumGap)
{
    return strideSteps(halfLane, minimumGap,
                       {1, static_cast<int>(protocol::kGridCoarseLevelStride)});
}

std::vector<LevelLine> levelLines(double halfLane, protocol::GridDensity density)
{
    constexpr double kStepDb = protocol::kGridLevelStepDb;
    std::vector<LevelLine> lines;
    if (density == protocol::GridDensity::kCoarse) {
        for (const int step : strideSteps(halfLane, protocol::kGridCoarseLineGap,
                                          {static_cast<int>(protocol::kGridCoarseLevelStride)})) {
            lines.push_back({.step = step * kStepDb});
        }
        return lines;
    }
    const std::vector<int> steps = levelSteps(halfLane, protocol::kGridMediumLineGap);
    if (density == protocol::GridDensity::kMedium) {
        for (const int step : steps) {
            lines.push_back({.step = step * kStepDb});
        }
        return lines;
    }
    const auto below = static_cast<int>(protocol::kGridLevelsBelowTop);
    const auto subdivisions = static_cast<int>(protocol::kGridFineLevelSubdivisions);
    const double minor = kStepDb / subdivisions;
    const bool everyStep = steps.size() == static_cast<std::size_t>(below) + 1;
    for (const int step : steps) {
        const double top = step * kStepDb;
        lines.push_back({.step = top, .strong = true});
        if (!everyStep || step >= below) {
            continue;
        }
        bool fits = true;
        for (int edge = 0; edge < subdivisions; ++edge) {
            const double gap =
                dbFraction(top + edge * minor) - dbFraction(top + (edge + 1) * minor);
            fits = fits && gap * halfLane >= protocol::kGridFineLineGap;
        }
        if (fits) {
            for (int inside = 1; inside < subdivisions; ++inside) {
                lines.push_back({.step = top + inside * minor});
            }
        }
    }
    return lines;
}

std::size_t gridColumns(protocol::GridDensity time)
{
    switch (time) {
    case protocol::GridDensity::kCoarse:
        return protocol::kGridColumnsCoarse;
    case protocol::GridDensity::kFine:
        return protocol::kGridColumnsFine;
    case protocol::GridDensity::kMedium:
        break;
    }
    return protocol::kGridColumnsMedium;
}

GridLines gridLines(double width, double height, std::size_t laneCount, double pixelRatio,
                    const GridOptions& options)
{
    GridLines lines;
    lines.lineWidth = std::max(1.0, std::round(pixelRatio));
    // JavaScript's Math.round: halves round up.
    const auto round = [](double value) { return std::floor(value + 0.5); };
    std::vector<VerticalLine> divisions;
    if (options.vertical != nullptr) {
        divisions = *options.vertical;
    } else {
        for (std::size_t index = 1; index < options.columns; ++index) {
            divisions.push_back(
                {.position = static_cast<double>(index) / static_cast<double>(options.columns)});
        }
    }
    const double offset = std::fmod(lines.lineWidth, 2.0) / 2.0;
    const auto xOf = [&](double position) { return round(width * position) + offset; };
    const std::vector<Lane> laneList =
        screenLanes(laneCount, protocol::ViewLayout::kStacked, false);
    const auto rowY = [&](const Lane& lane, double fraction) {
        return round((1.0 - (lane.center + fraction * lane.halfHeight)) / 2.0 * height) + offset;
    };
    // Each lane's level lines, both signs, top lane first.
    std::vector<GridLines::Horizontal> levelRows;
    for (const Lane& lane : laneList) {
        const double halfLaneCss = lane.halfHeight * height / 2.0 / std::max(1.0, pixelRatio);
        for (const LevelLine& level : levelLines(halfLaneCss, options.level)) {
            for (const double sign : {1.0, -1.0}) {
                levelRows.push_back(
                    {.y = rowY(lane, sign * dbFraction(level.step)), .axis = level.strong});
            }
        }
    }
    if (options.type == protocol::GridType::kGrid) {
        for (const VerticalLine& line : divisions) {
            lines.vertical.push_back({.x = xOf(line.position), .strong = line.strong});
        }
        lines.horizontal = levelRows;
    }
    for (const Lane& lane : laneList) {
        lines.horizontal.push_back({.y = rowY(lane, 0.0), .axis = true});
    }
    return lines;
}

namespace {

constexpr double kFinestBeatStep = 1.0 / 1024.0;

// Beats' grid levels, in quarter notes, longest first, a level as long as another merged with it
// (the page's beatLadder).
std::vector<double> beatLadder(double barQuarterNotes, protocol::BeatDivision division)
{
    constexpr double kEpsilon = protocol::kMusicalTolerance;
    std::vector<double> levels{16.0 * barQuarterNotes, 4.0 * barQuarterNotes, barQuarterNotes, 1.0};
    for (double level = division == protocol::BeatDivision::kTriplets ? 1.0 / 3.0 : 0.25;;
         level /= 2.0) {
        levels.push_back(level);
        if (level <= kFinestBeatStep) {
            break;
        }
    }
    std::ranges::sort(levels, std::greater<>());
    std::vector<double> merged;
    for (const double level : levels) {
        if (merged.empty() || merged.back() - level > kEpsilon * level) {
            merged.push_back(level);
        }
    }
    return merged;
}

struct BeatSteps
{
    double step = 1.0;
    double strongEvery = 4.0;
};

// The step and the strong lines' step (the page's beatSteps).
BeatSteps beatSteps(double visibleSpan, double barQuarterNotes, const BeatGrid& grid)
{
    constexpr double kEpsilon = protocol::kMusicalTolerance;
    const std::vector<double> ladder = beatLadder(barQuarterNotes, grid.division);
    const bool triplets = grid.division == protocol::BeatDivision::kTriplets;
    const auto coarser = [&](double length) {
        // The shortest level longer than `length`: the ladder runs longest first.
        for (const double level : std::views::reverse(ladder)) {
            if (level - length > kEpsilon * length) {
                return level;
            }
        }
        return 4.0 * length;
    };
    const auto finer = [&](double length) -> std::optional<double> {
        for (const double level : ladder) {
            if (length - level > kEpsilon * length) {
                return level;
            }
        }
        return std::nullopt;
    };
    const bool longView = visibleSpan >= 8.0 * barQuarterNotes;
    double step = longView                         ? barQuarterNotes
                  : visibleSpan >= barQuarterNotes ? 1.0
                  : triplets                       ? 1.0 / 3.0
                                                   : 0.25;
    while (visibleSpan < 4.0 * step && step > kFinestBeatStep) {
        step = triplets && std::abs(step - 1.0) < kEpsilon ? 1.0 / 3.0 : step / 2.0;
    }
    const double strongEvery = triplets && step < 1.0 - kEpsilon ? coarser(step)
                               : longView                        ? 4.0 * barQuarterNotes
                               : visibleSpan >= barQuarterNotes  ? barQuarterNotes
                               : step < 0.25                     ? 0.25
                                                                 : 1.0;
    if (grid.time == protocol::GridDensity::kCoarse) {
        const double coarse = coarser(step);
        return {.step = coarse, .strongEvery = coarser(coarse)};
    }
    if (grid.time == protocol::GridDensity::kFine) {
        const auto fine = finer(step);
        if (fine && *fine / visibleSpan * grid.widthCss >= protocol::kGridFineBeatLineGap) {
            return {.step = *fine, .strongEvery = coarser(*fine)};
        }
    }
    return {.step = step, .strongEvery = strongEvery};
}

} // namespace

std::vector<VerticalLine> musicalGridLines(double cycleStart, double span, double barQuarterNotes,
                                           ViewWindow view, const BeatGrid& grid)
{
    if (span <= 0.0 || barQuarterNotes <= 0.0) {
        return {};
    }
    constexpr double kEpsilon = protocol::kMusicalTolerance;
    const double visibleStart = cycleStart + view.start * span;
    const double visibleSpan = (view.end - view.start) * span;
    const auto [step, strongEvery] = beatSteps(visibleSpan, barQuarterNotes, grid);
    const auto isMultiple = [&](double value, double of) {
        return std::abs(value / of - std::round(value / of)) < kEpsilon;
    };
    std::vector<VerticalLine> lines;
    for (auto index = static_cast<long long>(std::floor(visibleStart / step)) + 1;
         static_cast<double>(index) * step < visibleStart + visibleSpan - kEpsilon; ++index) {
        const double quarterNotes = static_cast<double>(index) * step;
        if (quarterNotes > visibleStart + kEpsilon) {
            lines.push_back({.position = (quarterNotes - visibleStart) / visibleSpan,
                             .strong = isMultiple(quarterNotes, strongEvery)});
        }
    }
    return lines;
}

PixelBand playheadBand(double position, double width, double pixelRatio)
{
    const double lineWidth = std::max(1.0, std::floor(pixelRatio));
    const double left = position * width - lineWidth / 2.0;
    return {.left = left, .right = left + lineWidth};
}

double bandCoverage(double pixel, PixelBand band)
{
    return std::clamp(std::min(pixel + 1.0, band.right) - std::max(pixel, band.left), 0.0, 1.0);
}

} // namespace orizon::editor
