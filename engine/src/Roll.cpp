#include "engine/Roll.h"

#include <algorithm>
#include <cmath>

namespace orizon::engine {

RollWindow rollWindow(std::uint64_t end, double spanSamples, std::size_t columns)
{
    const std::uint64_t columnCount = std::max<std::size_t>(columns, 1);
    const auto perColumn = static_cast<std::uint64_t>(
        std::max(1.0, std::round(spanSamples / static_cast<double>(columnCount))));
    const std::uint64_t windowEnd = end / perColumn * perColumn;
    const std::uint64_t count = std::min(columnCount * perColumn, windowEnd);
    return {.start = windowEnd - count, .sampleCount = count, .samplesPerColumn = perColumn};
}

} // namespace orizon::engine
