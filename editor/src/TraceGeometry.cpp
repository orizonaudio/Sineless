#include "editor/TraceGeometry.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>

namespace orizon::editor {

std::vector<Lane> lanes(std::size_t count, protocol::ViewLayout layout)
{
    if (layout == protocol::ViewLayout::kOverlaid || count <= 1) {
        return std::vector<Lane>(count, Lane{});
    }
    const double halfHeight = 1.0 / static_cast<double>(count);
    std::vector<Lane> out;
    out.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        out.push_back({.center = 1.0 - halfHeight * static_cast<double>(2 * index + 1),
                       .halfHeight = halfHeight});
    }
    return out;
}

double columnPosition(const FrameView& frame, double column)
{
    return frame.viewStart + column / static_cast<double>(frame.pointCount) *
                                 static_cast<double>(frame.viewEnd - frame.viewStart);
}

double pointPosition(const FrameView& frame, double index)
{
    const double samples =
        frame.visibleSamples > 0.0f ? frame.visibleSamples : static_cast<double>(frame.pointCount);
    return frame.viewStart + (index - frame.startFraction) / samples *
                                 static_cast<double>(frame.viewEnd - frame.viewStart);
}

double screenX(double position, ViewWindow view)
{
    return -1.0 + 2.0 * (position - view.start) / (view.end - view.start);
}

namespace {

// Each point's peak, NaN where a value is not finite; raw points over a short window.
std::vector<double> peaks(const FrameView& frame, std::span<const float> values)
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
    std::vector<double> smoothed(count);
    for (std::size_t point = 0; point < count; ++point) {
        double peak = 0.0;
        const std::size_t from = point >= reach ? point - reach : 0;
        const std::size_t to = std::min(count - 1, point + reach);
        for (std::size_t near = from; near <= to; ++near) {
            if (std::isfinite(out[near])) {
                peak = std::max(peak, out[near]);
            }
        }
        smoothed[point] =
            std::isfinite(out[point]) ? peak : std::numeric_limits<double>::quiet_NaN();
    }
    return smoothed;
}

} // namespace

std::vector<Band> cancellationBands(const FrameView& frame, protocol::ViewLayout layout,
                                    const std::vector<bool>& compared, ViewWindow view)
{
    if (!frame.compared() || frame.lanes.size() < 3 || frame.pointCount == 0) {
        return {};
    }
    std::vector<std::span<const float>> tracks;
    bool related = false;
    for (std::size_t index = 0; index + 1 < frame.lanes.size(); ++index) {
        if (index < compared.size() && compared[index]) {
            tracks.push_back(frame.lanes[index]);
            related = related || (index < frame.related.size() && frame.related[index]);
        }
    }
    if (tracks.size() < 2 || !related) {
        return {};
    }
    const std::vector<double> levelSum = peaks(frame, frame.lanes.back());
    std::vector<double> louder(frame.pointCount, 0.0);
    for (const auto& track : tracks) {
        const std::vector<double> levels = peaks(frame, track);
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
        if (resume < cancelling.size() && resume - point <= protocol::kCancellationBridgedPoints) {
            std::fill(cancelling.begin() + static_cast<std::ptrdiff_t>(point),
                      cancelling.begin() + static_cast<std::ptrdiff_t>(resume), true);
        }
    }
    const std::size_t count = frame.lanes.size();
    const Lane lane = lanes(count, layout).back();
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
    const std::vector<VerticalLine>* vertical = options.vertical;
    GridLines lines;
    lines.lineWidth = std::max(1.0, std::round(pixelRatio));
    // JavaScript's Math.round: halves round up.
    const auto round = [](double value) { return std::floor(value + 0.5); };
    std::vector<VerticalLine> divisions;
    if (vertical == nullptr) {
        for (std::size_t index = 1; index < options.columns; ++index) {
            divisions.push_back(
                {.position = static_cast<double>(index) / static_cast<double>(options.columns)});
        }
        vertical = &divisions;
    }
    for (const VerticalLine& line : *vertical) {
        lines.vertical.push_back({.x = round(width * line.position) + 0.5, .strong = line.strong});
    }
    const std::vector<Lane> laneList = lanes(laneCount, protocol::ViewLayout::kStacked);
    const auto rowY = [&](const Lane& lane, double fraction) {
        return round((1.0 - (lane.center + fraction * lane.halfHeight)) / 2.0 * height) + 0.5;
    };
    for (const Lane& lane : laneList) {
        const double halfLaneCss = lane.halfHeight * height / 2.0 / std::max(1.0, pixelRatio);
        for (const LevelLine& level : levelLines(halfLaneCss, options.level)) {
            for (const double sign : {1.0, -1.0}) {
                lines.horizontal.push_back(
                    {.y = rowY(lane, sign * dbFraction(level.step)), .axis = level.strong});
            }
        }
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
        for (auto level = ladder.rbegin(); level != ladder.rend(); ++level) {
            if (*level - length > kEpsilon * length) {
                return *level;
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
