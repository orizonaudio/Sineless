#include "AllocationCounter.h"
#include "engine/CaptureRing.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <random>
#include <thread>
#include <vector>

namespace orizon::engine {
namespace {

constexpr std::size_t kSmallCapacity = 1024;

// The value written for sample index `index` of `channel`. Exact in float for the ranges used.
float valueAt(std::uint64_t index, int channel)
{
    return static_cast<float>((index % 100000) * 2 + static_cast<std::uint64_t>(channel));
}

// Writes `count` samples continuing from the ring's write index, using valueAt().
void writeBlock(CaptureRing& ring, int numChannels, int count)
{
    const std::uint64_t start = ring.writeIndex();
    std::array<std::vector<float>, CaptureRing::kMaxChannels> blocks;
    std::array<const float*, CaptureRing::kMaxChannels> pointers{};
    for (int channel = 0; channel < numChannels; ++channel) {
        auto& block = blocks[static_cast<std::size_t>(channel)];
        block.resize(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = valueAt(start + i, channel);
        }
        pointers[static_cast<std::size_t>(channel)] = block.data();
    }
    ring.write(pointers.data(), numChannels, count);
}

void expectRange(const CaptureRing& ring, int channel, std::uint64_t start, std::size_t count)
{
    std::vector<float> copy(count);
    ASSERT_TRUE(ring.read(channel, start, copy));
    for (std::size_t i = 0; i < count; ++i) {
        ASSERT_EQ(copy[i], valueAt(start + i, channel)) << "index " << start + i;
    }
}

TEST(CaptureRing, DefaultCapacityCoversFourSecondsAt192kHzWithMargin)
{
    constexpr std::size_t kFourSecondsAt192k = std::size_t{4} * 192000;
    EXPECT_GE(CaptureRing::kDefaultCapacity, kFourSecondsAt192k + 192000);
}

TEST(CaptureRing, StartsEmpty)
{
    CaptureRing ring(kSmallCapacity);
    std::vector<float> copy(1);
    EXPECT_EQ(ring.writeIndex(), 0u);
    EXPECT_FALSE(ring.read(0, 0, copy));
}

TEST(CaptureRing, ReadsBackWrittenSamplesPerChannel)
{
    CaptureRing ring(kSmallCapacity);
    writeBlock(ring, 2, 100);
    EXPECT_EQ(ring.writeIndex(), 100u);
    expectRange(ring, 0, 0, 100);
    expectRange(ring, 1, 0, 100);
    expectRange(ring, 1, 40, 20);
}

TEST(CaptureRing, KeepsTheNewestCapacitySamplesAcrossWrapAround)
{
    CaptureRing ring(kSmallCapacity);
    std::mt19937 random(7); // NOLINT(bugprone-random-generator-seed): reproducible
    std::uniform_int_distribution<int> blockSize(1, 300);
    while (ring.writeIndex() < 10 * kSmallCapacity) {
        writeBlock(ring, 2, blockSize(random));
    }
    const std::uint64_t end = ring.writeIndex();
    expectRange(ring, 0, end - kSmallCapacity, kSmallCapacity);
    expectRange(ring, 1, end - kSmallCapacity, kSmallCapacity);
}

TEST(CaptureRing, RejectsOverwrittenAndUnwrittenRanges)
{
    CaptureRing ring(kSmallCapacity);
    writeBlock(ring, 1, 3 * static_cast<int>(kSmallCapacity));
    const std::uint64_t end = ring.writeIndex();
    std::vector<float> copy(10);
    EXPECT_FALSE(ring.read(0, end - kSmallCapacity - 1, copy)) << "starts before the oldest";
    EXPECT_FALSE(ring.read(0, end - 5, copy)) << "ends after the newest";
    EXPECT_TRUE(ring.read(0, end - 10, copy));
    std::vector<float> tooLong(kSmallCapacity + 1);
    EXPECT_FALSE(ring.read(0, end - kSmallCapacity, tooLong));
    EXPECT_FALSE(ring.read(CaptureRing::kMaxChannels, end - 10, copy)) << "invalid channel";
}

TEST(CaptureRing, NullChannelAndMissingChannelRecordSilence)
{
    CaptureRing ring(kSmallCapacity);
    std::vector<float> left(8, 1.0f);
    std::array<const float*, 2> pointers{left.data(), nullptr};
    ring.write(pointers.data(), 2, 8);
    ring.write(pointers.data(), 1, 8); // Mono: channel 1 is not provided.

    std::vector<float> copy(16);
    ASSERT_TRUE(ring.read(1, 0, copy));
    for (float value : copy) {
        EXPECT_EQ(value, 0.0f);
    }
}

TEST(CaptureRing, ConvertsDoubleInputToFloat)
{
    CaptureRing ring(kSmallCapacity);
    std::vector<double> samples{0.5, -0.25, 1.0 / 3.0};
    const double* pointer = samples.data();
    ring.write(&pointer, 1, 3);

    std::vector<float> copy(3);
    ASSERT_TRUE(ring.read(0, 0, copy));
    EXPECT_EQ(copy[0], 0.5f);
    EXPECT_EQ(copy[1], -0.25f);
    EXPECT_EQ(copy[2], static_cast<float>(1.0 / 3.0));
}

TEST(CaptureRing, WriteDoesNotAllocate)
{
    CaptureRing ring(kSmallCapacity);
    std::vector<float> block(256, 0.5f);
    std::array<const float*, 2> pointers{block.data(), block.data()};
    std::size_t allocations = 0;
    {
        testing::AllocationCounter counter;
        ring.write(pointers.data(), 2, 256);
        allocations = counter.allocations() + counter.deallocations();
    }
    EXPECT_EQ(allocations, 0u);
}

// A writer races readers on a small ring so that overwrites happen constantly. Every read that
// succeeds must return exactly the values written; failed reads are expected and allowed.
TEST(CaptureRing, ConcurrentReadsNeverReturnTornData)
{
    constexpr std::uint64_t kTotalSamples = 4'000'000;
    CaptureRing ring(kSmallCapacity);
    std::atomic<bool> done{false};

    std::thread writer([&] {
        std::mt19937 random(11); // NOLINT(bugprone-random-generator-seed): reproducible
        std::uniform_int_distribution<int> blockSize(1, 128);
        while (ring.writeIndex() < kTotalSamples) {
            writeBlock(ring, 2, blockSize(random));
        }
        done.store(true);
    });

    std::uint64_t successfulReads = 0;
    std::uint64_t corruptReads = 0;
    std::mt19937 random(13); // NOLINT(bugprone-random-generator-seed): reproducible
    std::vector<float> copy(256);
    while (!done.load()) {
        const std::uint64_t end = ring.writeIndex();
        if (end < kSmallCapacity) {
            continue;
        }
        // Start anywhere in the valid window, including its oldest part, which is the most
        // likely to be overwritten during the copy.
        std::uniform_int_distribution<std::uint64_t> offset(0, kSmallCapacity - copy.size());
        const std::uint64_t start = end - kSmallCapacity + offset(random);
        const int channel = static_cast<int>(start % 2);
        if (!ring.read(channel, start, copy)) {
            continue;
        }
        ++successfulReads;
        for (std::size_t i = 0; i < copy.size(); ++i) {
            if (copy[i] != valueAt(start + i, channel)) {
                ++corruptReads;
                break;
            }
        }
    }
    writer.join();

    EXPECT_EQ(corruptReads, 0u);
    EXPECT_GT(successfulReads, 0u);
}

} // namespace
} // namespace orizon::engine
