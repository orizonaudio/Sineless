#include "engine/Reducer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace orizon::engine {

using protocol::FrameMode;

namespace {

// The most samples read at once when reducing again: bounds the working space.
constexpr std::size_t kChunkSamples = 16384;

// Whether `samples` hold one that is not finite (NaN or infinity): its bits without the sign at
// or above an infinity's. Found per lane, as extremesOf does, so that the compiler can run the
// lanes as vector registers: an integer maximum, with no branch.
bool holdsNonFinite(std::span<const float> samples)
{
    constexpr std::size_t kLanes = 8;
    constexpr std::uint32_t kMagnitude = 0x7fffffffU;
    constexpr std::uint32_t kInfinity = 0x7f800000U;
    std::array<std::uint32_t, kLanes> largest{};
    std::size_t index = 0;
    for (; index + kLanes <= samples.size(); index += kLanes) {
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            largest[lane] = std::max(
                largest[lane], std::bit_cast<std::uint32_t>(samples[index + lane]) & kMagnitude);
        }
    }
    std::uint32_t magnitude = *std::ranges::max_element(largest);
    for (; index < samples.size(); ++index) {
        magnitude = std::max(magnitude, std::bit_cast<std::uint32_t>(samples[index]) & kMagnitude);
    }
    return magnitude >= kInfinity;
}

} // namespace

void ColumnPeaks::wave(std::span<const float> samples, std::size_t n,
                       std::span<const std::size_t> seams)
{
    // around[k] holds samples[n - kColumnReach + k] less samples[n], NaN where `samples` does not
    // hold it: taken from the extreme sample, so that a constant stays exactly the constant (the
    // filter's weights, rounded to float, sum to 1 only within about 1e-7).
    reference = samples[n];
    if (n >= kColumnReach && n + kColumnReach < samples.size()) {
        const float* from = samples.data() + (n - kColumnReach);
        for (std::size_t k = 0; k < kSpan; ++k) {
            around[k] = from[k] - reference;
        }
    } else {
        for (std::size_t k = 0; k < kSpan; ++k) {
            const std::size_t at = n + k;
            around[k] = at >= kColumnReach && at - kColumnReach < samples.size()
                            ? samples[at - kColumnReach] - reference
                            : std::numeric_limits<float>::quiet_NaN();
        }
    }
    // Every join within `around`, as indices into it.
    aroundSeams.clear();
    for (const std::size_t seam : seams) {
        if (seam != kNoSeam && seam + kColumnReach >= n && seam + kColumnReach - n < kSpan) {
            aroundSeams.push_back(seam + kColumnReach - n);
        }
    }
    // The four gaps n - 2 … n + 1 (around[5 + g], g = 0 … 3, to the next): each the wave where
    // Reconstruction draws it so, else straight between its samples (or not finite beside one that
    // is not). Only the points the step reads, 3 to 13, each a sum in the order Reconstruction
    // takes it, the eight side by side so that none waits for another; a gap drawn straight ignores
    // its sums, taken over zeros where a neighbour is not finite.
    constexpr std::size_t kFirstGap = kColumnReach - 2;
    bool allFinite = true;
    for (std::size_t k = 0; k < kSpan; ++k) {
        const bool finite = std::isfinite(around[k]);
        allFinite = allFinite && finite;
        finiteAround[k] = finite ? around[k] : 0.0f;
    }
    // Every neighbour held, finite and on one side of any join: Reconstruction's rule draws all
    // four gaps as the wave, which is most columns; otherwise it is asked gap by gap.
    for (std::size_t g = 0; g < 4; ++g) {
        reconstructed[g] = (allFinite && aroundSeams.empty()) ||
                                   filter.reconstructsGap(around, kFirstGap + g, aroundSeams)
                               ? 1
                               : 0;
    }
    if (phaseWeights[0] == nullptr) {
        for (std::size_t phase = 0; phase < kColumnOversampling; ++phase) {
            phaseWeights.at(phase) = filter.weights(kColumnOversampling, phase).data();
        }
    }
    constexpr std::size_t kTaps = 2 * kColumnFilterReach;
    constexpr std::array<std::size_t, 8> kPoint{3, 5, 6, 7, 9, 10, 11, 13};
    std::array<const float*, 8> weightsOf{};
    std::array<const float*, 8> from{};
    for (std::size_t j = 0; j < kPoint.size(); ++j) {
        // Gap g reads samples n - 7 + g … n + 4 + g.
        weightsOf[j] = phaseWeights.at(kPoint[j] % kColumnOversampling);
        from[j] = finiteAround.data() + (kPoint[j] / kColumnOversampling);
    }
    std::array<float, 8> sums{};
    for (std::size_t k = 0; k < kTaps; ++k) {
        for (std::size_t j = 0; j < kPoint.size(); ++j) {
            sums[j] += weightsOf[j][k] * from[j][k];
        }
    }
    for (std::size_t j = 0; j < kPoint.size(); ++j) {
        const std::size_t gap = kPoint[j] / kColumnOversampling;
        const float here = around[kFirstGap + gap];
        const float next = around[kFirstGap + gap + 1];
        float value = std::numeric_limits<float>::quiet_NaN();
        if (reconstructed[gap] != 0) {
            value = sums[j];
        } else if (std::isfinite(here) && std::isfinite(next)) {
            const float fraction = static_cast<float>(kPoint[j] % kColumnOversampling) /
                                   static_cast<float>(kColumnOversampling);
            value = here + ((next - here) * fraction);
        }
        points[kPoint[j]] = value + reference;
    }
    // The samples themselves exactly, as the raw view keeps them: n - 2 … n + 2.
    for (std::size_t k = 0; k <= 4; ++k) {
        const std::size_t at = n + k;
        points[k * kColumnOversampling] = at >= 2 && at - 2 < samples.size()
                                              ? samples[at - 2]
                                              : std::numeric_limits<float>::quiet_NaN();
    }
}

