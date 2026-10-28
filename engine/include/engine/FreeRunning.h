#pragma once

#include "protocol/Protocol.h"

#include <cstddef>
#include <cstdint>

namespace orizon::engine {

// Spans above this run free whatever the trigger mode; the Free trigger mode runs free at any span.
inline constexpr double kFreeRunningThresholdSeconds = protocol::kFreeRunningThresholdSeconds;

// Whether the window runs free (ends at the newest audio and moves with it) rather than being
// placed by the trigger.
[[nodiscard]] constexpr bool isFreeRunning(double spanSeconds, protocol::TriggerMode mode)
{
    return spanSeconds > kFreeRunningThresholdSeconds || mode == protocol::TriggerMode::kFree;
}

// The columns of a span of `spanSamples` samples shown in `columns` columns, on a grid fixed to
// absolute sample indices: column k covers [k * spanSamples / columns, (k + 1) * spanSamples /
// columns), rounded down. Any `columns` consecutive columns hold exactly the span, and successive
// frames show the same columns shifted rather than regrouped samples that shimmer.
struct ColumnGrid
{
    std::uint64_t spanSamples = 0;
    std::uint64_t columns = 1;

    // Where column `column` starts. The product stays far below 2^64: a year of audio at the
    // highest rate in 16384 columns is 2e17.
    [[nodiscard]] constexpr std::uint64_t boundary(std::uint64_t column) const
    {
        return column * spanSamples / columns;
    }
    // The column holding sample `index`: the last one starting at or before it.
    [[nodiscard]] constexpr std::uint64_t columnOf(std::uint64_t index) const
    {
        return ((index + 1) * columns + spanSamples - 1) / spanSamples - 1;
    }
};

// The samples a free-running frame shows: the `columns` grid columns ending at the last column
// boundary at or before `edge`. With no more samples than columns, the grid is the samples and the
// window ends at `edge`. At the start of the capture, what exists.
struct FreeWindow
{
    std::uint64_t start = 0;
    std::uint64_t sampleCount = 0;
    std::uint64_t firstColumn = 0; // The grid column at `start`, when on the grid.
    bool onGrid = false;           // Whole grid columns, more samples than columns.
};

[[nodiscard]] FreeWindow freeWindow(std::uint64_t edge, std::uint64_t spanSamples,
                                    std::size_t columns);

} // namespace orizon::engine
