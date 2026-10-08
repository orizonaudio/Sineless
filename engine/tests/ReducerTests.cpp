#include "engine/Reducer.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::FrameMode;

std::vector<float> randomSamples(std::size_t count, unsigned seed)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> value(-1.0f, 1.0f);
    std::vector<float> samples(count);
    std::ranges::generate(samples, [&] { return value(random); });
    return samples;
}

// Independent reference: assigns each sample to its column by index, then takes min and max.
std::vector<float> bruteForceMinMax(const std::vector<float>& samples, std::size_t columns)
{
    std::vector<float> minimum(columns, std::numeric_limits<float>::infinity());
    std::vector<float> maximum(columns, -std::numeric_limits<float>::infinity());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        // Sample i belongs to the last column whose start is at or before i.
        std::size_t column = 0;
        while (column + 1 < columns && std::uint64_t{column + 1} * samples.size() / columns <= i) {
            ++column;
        }
        minimum[column] = std::min(minimum[column], samples[i]);
        maximum[column] = std::max(maximum[column], samples[i]);
    }
    std::vector<float> pairs;
    for (std::size_t column = 0; column < columns; ++column) {
        pairs.push_back(minimum[column]);
        pairs.push_back(maximum[column]);
    }
    return pairs;
}

TEST(Reducer, MatchesBruteForceAcrossSpansAndWidths)
{
    unsigned seed = 1;
    for (std::size_t count : {2U, 3U, 101U, 960U, 1000U, 4801U, 48000U}) {
        for (std::size_t columns : {1U, 2U, 7U, 100U, 640U, 1919U}) {
            if (count <= columns) {
                continue;
            }
            const auto samples = randomSamples(count, seed++);
            std::vector<float> output;
            ASSERT_EQ(reduceToColumns(samples, columns, output), FrameMode::kMinMax);
            ASSERT_EQ(output, bruteForceMinMax(samples, columns))
                << count << " samples, " << columns << " columns";
        }
    }
}

TEST(Reducer, KeepsASinglePeakVisible)
{
    std::vector<float> samples(48000, 0.0f);
    samples[12345] = 1.0f;
    samples[30000] = -1.0f;
    std::vector<float> output;
    reduceToColumns(samples, 800, output);
    EXPECT_EQ(*std::ranges::max_element(output), 1.0f);
    EXPECT_EQ(*std::ranges::min_element(output), -1.0f);
}

TEST(Reducer, PassesSamplesThroughAtOneOrFewerPerColumn)
{
    const auto samples = randomSamples(500, 3);
    std::vector<float> output;
    EXPECT_EQ(reduceToColumns(samples, 500, output), FrameMode::kRaw);
    EXPECT_EQ(output, samples);
    EXPECT_EQ(reduceToColumns(samples, 1920, output), FrameMode::kRaw);
    EXPECT_EQ(output, samples);
}

TEST(Reducer, HandlesEmptyInputAndZeroColumns)
{
    std::vector<float> output{1.0f};
    EXPECT_EQ(reduceToColumns({}, 100, output), FrameMode::kRaw);
    EXPECT_TRUE(output.empty());
    const auto samples = randomSamples(10, 4);
    EXPECT_EQ(reduceToColumns(samples, 0, output), FrameMode::kRaw);
    EXPECT_EQ(output, samples);
}

TEST(Reducer, ReusesOutputCapacity)
{
    const auto samples = randomSamples(48000, 5);
    std::vector<float> output;
    reduceToColumns(samples, 1000, output);
    const float* storage = output.data();
    reduceToColumns(samples, 1000, output);
    EXPECT_EQ(output.data(), storage);
}

// Reduces `samples` incrementally: a full pass, then again only [first, end) after changing it.
std::vector<float> incremental(std::vector<float>& samples, std::size_t columns, std::size_t first,
                               std::size_t end, unsigned seed)
{
    ColumnReduction reduction;
    const auto read = [&](std::size_t from, std::span<float> destination) {
        std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(from), destination.size(),
                    destination.begin());
        return true;
    };
    reduction.reset(samples.size(), columns);
    EXPECT_TRUE(reduction.update(0, samples.size(), read));
    const auto changed = randomSamples(end - first, seed);
    std::ranges::copy(changed, samples.begin() + static_cast<std::ptrdiff_t>(first));
    EXPECT_TRUE(reduction.update(first, end, read));
    return {reduction.values().begin(), reduction.values().end()};
}

