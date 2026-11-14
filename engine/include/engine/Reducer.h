#pragma once

#include "engine/FreeRunning.h"
#include "engine/Reconstruction.h"
#include "protocol/Protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace orizon::engine {

// No join in a block: every gap between samples may be reconstructed.
inline constexpr std::size_t kNoSeam = std::numeric_limits<std::size_t>::max();

// The samples each side of its own that a min/max column reads (Reconstruction's column filter,
// 6 samples each side of a gap, over the gaps from two before its extreme sample to one after).
inline constexpr std::size_t kColumnFilterReach = 6;
inline constexpr std::size_t kColumnReach = kColumnFilterReach + 1;

// The smallest and largest finite sample of a column and the first index of each, kNone (and NaN
// values) in a column of nothing but samples that are not a number.
struct Extremes
{
    static constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
    float minimum = std::numeric_limits<float>::quiet_NaN();
    float maximum = std::numeric_limits<float>::quiet_NaN();
    std::size_t lowest = kNone;
    std::size_t highest = kNone;
};
[[nodiscard]] Extremes extremesOf(std::span<const float> samples);

// A min/max column's extremes from the band-limited wave around its extreme samples, not the
// samples alone, so that a crest between two samples is drawn at its height (DESIGN.md, AD-32):
// the wave at kColumnOversampling points a sample over the gaps from two samples before an
// extreme sample to one after, with a short filter (kColumnFilterReach each side, Kaiser β 6),
// and the vertex of the parabola through each local extreme of those points. A gap whose
// neighbours run out, cross the seam or include one not finite is straight, as the raw view draws
// it. Its working space is sized once.
class ColumnPeaks
{
public:
    static constexpr std::size_t kColumnOversampling = 4;
    static_assert(kColumnOversampling == protocol::kMinPointsPerSample,
                  "the crossing from columns to raw is bounded by the raw floor's polyline; the "
                  "step's indices below assume 4 points a sample");

    // Widens `minimum` and `maximum`, the column's sample extremes, to the wave around its lowest
    // and highest samples, samples[lowest] and samples[highest]. Positions outside `samples` are
    // not held; `seams` (indices into `samples`) are the first samples after joins, such as a
    // sweep's playhead: no wave is drawn across any of them.
    void widen(std::span<const float> samples, std::size_t lowest, std::size_t highest,
               std::span<const std::size_t> seams, float& minimum, float& maximum);

private:
    // Samples n - kColumnReach … n + kColumnReach around an extreme sample n.
    static constexpr std::size_t kSpan = (2 * kColumnReach) + 1;
    Reconstruction filter{kColumnFilterReach, 6.0};
    std::array<float, kSpan> around{};
    std::array<float, kSpan> finiteAround{}; // `around`, 0 where it is not finite.
    std::array<float, (4 * kColumnOversampling) + 1> points{};
    std::array<std::uint8_t, 4> reconstructed{};
    std::vector<std::size_t> aroundSeams; // The joins within `around`, as indices into it.
    float reference = 0.0f;               // The extreme sample the wave is taken from.
    std::array<const float*, kColumnOversampling> phaseWeights{}; // Into `filter`, once designed.

    // The wave around samples[n]: 17 points a quarter sample apart from n - 2 to n + 2.
    void wave(std::span<const float> samples, std::size_t n, std::span<const std::size_t> seams);
    void widenAround(bool highest, float& value) const;
};

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
// The same, of `withNeighbours` whose first `lead` and last `trail` samples are only neighbours,
// read for the wave between the samples: the mode is decided by the samples shown between them.
// Raw, the output keeps the neighbours; min/max columns cover the shown samples alone.
// Min/max columns take their extremes from the wave around their extreme samples (ColumnPeaks),
// reading the neighbours as far as `withNeighbours` holds them; `seam` indexes `withNeighbours`.
protocol::FrameMode reduceToColumns(std::span<const float> withNeighbours, std::size_t lead,
                                    std::size_t trail, std::size_t columns,
                                    std::vector<float>& output, std::size_t seam = kNoSeam,
                                    ColumnPeaks* peaks = nullptr);

// reduceToColumns of a span whose samples change only in places, kept up to date by reducing
// again just the columns that hold changed samples. The result matches reducing the whole span.
class ColumnReduction
{
public:
    // Reads samples [first, first + destination.size()) of the span into `destination`.
    using Reader = std::function<bool(std::size_t first, std::span<float> destination)>;