void ColumnPeaks::widenAround(bool highest, float& value) const
{
    // Points 4 … 12 are n - 1 … n + 1; 3 and 13 are only neighbours of a vertex. A point's gaps:
    // a sample point (every fourth) lies between two, any other within one.
    const auto gapsReconstructed = [&](std::size_t i) {
        const std::size_t gap = i / kColumnOversampling;
        return i % kColumnOversampling != 0
                   ? reconstructed[gap] != 0
                   : reconstructed[gap - 1] != 0 && reconstructed[gap] != 0;
    };
    // Only a local extreme of the wave can be a crest: a point beyond both its neighbours (or
    // level with one), and, in reconstructed gaps, the vertex of the parabola through the three,
    // which lies within half a quarter sample of it and beyond it. A column on a slope or a flat
    // top keeps its samples' extremes; one that holds a crest takes its height.
    const float sign = highest ? 1.0f : -1.0f;
    for (std::size_t i = kColumnOversampling; i <= 3 * kColumnOversampling; ++i) {
        const float b = sign * points[i];
        const float a = sign * points[i - 1];
        const float c = sign * points[i + 1];
        if (!std::isfinite(b) || !(b >= a || std::isnan(a)) || !(b >= c || std::isnan(c))) {
            continue;
        }
        value = highest ? std::max(value, points[i]) : std::min(value, points[i]);
        if (a + c < 2.0f * b && gapsReconstructed(i)) {
            const float vertex = b + ((c - a) * (c - a) / (8.0f * (2.0f * b - a - c)));
            value = highest ? std::max(value, vertex) : std::min(value, -vertex);
        }
    }
}

void ColumnPeaks::widen(std::span<const float> samples, std::size_t lowest, std::size_t highest,
                        std::span<const std::size_t> seams, float& minimum, float& maximum)
{
    wave(samples, highest, seams);
    widenAround(true, maximum);
    if (lowest != highest) {
        wave(samples, lowest, seams);
    }
    widenAround(false, minimum);
}

FrameMode reduceToColumns(std::span<const float> samples, std::size_t columns,
                          std::vector<float>& output)
{
    return reduceToColumns(samples, 0, 0, columns, output);
}

