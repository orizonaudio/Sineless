#include "AllocationCounter.h"
#include "engine/CaptureRing.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace orizon::engine {
namespace {

// The concurrent tests run their writer until the reader has made this many successful reads, so
// that a fast machine cannot finish writing before the reader has started; for at most kMaxWait,
// which only a ring whose reads never succeed reaches.
constexpr std::uint64_t kMinimumReads = 100;
constexpr auto kMaxWait = std::chrono::seconds(10);

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

TEST(CaptureRing, ReadersTakeAllButAnEighth)
{
    EXPECT_EQ(CaptureRing::readableLength(1024), 896u);
}

struct RateCapacity
{
    double rate;
    std::size_t capacity;
};

class CapacityForRate : public ::testing::TestWithParam<RateCapacity>
{
};

// The smallest capture up to the rate at which its readable part holds exactly the 4 s freeze
// (229.376 kHz), the largest above it, and no larger above the highest rate; the smallest for
// what is not a rate.
TEST_P(CapacityForRate, IsTheSmallestThatKeepsAFreeze)
{
    const auto [rate, capacity] = GetParam();
    EXPECT_EQ(CaptureRing::capacityFor(rate), capacity);
    if (rate > 0.0 && rate <= CaptureRing::kHighestSampleRate) {
        EXPECT_GE(static_cast<double>(CaptureRing::readableLength(capacity)),
                  protocol::kFrozenHoldSeconds * rate)
            << "keeps a freeze";
    }
}

constexpr std::size_t kSmallest = CaptureRing::kMinimumCapacity;
constexpr std::size_t kLargest = CaptureRing::kMaximumCapacity;
constexpr double kLastRateOfTheSmallest =
    static_cast<double>(CaptureRing::readableLength(kSmallest)) / protocol::kFrozenHoldSeconds;

INSTANTIATE_TEST_SUITE_P(
    Rates, CapacityForRate,
    ::testing::Values(RateCapacity{44100.0, kSmallest}, RateCapacity{48000.0, kSmallest},
                      RateCapacity{96000.0, kSmallest}, RateCapacity{192000.0, kSmallest},
                      RateCapacity{kLastRateOfTheSmallest, kSmallest},
                      RateCapacity{kLastRateOfTheSmallest + 1.0, kLargest},
                      RateCapacity{352800.0, kLargest}, RateCapacity{384000.0, kLargest},
                      RateCapacity{768000.0, kLargest}, RateCapacity{0.0, kSmallest},
                      RateCapacity{-48000.0, kSmallest},
                      RateCapacity{std::numeric_limits<double>::quiet_NaN(), kSmallest},
                      RateCapacity{std::numeric_limits<double>::infinity(), kLargest}),
    [](const ::testing::TestParamInfo<RateCapacity>& info) {
        return "Case" + std::to_string(info.index);
    });

TEST(CaptureRing, KeepsWhatItHoldsResizedToItsOwnSize)
{
    CaptureRing ring(kSmallCapacity);
    writeBlock(ring, 2, 300);
    ring.resize(kSmallCapacity);
    EXPECT_EQ(ring.capacity(), kSmallCapacity);
    EXPECT_EQ(ring.beginIndex(), 0u);
    expectRange(ring, 0, 0, 300);
}

// Resized for another rate, the capture starts empty at the newest index, which goes on from
// there; what came before is no longer held. Writing after it allocates nothing.
TEST(CaptureRing, StartsEmptyWhenResized)
{
    for (const std::size_t size : {kSmallCapacity * 2, kSmallCapacity / 2}) {
        SCOPED_TRACE(size);
        CaptureRing ring(kSmallCapacity);
        writeBlock(ring, 2, 3000);
        ring.resize(size);
        EXPECT_EQ(ring.capacity(), size);
        EXPECT_EQ(ring.writeIndex(), 3000u);
        EXPECT_EQ(ring.beginIndex(), 3000u) << "nothing held";
        std::vector<float> copy(10);
        EXPECT_FALSE(ring.read(0, 2990, copy)) << "written before the resize";

        std::vector<float> block(size, 0.5f);
        std::array<const float*, 2> pointers{block.data(), block.data()};
        std::size_t allocations = 0;
        {
            testing::AllocationCounter counter;
            ring.write(pointers.data(), 2, static_cast<int>(size / 2));
            allocations = counter.allocations() + counter.deallocations();
        }
        if (testing::kCountsAllocations) {
            EXPECT_EQ(allocations, 0u);
        }
        EXPECT_EQ(ring.beginIndex(), 3000u);
        writeBlock(ring, 2, static_cast<int>(size));
        EXPECT_EQ(ring.beginIndex(), 3000u + size / 2) << "full again";
        expectRange(ring, 1, ring.writeIndex() - size, size);
    }
}

// Readers copy while the capture is resized again and again between blocks (the writer never
// runs during a resize, as the host guarantees). A read that succeeds returns exactly what was
// written, one that meets a resize may fail, and none touches a freed buffer (AddressSanitizer)
// or races with the writer (ThreadSanitizer).
TEST(CaptureRing, ReadsAcrossResizesAreWholeOrFail)
{
    constexpr int kResizes = 4000;
    CaptureRing ring(kSmallCapacity);
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> successfulReads{0};
    std::atomic<std::uint64_t> corruptReads{0};

    std::thread reader([&] {
        std::vector<float> copy(200);
        while (!done.load()) {
            const std::uint64_t begin = ring.beginIndex();
            const std::uint64_t end = ring.writeIndex();
            if (end - begin < copy.size()) {
                continue;
            }
            const std::uint64_t start = end - copy.size();
            if (!ring.read(0, start, copy)) {
                continue;
            }
            successfulReads.fetch_add(1);
            for (std::size_t i = 0; i < copy.size(); ++i) {
                if (copy[i] != valueAt(start + i, 0)) {
                    corruptReads.fetch_add(1);
                    break;
                }
            }
        }
    });
    // At least kResizes, and on until the reader has had its reads, however fast the resizes run
    // beside it.
    const auto deadline = std::chrono::steady_clock::now() + kMaxWait;
    for (int resize = 0; resize < kResizes || (successfulReads.load() < kMinimumReads &&
                                               std::chrono::steady_clock::now() < deadline);
         ++resize) {
        ring.resize(resize % 2 == 0 ? kSmallCapacity * 2 : kSmallCapacity);
        for (int block = 0; block < 8; ++block) {
            writeBlock(ring, 2, 64);
        }
    }
    done.store(true);
    reader.join();

    EXPECT_EQ(corruptReads.load(), 0u);
    EXPECT_GE(successfulReads.load(), kMinimumReads);
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
    if (!testing::kCountsAllocations) {
        GTEST_SKIP() << "ThreadSanitizer's build counts no allocations (AllocationCounter.h)";
    }
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
    std::atomic<std::uint64_t> successfulReads{0};

    std::thread writer([&] {
        std::mt19937 random(11); // NOLINT(bugprone-random-generator-seed): reproducible
        std::uniform_int_distribution<int> blockSize(1, 128);
        // At least kTotalSamples, and on until the reader has had its reads, however fast the
        // writes run beside it.
        const auto deadline = std::chrono::steady_clock::now() + kMaxWait;
        while (ring.writeIndex() < kTotalSamples || (successfulReads.load() < kMinimumReads &&
                                                     std::chrono::steady_clock::now() < deadline)) {
            writeBlock(ring, 2, blockSize(random));
        }
        done.store(true);
    });

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
        successfulReads.fetch_add(1);
        for (std::size_t i = 0; i < copy.size(); ++i) {
            if (copy[i] != valueAt(start + i, channel)) {
                ++corruptReads;
                break;
            }
        }
    }
    writer.join();