TEST(ColumnReduction, MatchesAFullReductionAfterChangesAnywhere)
{
    for (const auto [first, end] : {std::pair<std::size_t, std::size_t>{0, 1},
                                    {47999, 48000},
                                    {1000, 1801},
                                    {0, 48000},
                                    {23999, 24001}}) {
        auto samples = randomSamples(48000, 6);
        const auto result = incremental(samples, 1366, first, end, 7);
        std::vector<float> expected;
        EXPECT_EQ(reduceToColumns(samples, 1366, expected), FrameMode::kMinMax);
        EXPECT_EQ(result, expected) << "changed [" << first << ", " << end << ")";
    }
}

TEST(ColumnReduction, PassesRawSamplesThroughWhenThereAreFew)
{
    auto samples = randomSamples(500, 8);
    const auto result = incremental(samples, 1366, 100, 140, 9);
    EXPECT_EQ(result, samples);
}

TEST(ColumnReduction, StartsOverWhenTheSpanOrWidthChanges)
{
    ColumnReduction reduction;
    reduction.reset(48000, 1000);
    EXPECT_TRUE(reduction.fits(48000, 1000));
    EXPECT_FALSE(reduction.fits(48000, 1200));
    reduction.invalidate();
    EXPECT_FALSE(reduction.fits(48000, 1000));
    EXPECT_FALSE(reduction.update(0, 10, [](std::size_t, std::span<float>) { return true; }));
}

TEST(RollColumns, MatchesAFullReductionAsTheWindowRolls)
{
    const auto signal = randomSamples(200000, 10);
    std::size_t reads = 0;
    std::uint64_t available = 150000; // Audio captured so far.
    const auto read = [&](std::uint64_t first, std::span<float> destination) {
        if (first + destination.size() > available) {
            return false;
        }
        reads += destination.size();
        std::copy_n(signal.begin() + static_cast<std::ptrdiff_t>(first), destination.size(),
                    destination.begin());
        return true;
    };
    RollColumns roll;
    constexpr std::size_t kColumns = 500;
    constexpr std::uint64_t kPerColumn = 96;
    for (const std::uint64_t start : {9600, 9600 + 96 * 3, 9600 + 96 * 40, 9600 + 96 * 41}) {
        reads = 0;
        const auto values = roll.update(start, kColumns, kPerColumn, read);
        std::vector<float> expected;
        reduceToColumns(std::span(signal).subspan(start, kColumns * kPerColumn), kColumns,
                        expected);
        EXPECT_EQ(std::vector<float>(values.begin(), values.end()), expected) << start;
        if (start != 9600) {
            EXPECT_LE(reads, 41 * kPerColumn) << "only the new columns are read";
        }
    }
}

TEST(RollColumns, FillsInColumnsWhoseAudioArrivesLater)
{
    const auto signal = randomSamples(20000, 11);
    std::uint64_t available = 9000;
    const auto read = [&](std::uint64_t first, std::span<float> destination) {
        if (first + destination.size() > available) {
            return false;
        }
        std::copy_n(signal.begin() + static_cast<std::ptrdiff_t>(first), destination.size(),
                    destination.begin());
        return true;
    };
    RollColumns roll;
    auto values = roll.update(0, 100, 100, read); // Wants 10000 samples; 9000 are there.
    EXPECT_EQ(values[std::size_t{2} * 99], 0.0f) << "silent until its audio arrives";
    available = 10000;
    values = roll.update(0, 100, 100, read);
    std::vector<float> expected;
    reduceToColumns(std::span(signal).first(10000), 100, expected);
    EXPECT_EQ(std::vector<float>(values.begin(), values.end()), expected);
}

} // namespace
} // namespace orizon::engine
