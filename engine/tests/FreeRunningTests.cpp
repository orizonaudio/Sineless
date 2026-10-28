#include "engine/FreeRunning.h"
#include "engine/Reducer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::TriggerMode;

class FreeRunningModes : public testing::TestWithParam<std::size_t>
{
};

// Above the threshold the window runs free in every trigger mode; at or below it, only in Free.
TEST_P(FreeRunningModes, RunFreeAboveTheThresholdAndInFree)
{
    const auto mode = static_cast<TriggerMode>(GetParam());
    const bool free = mode == TriggerMode::kFree;
    for (const double rate : {44100.0, 48000.0, 96000.0, 384000.0}) {
        // The threshold as a whole number of samples at the rate, and one sample more.
        const double atThreshold = std::round(kFreeRunningThresholdSeconds * rate) / rate;
        EXPECT_EQ(isFreeRunning(atThreshold, mode), free) << rate;
        EXPECT_TRUE(isFreeRunning(atThreshold + 1.0 / rate, mode)) << rate;
    }
    EXPECT_EQ(isFreeRunning(0.0005, mode), free);
    EXPECT_EQ(isFreeRunning(0.05, mode), free);
    EXPECT_TRUE(isFreeRunning(4.0, mode));
}

INSTANTIATE_TEST_SUITE_P(TriggerModes, FreeRunningModes,
                         testing::Range<std::size_t>(0, protocol::kTriggerModeCount),
                         [](const testing::TestParamInfo<std::size_t>& info) {
                             return std::string(protocol::kTriggerModeNames.at(info.param));
                         });

struct GridCase
{
    double rate;
    double seconds;
    std::size_t columns;
};

class FreeWindows : public testing::TestWithParam<GridCase>
{
protected:
    [[nodiscard]] std::uint64_t span() const
    {
        return static_cast<std::uint64_t>(std::llround(GetParam().seconds * GetParam().rate));
    }
};

// The window always shows exactly the span, also when it does not divide into the columns, and
// ends at the last column boundary at or before the edge.
TEST_P(FreeWindows, ShowExactlyTheSpanEndingAtTheLastBoundary)
{
    const ColumnGrid grid{.spanSamples = span(), .columns = GetParam().columns};
    for (std::uint64_t edge = 3 * span(); edge < 3 * span() + 5 * span() / grid.columns + 7;
         edge += 3) {
        const FreeWindow window = freeWindow(edge, span(), GetParam().columns);
        ASSERT_TRUE(window.onGrid);
        ASSERT_EQ(window.sampleCount, span()) << edge;
        ASSERT_EQ(window.start, grid.boundary(window.firstColumn)) << edge;
        const std::uint64_t end = window.start + window.sampleCount;
        ASSERT_EQ(end, grid.boundary(window.firstColumn + grid.columns)) << edge;
        ASSERT_LE(end, edge);
        ASSERT_GT(grid.boundary(window.firstColumn + grid.columns + 1), edge) << edge;
    }
}

// Successive frames show the same columns shifted, never regrouped samples: the columns kept from
// an earlier frame equal the ones a fresh reduction of the later frame finds.
TEST_P(FreeWindows, SuccessiveFramesShowTheSameColumnsShifted)
{
    const std::uint64_t spanSamples = span();
    const std::size_t columns = GetParam().columns;
    std::vector<float> signal(static_cast<std::size_t>(4 * spanSamples));
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.0123) *
                                       std::sin(static_cast<double>(i) * 0.000071));
    }
    const auto read = [&](std::uint64_t first, std::span<float> destination) {
        if (first + destination.size() > signal.size()) {
            return false;
        }
        std::copy_n(signal.begin() + static_cast<std::ptrdiff_t>(first), destination.size(),
                    destination.begin());
        return true;
    };
    const ColumnGrid grid{.spanSamples = spanSamples, .columns = columns};
    RollColumns kept;
    for (std::uint64_t edge = 2 * spanSamples; edge < 2 * spanSamples + 40 * spanSamples / columns;
         edge += spanSamples / columns * 3 + 5) {
        const FreeWindow window = freeWindow(edge, spanSamples, columns);
        const auto values = kept.update(0, window.firstColumn, grid, read);
        RollColumns fresh;
        const auto expected = fresh.update(0, window.firstColumn, grid, read);
        ASSERT_TRUE(std::ranges::equal(values, expected)) << edge;
        // And each column is the min and max of exactly its samples.
        for (std::size_t column : {std::size_t{0}, columns / 2, columns - 1}) {
            const auto from =
                static_cast<std::ptrdiff_t>(grid.boundary(window.firstColumn + column));
            const auto to =
                static_cast<std::ptrdiff_t>(grid.boundary(window.firstColumn + column + 1));
            const auto [low, high] =
                std::minmax_element(signal.begin() + from, signal.begin() + to);
            ASSERT_EQ(values[column * 2], *low) << column;
            ASSERT_EQ(values[column * 2 + 1], *high) << column;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    SpansAndColumns, FreeWindows,
    testing::Values(GridCase{.rate = 48000.0, .seconds = 4.0, .columns = 1000},
                    GridCase{.rate = 48000.0, .seconds = 1.3, .columns = 1000},
                    GridCase{.rate = 48000.0, .seconds = 1.3, .columns = 1234},
                    GridCase{.rate = 44100.0, .seconds = 1.0, .columns = 2560},
                    GridCase{.rate = 96000.0, .seconds = 0.05, .columns = 1517},
                    GridCase{.rate = 384000.0, .seconds = 4.0, .columns = 16384},
                    GridCase{.rate = 44100.0, .seconds = 0.0234, .columns = 1000}),
    [](const testing::TestParamInfo<GridCase>& info) {
        return "Rate" + std::to_string(static_cast<int>(info.param.rate)) + "Span" +
               std::to_string(static_cast<int>(std::lround(info.param.seconds * 10000))) +
               "Columns" + std::to_string(info.param.columns);
    });

// With no more samples than columns the grid is the samples: the window ends at the edge.
TEST(FreeWindow, IsRawSamplesUpToOneSamplePerColumn)
{
    for (const std::uint64_t span : {999U, 1000U}) {
        const FreeWindow window = freeWindow(123'457, span, 1000);
        EXPECT_FALSE(window.onGrid) << span;
        EXPECT_EQ(window.start + window.sampleCount, 123'457U) << span;
        EXPECT_EQ(window.sampleCount, span);
    }
    const FreeWindow columns = freeWindow(123'457, 1001, 1000);
    EXPECT_TRUE(columns.onGrid);
    EXPECT_EQ(columns.sampleCount, 1001U);
}

TEST(FreeWindow, ShowsWhatExistsAtTheStart)
{
    const FreeWindow window = freeWindow(5000, 192'000, 1000);
    EXPECT_EQ(window.start, 0U);
    EXPECT_EQ(window.sampleCount, 4992U);
    EXPECT_FALSE(window.onGrid);
    const FreeWindow raw = freeWindow(300, 800, 1000);
    EXPECT_EQ(raw.start, 0U);
    EXPECT_EQ(raw.sampleCount, 300U);
}

} // namespace
} // namespace orizon::engine
