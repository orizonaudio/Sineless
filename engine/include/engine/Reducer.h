#pragma once

#include "protocol/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace orizon::engine {

// Reduces the visible span of one channel to the UI's device-pixel resolution.
//
// With more samples than columns, column c covers samples [c * n / columns, (c + 1) * n / columns)
// and contributes a (min, max) pair, so every sample lands in exactly one column and no peak is
// lost. With at most one sample per column the samples are passed through unchanged.
// Values stay in signal units; all mapping to pixels happens in the UI.
//
// Returns the mode used. `output` is resized but keeps its capacity across calls.
protocol::FrameMode reduceToColumns(std::span<const float> samples, std::size_t columns,
                                    std::vector<float>& output);

// reduceToColumns of a span whose samples change only in places, kept up to date by reducing
// again just the columns that hold changed samples. The result matches reducing the whole span.
class ColumnReduction
{
public:
    // Reads samples [first, first + destination.size()) of the span into `destination`.
    using Reader = std::function<bool(std::size_t first, std::span<float> destination)>;

    // Starts over for a span of `count` samples shown in `columns`; everything is stale.
    void reset(std::size_t count, std::size_t columns);
    [[nodiscard]] bool fits(std::size_t count, std::size_t columns) const
    {
        return valid && count == spanCount && columns == columnCount;
    }
    // Forgets the result, so the next use starts over.
    void invalidate() { valid = false; }

    // Reduces again the columns that hold samples [first, end). False if `read` fails; the result
    // is then invalid.
    bool update(std::size_t first, std::size_t end, const Reader& read);

    [[nodiscard]] protocol::FrameMode mode() const
    {
        return spanCount <= columnCount || columnCount == 0 ? protocol::FrameMode::kRaw
                                                            : protocol::FrameMode::kMinMax;
    }
    // Raw samples, or a (min, max) pair per column.
    [[nodiscard]] std::span<const float> values() const { return output; }

private:
    [[nodiscard]] std::size_t columnBegin(std::size_t column) const;

    std::size_t spanCount = 0;
    std::size_t columnCount = 0;
    bool valid = false;
    std::vector<float> output;
    std::vector<float> scratch;
};

// The columns of a rolling window, kept from frame to frame. Roll columns sit on fixed absolute
// sample indices (see RollWindow), and the past does not change, so a column once reduced stays
// valid: each frame reduces only the columns that scrolled in, or that had no audio yet.
class RollColumns
{
public:
    // Reads `destination.size()` samples from absolute index `first`; false if not available.
    using Reader = std::function<bool(std::uint64_t first, std::span<float> destination)>;

    // The `columns` columns of `perColumn` samples each from absolute sample `start`, as (min, max)
    // pairs. Columns without audio yet are silent until a later update finds it.
    std::span<const float> update(std::uint64_t start, std::size_t columns, std::uint64_t perColumn,
                                  const Reader& read);

    // Forgets every column, e.g. when the source changes.
    void clear() { readyColumns = 0; }
    // Whether a column on screen holds a sample that is not finite (NaN or infinite).
    [[nodiscard]] bool holdsInvalid() const;

private:
    std::uint64_t firstSample = 0; // Absolute index where column 0 starts.
    std::uint64_t samplesPerColumn = 0;
    std::size_t columnCount = 0;
    std::size_t readyColumns = 0; // Leading columns holding reduced audio.
    std::vector<float> output;
    std::vector<std::uint8_t> invalid; // Per column: holds a sample that is not finite.
    std::vector<float> scratch;
};

} // namespace orizon::engine
