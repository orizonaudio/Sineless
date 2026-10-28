#include "engine/FreeRunning.h"

#include <algorithm>

namespace orizon::engine {

FreeWindow freeWindow(std::uint64_t edge, std::uint64_t spanSamples, std::size_t columns)
{
    const std::uint64_t columnCount = std::max<std::size_t>(columns, 1);
    if (spanSamples <= columnCount) {
        // Raw samples: the grid is the samples.
        const std::uint64_t count = std::min(spanSamples, edge);
        return {.start = edge - count, .sampleCount = count};
    }
    const ColumnGrid grid{.spanSamples = spanSamples, .columns = columnCount};
    const std::uint64_t last = grid.columnOf(edge); // Its column is not complete yet.
    if (last < columnCount) {
        return {.sampleCount = grid.boundary(last)};
    }
    const std::uint64_t first = last - columnCount;
    return {.start = grid.boundary(first),
            .sampleCount = spanSamples,
            .firstColumn = first,
            .onGrid = true};
}

} // namespace orizon::engine
