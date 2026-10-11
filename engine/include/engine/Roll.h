#pragma once

#include "protocol/Protocol.h"

#include <cstddef>
#include <cstdint>

namespace orizon::engine {

// Spans above this scroll continuously instead of being triggered.
inline constexpr double kRollThresholdSeconds = protocol::kRollThresholdSeconds;

[[nodiscard]] constexpr bool isRollSpan(double spanSeconds)
{
    return spanSeconds > kRollThresholdSeconds;
}

// The samples a roll frame shows. Each column covers a whole number of samples, and column
// boundaries sit on fixed absolute indices (multiples of samplesPerColumn), so successive frames
// show the same columns shifted rather than regrouped samples that shimmer. The newest partial
// column is left out until it is complete.
struct RollWindow
{
    std::uint64_t start = 0;
    std::uint64_t sampleCount = 0;
    std::uint64_t samplesPerColumn = 1;
};

[[nodiscard]] RollWindow rollWindow(std::uint64_t end, double spanSamples, std::size_t columns);

} // namespace orizon::engine