    // Starts over for a span of `count` samples in `columns`, its first `lead` and last `trail`
    // only neighbours read for the wave between the samples; everything is stale. With
    // `trackInvalid`, it also notes which columns hold a sample that is not finite.
    void reset(std::size_t count, std::size_t columns, bool trackInvalid = false,
               std::size_t lead = 0, std::size_t trail = 0);
    [[nodiscard]] bool fits(std::size_t count, std::size_t columns, std::size_t lead = 0,
                            std::size_t trail = 0) const
    {
        return valid && count == spanCount && columns == columnCount && lead == leadCount &&
               trail == trailCount;
    }
    // Forgets the result, so the next use starts over.
    void invalidate() { valid = false; }

    // Reduces again the columns that hold samples [first, end). False if `read` fails; the result
    // is then invalid.
    // Min/max columns read up to kColumnReach neighbours each side of their samples within the
    // span, and every column whose neighbours changed is reduced again. `seam` indexes the span.
    bool update(std::size_t first, std::size_t end, const Reader& read, std::size_t seam = kNoSeam);
    // The same with several joins, as a sum of lanes has at each lane's own.
    bool update(std::size_t first, std::size_t end, const Reader& read,
                std::span<const std::size_t> seams);

    // Decided by the samples shown, without the neighbours.
    [[nodiscard]] protocol::FrameMode mode() const
    {
        return shownCount() <= columnCount || columnCount == 0 ? protocol::FrameMode::kRaw
                                                               : protocol::FrameMode::kMinMax;
    }
    // Raw samples with their neighbours, or a (min, max) pair per column.
    [[nodiscard]] std::span<const float> values() const { return output; }
    // Tracking them (reset), whether a column holds a sample that is not finite (NaN or infinite),
    // which min/max columns leave out.
    [[nodiscard]] bool holdsInvalid() const;

private:
    [[nodiscard]] std::size_t columnBegin(std::size_t column) const;
    [[nodiscard]] std::size_t shownCount() const { return spanCount - leadCount - trailCount; }

    std::size_t spanCount = 0;
    std::size_t columnCount = 0;
    std::size_t leadCount = 0;
    std::size_t trailCount = 0;
    bool valid = false;
    bool tracking = false;
    std::vector<float> output;
    std::vector<float> scratch;
    std::vector<std::size_t> seamsHere; // The joins, as indices into `scratch`.
    ColumnPeaks peaks;
    std::vector<std::uint8_t> invalid; // Per min/max column, while tracking: holds one.
    std::size_t invalidColumns = 0;    // How many of them do.
};

// The columns of a free-running window, kept from frame to frame. Its columns sit on a grid fixed
// to absolute sample indices (ColumnGrid), and the past does not change, so a column once reduced
// stays valid: each frame reduces only the columns that scrolled in, or that had no audio yet.
class RollColumns
{
public:
    // Reads `destination.size()` samples from absolute index `first`; false if not available.
    using Reader = std::function<bool(std::uint64_t first, std::span<float> destination)>;

    // The `grid.columns` columns from grid column `firstColumn`, as (min, max) pairs; column k
    // covers the samples from `origin + grid.boundary(k)`. Another track's lane shows the same
    // columns at its own `origin`. Columns without audio yet are silent until a later update
    // finds it. Each column reads kColumnReach neighbours each side (ColumnPeaks); one whose
    // neighbours after it are not held yet is drawn but reduced again until they are.
    std::span<const float> update(std::uint64_t origin, std::uint64_t firstColumn,
                                  const ColumnGrid& grid, const Reader& read);

    // Forgets every column, e.g. when the source changes.
    void clear()
    {
        readyColumns = 0;
        drawnColumns = 0;
    }
    // Whether a column on screen holds a sample that is not finite (NaN or infinite).
    [[nodiscard]] bool holdsInvalid() const;

private:
    std::uint64_t gridOrigin = 0;
    std::uint64_t firstColumnShown = 0; // Grid column shown first.
    ColumnGrid columnGrid{};
    std::size_t readyColumns = 0; // Leading columns reduced for good.
    std::size_t drawnColumns = 0; // Leading columns drawn: the ready ones and those still waiting
                                  // for their trailing neighbours.
    std::vector<float> output;
    std::vector<std::uint8_t> invalid; // Per column: holds a sample that is not finite.
    std::vector<float> scratch;
    ColumnPeaks peaks;
};

} // namespace orizon::engine
