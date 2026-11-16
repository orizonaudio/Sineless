#include "engine/BlockStats.h"

#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <random>
#include <vector>

namespace orizon::engine {
namespace {

std::vector<float> randomSamples(std::size_t count, unsigned seed)
{
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);
    std::vector<float> samples(count);
    for (float& sample : samples) {
        sample = distribution(generator);
    }
    return samples;
}

BlockStats::Reader readerOf(const std::vector<float>& samples)
{
    return [&samples](std::size_t from, std::span<float> destination) {
        std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(from), destination.size(),
                    destination.begin());
        return true;
    };
}

// Measures `samples` incrementally: a full pass, then again only [first, end) after changing it.
BlockStats incremental(std::vector<float>& samples, std::size_t block, std::size_t first,
                       std::size_t end, unsigned seed)
{
    BlockStats stats;
    stats.reset(samples.size(), block);
    EXPECT_TRUE(stats.update(0, samples.size(), readerOf(samples)));
    const auto changed = randomSamples(end - first, seed);
    std::ranges::copy(changed, samples.begin() + static_cast<std::ptrdiff_t>(first));
    EXPECT_TRUE(stats.update(first, end, readerOf(samples)));
    return stats;
}

TEST(BlockStats, MatchesAFullMeasurementAfterChangesAnywhere)
{
    for (const auto [first, end] : {std::pair<std::size_t, std::size_t>{0, 1},
                                    {47999, 48000},
                                    {1000, 1801},
                                    {0, 48000},
                                    {23999, 24001},
                                    {47500, 48000}}) {
        auto samples = randomSamples(48000, 6);
        const BlockStats stats = incremental(samples, 480, first, end, 7);
        BlockStats expected;
        expected.reset(samples.size(), 480);
        ASSERT_TRUE(expected.update(0, samples.size(), readerOf(samples)));
        ASSERT_EQ(stats.blocks().size(), 100u);
        for (std::size_t block = 0; block < 100; ++block) {
            EXPECT_EQ(stats.blocks()[block].peak, expected.blocks()[block].peak)
                << "block " << block << " after changing [" << first << ", " << end << ")";
            EXPECT_DOUBLE_EQ(stats.blocks()[block].sumSquares, expected.blocks()[block].sumSquares)
                << "block " << block;
        }
        EXPECT_EQ(stats.peak(), expected.peak());
        EXPECT_DOUBLE_EQ(stats.sumSquares(), expected.sumSquares());
    }
}

TEST(BlockStats, MeasuresTheWholeSpanWithAShortLastBlock)
{
    std::vector<float> samples(1000, 0.25f);
    samples[999] = -0.5f;
    BlockStats stats;
    stats.reset(samples.size(), 300);
    ASSERT_TRUE(stats.update(0, samples.size(), readerOf(samples)));
    ASSERT_EQ(stats.blocks().size(), 4u) << "three whole blocks and one of 100 samples";
    EXPECT_FLOAT_EQ(stats.blocks()[3].peak, 0.5f);
    EXPECT_NEAR(stats.blocks()[3].sumSquares, 99 * 0.0625 + 0.25, 1e-9);
    EXPECT_FLOAT_EQ(stats.peak(), 0.5f);
    EXPECT_NEAR(stats.sumSquares(), 999 * 0.0625 + 0.25, 1e-9);
}

TEST(BlockStats, StartsOverWhenTheSpanOrBlockChanges)
{
    BlockStats stats;
    stats.reset(48000, 480);
    EXPECT_TRUE(stats.fits(48000, 480));
    EXPECT_FALSE(stats.fits(48000, 400));
    EXPECT_FALSE(stats.fits(47000, 480));
    EXPECT_FALSE(stats.update(0, 10, [](std::size_t, std::span<float>) { return false; }))
        << "a failed read invalidates the statistics";
    EXPECT_FALSE(stats.fits(48000, 480));
}

// The levels of any part of the span, as the Meter reads the part on screen: the blocks inside it
// as kept, and the samples of the blocks its edges fall inside, so that a zoom onto a passage
// reads what a pass over its samples reads, without reading the blocks inside it again. With
// samples that are not finite, left out and counted, and a last block shorter than the rest.
TEST(BlockStats, MeasuresAnyPartFromItsBlocksAndTheSamplesAtItsEdges)
{
    const std::size_t block = 480;
    auto samples = randomSamples(47900, 3);
    for (std::size_t index = 7; index < samples.size(); index += 1013) {
        samples[index] = index % 2 == 0 ? std::numeric_limits<float>::quiet_NaN()
                                        : std::numeric_limits<float>::infinity();
    }
    BlockStats stats;
    stats.reset(samples.size(), block);
    ASSERT_TRUE(stats.update(0, samples.size(), readerOf(samples)));
    std::size_t read = 0;
    const BlockStats::Reader counting = [&](std::size_t from, std::span<float> destination) {
        read += destination.size();
        return readerOf(samples)(from, destination);
    };
    std::vector<std::pair<std::size_t, std::size_t>> parts{
        {0, 47900},   {0, 480},       {100, 101},     {479, 481},   {480, 960},
        {481, 47899}, {47520, 47900}, {47521, 47900}, {1000, 1801}, {959, 960}};
    std::mt19937 random(5); // NOLINT(bugprone-random-generator-seed): reproducible
    std::uniform_int_distribution<std::size_t> anywhere(0, samples.size() - 1);
    for (int index = 0; index < 8; ++index) {
        const std::size_t a = anywhere(random);
        const std::size_t b = anywhere(random);
        parts.emplace_back(std::min(a, b), std::max(a, b) + 1);
    }
    for (const auto& [first, end] : parts) {
        SCOPED_TRACE(testing::Message() << first << " to " << end);
        read = 0;
        const auto levels = stats.measure(first, end, counting);
        ASSERT_TRUE(levels);
        const BlockStats::Block expected =
            measureBlock(std::span<const float>(samples).subspan(first, end - first));
        EXPECT_FLOAT_EQ(levels->peak, expected.peak);
        EXPECT_NEAR(levels->sumSquares, expected.sumSquares, 1e-9 * (1.0 + expected.sumSquares));
        EXPECT_EQ(levels->invalid, expected.invalid);
        EXPECT_LT(read, 2 * block) << "more than the edge blocks' samples read";
    }
    read = 0;
    ASSERT_TRUE(stats.measure(0, samples.size(), counting));
    EXPECT_EQ(read, 0u) << "whole blocks, the short last one too, are read from what is kept";

    const BlockStats::Reader failing = [](std::size_t, std::span<float>) { return false; };
    EXPECT_FALSE(stats.measure(100, 2000, failing)) << "an edge that could not be read";
    BlockStats never;
    EXPECT_FALSE(never.measure(0, 10, counting)) << "nothing measured yet";
}

TEST(BlockStats, MeasuresABlockDirectly)
{
    const std::vector<float> samples{0.5f, -0.75f, 0.25f};
    const BlockStats::Block block = measureBlock(samples);
    EXPECT_FLOAT_EQ(block.peak, 0.75f);
    EXPECT_NEAR(block.sumSquares, 0.25 + 0.5625 + 0.0625, 1e-9);
}

} // namespace
} // namespace orizon::engine