Extremes extremesOf(std::span<const float> samples)
{
    // The values per lane first, so that the compiler can run the lanes as vector registers
    // (std::minmax_element must find the first minimum and the last maximum, which keeps it a
    // scalar loop: several times slower over long columns); then the first index of each, which
    // a sample equal to it (+0 and -0 alike) gives. A sample that is not a number is left out
    // (std::min and std::max keep their first argument when the other is NaN), as BlockStats
    // leaves it out of levels; a column of nothing else has no extremes.
    constexpr std::size_t kLanes = 8;
    std::array<float, kLanes> low{};
    std::array<float, kLanes> high{};
    low.fill(std::numeric_limits<float>::infinity());
    high.fill(-std::numeric_limits<float>::infinity());
    std::size_t index = 0;
    for (; index + kLanes <= samples.size(); index += kLanes) {
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            low[lane] = std::min(low[lane], samples[index + lane]);
            high[lane] = std::max(high[lane], samples[index + lane]);
        }
    }
    float minimum = *std::ranges::min_element(low);
    float maximum = *std::ranges::max_element(high);
    for (; index < samples.size(); ++index) {
        minimum = std::min(minimum, samples[index]);
        maximum = std::max(maximum, samples[index]);
    }
    Extremes found;
    if (minimum > maximum) {
        return found;
    }
    const auto first = [&](float value) {
        return static_cast<std::size_t>(std::ranges::find(samples, value) - samples.begin());
    };
    found.highest = first(maximum);
    found.lowest = first(minimum);
    found.maximum = samples[found.highest];
    found.minimum = samples[found.lowest];
    return found;
}

FrameMode reduceToColumns(std::span<const float> withNeighbours, std::size_t lead,
                          std::size_t trail, std::size_t columns, std::vector<float>& output,
                          std::size_t seam, ColumnPeaks* peaks)
{
    lead = std::min(lead, withNeighbours.size());
    trail = std::min(trail, withNeighbours.size() - lead);
    if (withNeighbours.size() - lead - trail <= columns || columns == 0) {
        output.assign(withNeighbours.begin(), withNeighbours.end());
        return FrameMode::kRaw;
    }
    ColumnPeaks own;
    ColumnPeaks& wave = peaks != nullptr ? *peaks : own;
    const std::size_t count = withNeighbours.size() - lead - trail;
    output.resize(columns * 2);
    for (std::size_t column = 0; column < columns; ++column) {
        // 64-bit products: count * columns stays far below 2^64 for any real span and width.
        const std::size_t begin =
            lead + static_cast<std::size_t>(std::uint64_t{column} * count / columns);
        const std::size_t end =
            lead + static_cast<std::size_t>(std::uint64_t{column + 1} * count / columns);
        Extremes found = extremesOf(withNeighbours.subspan(begin, end - begin));
        if (found.highest != Extremes::kNone) {
            wave.widen(withNeighbours, begin + found.lowest, begin + found.highest,
                       std::span(&seam, 1), found.minimum, found.maximum);
        }
        output[column * 2] = found.minimum;
        output[column * 2 + 1] = found.maximum;
    }
    return FrameMode::kMinMax;
}

void ColumnReduction::reset(std::size_t count, std::size_t columns, bool trackInvalid,
                            std::size_t lead, std::size_t trail)
{
    spanCount = count;
    columnCount = columns;
    leadCount = std::min(lead, count);
    trailCount = std::min(trail, count - leadCount);
    output.assign(mode() == FrameMode::kRaw ? count : columns * 2, 0.0f);
    tracking = trackInvalid;
    // Raw samples keep what is not finite in `output`; only min/max columns need flags.
    invalid.assign(tracking && mode() == FrameMode::kMinMax ? columns : 0, 0);
    invalidColumns = 0;
    valid = true;
}

bool ColumnReduction::holdsInvalid() const
{
    if (!valid || !tracking) {
        return false;
    }
    if (mode() == FrameMode::kRaw) {
        return holdsNonFinite(output);
    }
    return invalidColumns > 0;
}

std::size_t ColumnReduction::columnBegin(std::size_t column) const
{
    return leadCount + static_cast<std::size_t>(std::uint64_t{column} * shownCount() / columnCount);
}

