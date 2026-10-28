#include "engine/HoldBuffer.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace orizon::engine {
namespace {

constexpr std::size_t kCapacity = 1024;

void writeRamp(CaptureRing& ring, std::size_t count)
{
    std::vector<float> left(count);
    std::vector<float> right(count);
    const std::uint64_t start = ring.writeIndex();
    for (std::size_t i = 0; i < count; ++i) {
        left[i] = static_cast<float>(start + i);
        right[i] = -static_cast<float>(start + i);
    }
    std::array<const float*, 2> channels{left.data(), right.data()};
    ring.write(channels.data(), 2, static_cast<int>(count));
}

TEST(HoldBuffer, CapturesTheNewestSamplesOfEveryChannel)
{
    CaptureRing ring(kCapacity);
    writeRamp(ring, 3000);
    HoldBuffer hold(kCapacity);
    ASSERT_TRUE(hold.capture(ring, 500));
    EXPECT_EQ(hold.endIndex(), 3000u);
    EXPECT_EQ(hold.beginIndex(), 2500u);

    std::vector<float> copy(500);
    ASSERT_TRUE(hold.read(0, 2500, copy));
    EXPECT_EQ(copy.front(), 2500.0f);
    EXPECT_EQ(copy.back(), 2999.0f);
    ASSERT_TRUE(hold.read(1, 2500, copy));
    EXPECT_EQ(copy.back(), -2999.0f);
}

TEST(HoldBuffer, StaysUnchangedWhileTheRingMovesOn)
{
    CaptureRing ring(kCapacity);
    writeRamp(ring, 800);
    HoldBuffer hold(kCapacity);
    ASSERT_TRUE(hold.capture(ring, 400));
    writeRamp(ring, 5000);

    std::vector<float> copy(400);
    ASSERT_TRUE(hold.read(0, 400, copy));
    EXPECT_EQ(copy.front(), 400.0f);
    EXPECT_EQ(copy.back(), 799.0f);
}

TEST(HoldBuffer, HoldsWhatExistsWhenLessThanRequested)
{
    CaptureRing ring(kCapacity);
    writeRamp(ring, 100);
    HoldBuffer hold(kCapacity);
    ASSERT_TRUE(hold.capture(ring, 500));
    EXPECT_EQ(hold.beginIndex(), 0u);
    EXPECT_EQ(hold.endIndex(), 100u);
}

TEST(HoldBuffer, RejectsReadsOutsideTheHeldRange)
{
    CaptureRing ring(kCapacity);
    writeRamp(ring, 1000);
    HoldBuffer hold(kCapacity);
    ASSERT_TRUE(hold.capture(ring, 200));
    std::vector<float> copy(10);
    EXPECT_FALSE(hold.read(0, 795, copy)) << "starts before the held range";
    EXPECT_FALSE(hold.read(0, 995, copy)) << "ends after the held range";
    EXPECT_FALSE(hold.read(2, 900, copy)) << "invalid channel";
}

// The longest span and its margin for a paced edge, whole, at every rate up to the highest.
TEST(HoldBuffer, HoldsTheLongestSpanAndItsMarginWithinCapacity)
{
    for (const double rate : {48000.0, 192000.0, 384000.0}) {
        const HoldBuffer hold(CaptureRing::capacityFor(rate));
        EXPECT_EQ(hold.holdLength(rate),
                  static_cast<std::size_t>(std::lround(kSecondsFreezeSeconds * rate)))
            << rate;
        EXPECT_GT(static_cast<double>(hold.holdLength(rate)), protocol::kFrozenHoldSeconds * rate)
            << "more than the longest span";
    }
    const HoldBuffer shortHold(kCapacity);
    EXPECT_EQ(shortHold.holdLength(48000.0), kCapacity) << "limited by capacity";
}

// Resized for the capture of another rate, it holds nothing until the next capture, then up to
// the new size.
TEST(HoldBuffer, ResizesForAnotherCapture)
{
    CaptureRing ring(kCapacity * 2);
    writeRamp(ring, 3000);
    HoldBuffer hold(kCapacity);
    ASSERT_TRUE(hold.capture(ring, kCapacity * 2));
    EXPECT_EQ(hold.endIndex() - hold.beginIndex(), kCapacity);
    hold.resize(kCapacity * 2);
    EXPECT_EQ(hold.endIndex(), hold.beginIndex()) << "emptied";
    EXPECT_EQ(hold.holdLength(48000.0), kCapacity * 2);
    ASSERT_TRUE(hold.capture(ring, kCapacity * 2));
    EXPECT_EQ(hold.beginIndex(), 3000u - kCapacity * 2);
    std::vector<float> copy(kCapacity * 2);
    ASSERT_TRUE(hold.read(1, hold.beginIndex(), copy));
    EXPECT_EQ(copy.back(), -2999.0f);
    hold.resize(kCapacity * 2);
    EXPECT_EQ(hold.endIndex(), 3000u) << "the same size keeps what it holds";
}

TEST(CaptureRing, ReportsItsReadableRange)
{
    CaptureRing ring(kCapacity);
    EXPECT_EQ(ring.beginIndex(), 0u);
    writeRamp(ring, 3000);
    EXPECT_EQ(ring.endIndex(), 3000u);
    EXPECT_EQ(ring.beginIndex(), 3000u - kCapacity);
}

} // namespace
} // namespace orizon::engine
