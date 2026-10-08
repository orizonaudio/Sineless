#include "engine/Reducer.h"

#include <algorithm>
#include <cstdint>

namespace orizon::engine {

using protocol::FrameMode;

namespace {

// The most samples read at once when reducing again: bounds the working space.
constexpr std::size_t kChunkSamples = 16384;

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
        const auto [minimum, maximum] =
            std::minmax_element(samples.begin() + static_cast<std::ptrdiff_t>(begin),
                                samples.begin() + static_cast<std::ptrdiff_t>(end));
        output[column * 2] = *minimum;
        output[column * 2 + 1] = *maximum;
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
            const auto [minimum, maximum] = std::minmax_element(
                scratch.begin() + static_cast<std::ptrdiff_t>(columnBegin(column) - from),
                scratch.begin() + static_cast<std::ptrdiff_t>(columnBegin(column + 1) - from));
            output[column * 2] = *minimum;
            output[column * 2 + 1] = *maximum;
        }
    }
    return true;
}

std::span<const float> RollColumns::update(std::uint64_t start, std::size_t columns,
                                           std::uint64_t perColumn, const Reader& read)
{
    // Keep the columns still on screen, moved to their new places, if they are the same columns.
    std::size_t kept = 0;
    if (perColumn == samplesPerColumn && columns == columnCount && start >= firstSample &&
        (start - firstSample) % perColumn == 0) {
        const auto shift = (start - firstSample) / perColumn;
        if (shift < readyColumns) {
            kept = readyColumns - static_cast<std::size_t>(shift);
            std::copy(output.begin() + static_cast<std::ptrdiff_t>(shift * 2),
                      output.begin() + static_cast<std::ptrdiff_t>(readyColumns * 2),
                      output.begin());
        }
    }
    firstSample = start;
    samplesPerColumn = perColumn;
    columnCount = columns;
    output.resize(columns * 2);
    readyColumns = kept;
    if (kept == columns) {
        return output;
    }

    // The rest in stretches of whole columns; columns past the end of the source stay silent and
    // unready.
    const std::size_t stretch =
        std::max<std::size_t>(1, static_cast<std::size_t>(kChunkSamples / perColumn));
    std::size_t column = kept;
    while (column < columns) {
        const std::size_t wanted = std::min(stretch, columns - column);
        scratch.resize(static_cast<std::size_t>(wanted * perColumn));
        std::size_t available = wanted;
        while (available > 0 &&
               !read(start + column * perColumn,
                     std::span(scratch).first(static_cast<std::size_t>(available * perColumn)))) {
            available /= 2; // The newest audio may not be there yet: try a shorter stretch.
        }
        for (std::size_t index = 0; index < available; ++index) {
            const auto begin = scratch.begin() + static_cast<std::ptrdiff_t>(index * perColumn);
            const auto [minimum, maximum] =
                std::minmax_element(begin, begin + static_cast<std::ptrdiff_t>(perColumn));
            output[(column + index) * 2] = *minimum;
            output[(column + index) * 2 + 1] = *maximum;
        }
        column += available;
        if (available < wanted) {
            break;
        }
    }
    readyColumns = column;
    std::fill(output.begin() + static_cast<std::ptrdiff_t>(column * 2), output.end(), 0.0f);
    return output;
}

} // namespace orizon::engine