bool ColumnReduction::update(std::size_t first, std::size_t end, const Reader& read,
                             std::size_t seam)
{
    return update(first, end, read, std::span(&seam, 1));
}

bool ColumnReduction::update(std::size_t first, std::size_t end, const Reader& read,
                             std::span<const std::size_t> seams)
{
    end = std::min(end, spanCount);
    if (!valid || first >= end) {
        return valid;
    }
    if (mode() == FrameMode::kRaw) {
        if (!read(first, std::span(output).subspan(first, end - first))) {
            valid = false;
        }
        return valid;
    }
    // A changed sample changes the wave read by every column within kColumnReach of it. Only the
    // shown samples are reduced; neighbours read with them are not.
    first = std::max(first >= kColumnReach ? first - kColumnReach : 0, leadCount);
    end = std::min(end + kColumnReach, spanCount);
    end = std::min(end, spanCount - trailCount);
    if (first >= end) {
        return true;
    }
    // Column c holds samples [begin(c), begin(c + 1)); shown sample s lies in column
    // ceil((s + 1) * columns / shown) - 1.
    const std::size_t shown = shownCount();
    const auto columnOf = [&](std::size_t sample) {
        return static_cast<std::size_t>(
            (std::uint64_t{sample - leadCount + 1} * columnCount + shown - 1) / shown - 1);
    };
    const std::size_t lastColumn = columnOf(end - 1);
    // In stretches of whole columns, so the working space stays small however long the span.
    for (std::size_t column = columnOf(first); column <= lastColumn;) {
        const std::size_t from = columnBegin(column);
        std::size_t stop = column + 1;
        while (stop <= lastColumn && columnBegin(stop + 1) - from <= kChunkSamples) {
            ++stop;
        }
        // The stretch's own samples with kColumnReach neighbours each side within the span (the
        // span holds them all: its margins are the neighbours), in one read.
        const std::size_t to = columnBegin(stop);
        const std::size_t before = std::min(from, kColumnReach);
        const std::size_t after = std::min(spanCount - to, kColumnReach);
        scratch.resize(before + (to - from) + after);
        if (!read(from - before, scratch)) {
            valid = false;
            return false;
        }
        const std::size_t start = from - before; // The span's index of scratch[0].
        seamsHere.clear();
        for (const std::size_t seam : seams) {
            if (seam != kNoSeam && seam >= start) {
                seamsHere.push_back(seam - start);
            }
        }
        for (; column < stop; ++column) {
            const std::size_t begin = columnBegin(column) - start;
            const std::size_t length = columnBegin(column + 1) - columnBegin(column);
            const auto samples = std::span<const float>(scratch).subspan(begin, length);
            Extremes found = extremesOf(samples);
            if (found.highest != Extremes::kNone) {
                peaks.widen(scratch, begin + found.lowest, begin + found.highest, seamsHere,
                            found.minimum, found.maximum);
            }
            output[column * 2] = found.minimum;
            output[column * 2 + 1] = found.maximum;
            if (tracking) {
                const std::uint8_t holds = holdsNonFinite(samples) ? 1 : 0;
                invalidColumns = invalidColumns - invalid[column] + holds;
                invalid[column] = holds;
            }
        }
    }
    return true;
}

