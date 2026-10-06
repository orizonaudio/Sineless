#include "engine/Reducer.h"

#include <algorithm>
#include <cstdint>

namespace orizon::engine {

using protocol::FrameMode;

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

} // namespace orizon::engine