    EXPECT_EQ(corruptReads, 0u);
    EXPECT_GE(successfulReads.load(), kMinimumReads);
}

// Every plugin format records a block through captureBlock: a channel the host flags silent is
// recorded as silence even when its buffer still holds old samples, the other channel as given.
TEST(CaptureRing, CapturesFlaggedChannelsAsSilence)
{
    for (const std::uint64_t silent : {std::uint64_t{0b01}, std::uint64_t{0b10}}) {
        CaptureRing ring(kSmallCapacity);
        const std::array<float, 4> left{0.1F, 0.2F, 0.3F, 0.4F};
        const std::array<float, 4> right{-0.1F, -0.2F, -0.3F, -0.4F};
        const std::array<const float*, 2> channels{left.data(), right.data()};
        captureBlock(ring, channels.data(), 2, silent, 4);
        for (int channel = 0; channel < 2; ++channel) {
            std::vector<float> copy(4);
            ASSERT_TRUE(ring.read(channel, 0, copy));
            const bool flagged =
                (silent & (std::uint64_t{1} << static_cast<unsigned>(channel))) != 0;
            const auto& given = channel == 0 ? left : right;
            for (std::size_t i = 0; i < copy.size(); ++i) {
                EXPECT_EQ(copy[i], flagged ? 0.0F : given[i]) << "channel " << channel;
            }
        }
    }
}

} // namespace
} // namespace orizon::engine
