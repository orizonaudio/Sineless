#include "engine/Reducer.h"
#include "engine/Roll.h"

#include <cmath>
#include <gtest/gtest.h>
#include <vector>

namespace orizon::engine {
namespace {

TEST(Roll, AppliesToSpansAboveOneSecond)
{
    EXPECT_FALSE(isRollSpan(0.5));
    EXPECT_FALSE(isRollSpan(1.0)) << "1 s itself is still triggered (FR-3)";
    EXPECT_TRUE(isRollSpan(1.001));
    EXPECT_TRUE(isRollSpan(4.0));
}

TEST(Roll, EndsAtTheLastCompleteColumnOnFixedBoundaries)
{
    // 4 s at 48 kHz over 1000 columns: 192 samples per column.
    const RollWindow window = rollWindow(1'000'100, 192'000.0, 1000);
    EXPECT_EQ(window.samplesPerColumn, 192u);
    EXPECT_EQ(window.sampleCount, 192'000u);
    EXPECT_EQ((window.start + window.sampleCount) % 192, 0u);
    EXPECT_LE(window.start + window.sampleCount, 1'000'100u);
    EXPECT_GT(window.start + window.sampleCount + 192, 1'000'100u);
}

// Successive frames must show identical columns, shifted, never regrouped samples.
TEST(Roll, SuccessiveFramesShowTheSameColumnsShifted)
{
    std::vector<float> signal(400'000);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.0123) *
                                       std::sin(static_cast<double>(i) * 0.000071));
    }
    const auto columnsAt = [&](std::uint64_t end) {
        const RollWindow window = rollWindow(end, 192'000.0, 1000);
        std::vector<float> output;
        reduceToColumns(std::span(signal).subspan(window.start, window.sampleCount), 1000, output);
        return std::pair{window, output};
    };
    const auto [firstWindow, first] = columnsAt(300'000);
    const auto [laterWindow, later] = columnsAt(300'000 + 192 * 7 + 50);
    const std::uint64_t shift = (laterWindow.start - firstWindow.start) / 192;
    ASSERT_EQ(shift, 7u);
    for (std::size_t column = shift; column < 1000; ++column) {
        ASSERT_EQ(first[column * 2], later[(column - shift) * 2]) << column;
        ASSERT_EQ(first[column * 2 + 1], later[(column - shift) * 2 + 1]) << column;
    }
}

TEST(Roll, ShowsWhatExistsAtTheStart)
{
    const RollWindow window = rollWindow(5000, 192'000.0, 1000);
    EXPECT_EQ(window.start, 0u);
    EXPECT_EQ(window.sampleCount, 4992u);
}

} // namespace
} // namespace orizon::engine
