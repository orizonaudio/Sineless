#include "engine/Reducer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace orizon::engine {

using protocol::FrameMode;

namespace {

// The most samples read at once when reducing again: bounds the working space.
constexpr std::size_t kChunkSamples = 16384;

// The smallest and largest of `samples` (not empty), found per lane first so that the compiler
// can run the lanes as vector registers. std::minmax_element must find the first minimum and the
// last maximum, which keeps it a scalar loop: several times slower over long columns. A sample
// that is not a number is left out (std::min and std::max keep their first argument when the
// other is NaN), as BlockStats leaves it out of levels; a column of nothing else is NaN.
std::pair<float, float> minMax(std::span<const float> samples)
{
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
    if (minimum > maximum) {
        constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
        return {kNaN, kNaN};
    }
    return {minimum, maximum};
}

} // namespace

FrameMode reduceToColumns(std::span<const float> samples, std::size_t columns,
                          std::vector<float>& output)
{
    const std::size_t count = samples.size();
    if (count <= columns || columns == 0) {
        output.assign(samples.begin(), samples.end());
        return FrameMode::kRaw;
    }

    output.resize(columns * 2);
    for (std::size_t column = 0; column < columns; ++column) {
        // 64-bit products: count * columns stays far below 2^64 for any real span and width.
        const auto begin = static_cast<std::size_t>(std::uint64_t{column} * count / columns);
        const auto end = static_cast<std::size_t>(std::uint64_t{column + 1} * count / columns);
        const auto [minimum, maximum] = minMax(samples.subspan(begin, end - begin));
        output[column * 2] = minimum;
        output[column * 2 + 1] = maximum;
    }
    return FrameMode::kMinMax;
}

void ColumnReduction::reset(std::size_t count, std::size_t columns)
{
    spanCount = count;
    columnCount = columns;
    output.assign(mode() == FrameMode::kRaw ? count : columns * 2, 0.0f);
    valid = true;
}

std::size_t ColumnReduction::columnBegin(std::size_t column) const
{
    return static_cast<std::size_t>(std::uint64_t{column} * spanCount / columnCount);
}

bool ColumnReduction::update(std::size_t first, std::size_t end, const Reader& read)
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
    // Column c holds samples [begin(c), begin(c + 1)); sample s lies in column
    // ceil((s + 1) * columns / count) - 1.
    const auto columnOf = [&](std::size_t sample) {
        return static_cast<std::size_t>(
            (std::uint64_t{sample + 1} * columnCount + spanCount - 1) / spanCount - 1);
    };
    const std::size_t lastColumn = columnOf(end - 1);
    // In stretches of whole columns, so the working space stays small however long the span.
    for (std::size_t column = columnOf(first); column <= lastColumn;) {
        const std::size_t from = columnBegin(column);
        std::size_t stop = column + 1;
        while (stop <= lastColumn && columnBegin(stop + 1) - from <= kChunkSamples) {
            ++stop;
        }
        scratch.resize(columnBegin(stop) - from);
        if (!read(from, scratch)) {
            valid = false;
            return false;
        }
        for (; column < stop; ++column) {
            const auto [minimum, maximum] = minMax(std::span<const float>(scratch).subspan(
                columnBegin(column) - from, columnBegin(column + 1) - columnBegin(column)));
            output[column * 2] = minimum;
            output[column * 2 + 1] = maximum;
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
    if (kept == columns) {
        return output;
    }

    // The rest in stretches of whole columns; columns past the end of the source stay silent and
    // unready.
    const auto begin = [&](std::size_t column) { return grid.boundary(firstColumn + column); };
    std::size_t column = kept;
    while (column < columns) {
        std::size_t wanted = 1;
        while (column + wanted < columns &&
               begin(column + wanted + 1) - begin(column) <= kChunkSamples) {
            ++wanted;
        }
        std::size_t available = wanted;
        while (available > 0) {
            scratch.resize(static_cast<std::size_t>(begin(column + available) - begin(column)));
            if (read(origin + begin(column), scratch)) {
                break;
            }
            available /= 2; // The newest audio may not be there yet: try a shorter stretch.
        }
        for (std::size_t index = 0; index < available; ++index) {
            const std::span<const float> samples = std::span<const float>(scratch).subspan(
                static_cast<std::size_t>(begin(column + index) - begin(column)),
                static_cast<std::size_t>(begin(column + index + 1) - begin(column + index)));
            const auto [minimum, maximum] = minMax(samples);
            output[(column + index) * 2] = minimum;
            output[(column + index) * 2 + 1] = maximum;
            invalid[column + index] =
                std::ranges::all_of(samples, [](float sample) { return std::isfinite(sample); })
                    ? 0
                    : 1;
        }
        column += available;
        if (available < wanted) {
            break;
        }
    }
    readyColumns = column;
    std::fill(output.begin() + static_cast<std::ptrdiff_t>(column * 2), output.end(), 0.0f);
    std::fill(invalid.begin() + static_cast<std::ptrdiff_t>(column), invalid.end(), 0);
    return output;
}

bool RollColumns::holdsInvalid() const
{
    return std::any_of(invalid.begin(),
                       invalid.begin() +
                           static_cast<std::ptrdiff_t>(std::min(readyColumns, invalid.size())),
                       [](std::uint8_t column) { return column != 0; });
}

} // namespace orizon::engine
