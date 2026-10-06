#include "engine/Reducer.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <random>
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

} // namespace
} // namespace orizon::engine