std::span<const float> RollColumns::update(std::uint64_t origin, std::uint64_t firstColumn,
                                           const ColumnGrid& grid, const Reader& read)
{
    const auto columns = static_cast<std::size_t>(grid.columns);
    // Keep the columns still on screen, moved to their new places, if they are the same columns.
    std::size_t kept = 0;
    if (origin == gridOrigin && grid.spanSamples == columnGrid.spanSamples &&
        grid.columns == columnGrid.columns && firstColumn >= firstColumnShown) {
        const std::uint64_t shift = firstColumn - firstColumnShown;
        if (shift < readyColumns) {
            kept = readyColumns - static_cast<std::size_t>(shift);
            std::copy(output.begin() + static_cast<std::ptrdiff_t>(shift * 2),
                      output.begin() + static_cast<std::ptrdiff_t>(readyColumns * 2),
                      output.begin());
            std::copy(invalid.begin() + static_cast<std::ptrdiff_t>(shift),
                      invalid.begin() + static_cast<std::ptrdiff_t>(readyColumns), invalid.begin());
        }
    }
    gridOrigin = origin;
    firstColumnShown = firstColumn;
    columnGrid = grid;
    output.resize(columns * 2);
    invalid.resize(columns);
    readyColumns = kept;
    drawnColumns = kept;
    if (kept == columns) {
        return output;
    }

    // The rest in stretches of whole columns; columns past the end of the source stay silent and
    // unready. Each stretch reads kColumnReach neighbours each side apart from its own samples: a
    // missing lead (the source's start) leaves those gaps straight for good, a missing trail (the
    // newest audio) leaves the columns that read it drawn but not ready, to be reduced again.
    const auto begin = [&](std::size_t column) { return grid.boundary(firstColumn + column); };
    std::size_t column = kept;
    std::size_t ready = columns;
    while (column < columns) {
        std::size_t wanted = 1;
        while (column + wanted < columns &&
               begin(column + wanted + 1) - begin(column) <= kChunkSamples) {
            ++wanted;
        }
        std::size_t available = wanted;
        const std::uint64_t from = origin + begin(column);
        const auto before = static_cast<std::size_t>(std::min<std::uint64_t>(from, kColumnReach));
        while (available > 0) {
            scratch.resize(before +
                           static_cast<std::size_t>(begin(column + available) - begin(column)) +
                           kColumnReach);
            if (read(from,
                     std::span(scratch).subspan(before, scratch.size() - before - kColumnReach))) {
                break;
            }
            available /= 2; // The newest audio may not be there yet: try a shorter stretch.
        }
        if (available == 0) {
            break;
        }
        const std::span<float> all(scratch);
        if (before > 0 && !read(from - before, all.first(before))) {
            std::ranges::fill(all.first(before), std::numeric_limits<float>::quiet_NaN());
        }
        const std::uint64_t to = origin + begin(column + available);
        const bool trailHeld = read(to, all.last(kColumnReach));
        if (!trailHeld) {
            std::ranges::fill(all.last(kColumnReach), std::numeric_limits<float>::quiet_NaN());
        }
        for (std::size_t index = 0; index < available; ++index) {
            const auto offset =
                before + static_cast<std::size_t>(begin(column + index) - begin(column));
            const auto length =
                static_cast<std::size_t>(begin(column + index + 1) - begin(column + index));
            const std::span<const float> samples =
                std::span<const float>(scratch).subspan(offset, length);
            Extremes found = extremesOf(samples);
            if (found.highest != Extremes::kNone) {
                peaks.widen(scratch, offset + found.lowest, offset + found.highest, {},
                            found.minimum, found.maximum);
            }
            output[(column + index) * 2] = found.minimum;
            output[(column + index) * 2 + 1] = found.maximum;
            invalid[column + index] =
                std::ranges::all_of(samples, [](float sample) { return std::isfinite(sample); })
                    ? 0
                    : 1;
            // It reads up to sample e + kColumnReach - 1, e its end: not held when at or past `to`.
            if (!trailHeld && ready == columns &&
                origin + begin(column + index + 1) + kColumnReach - 1 >= to) {
                ready = column + index;
            }
        }
        column += available;
        if (available < wanted || !trailHeld) {
            break;
        }
    }
    readyColumns = std::min(column, ready);
    drawnColumns = column;
    std::fill(output.begin() + static_cast<std::ptrdiff_t>(column * 2), output.end(), 0.0f);
    std::fill(invalid.begin() + static_cast<std::ptrdiff_t>(column), invalid.end(), 0);
    return output;
}

bool RollColumns::holdsInvalid() const
{
    return std::any_of(invalid.begin(),
                       invalid.begin() +
                           static_cast<std::ptrdiff_t>(std::min(drawnColumns, invalid.size())),
                       [](std::uint8_t column) { return column != 0; });
}

} // namespace orizon::engine
