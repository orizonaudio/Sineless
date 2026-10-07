#include "engine/CaptureRing.h"
#include "engine/SweepImage.h"

#include <array>
#include <gtest/gtest.h>
#include <vector>

namespace orizon::engine {
namespace {

// A ring whose samples hold their own index, so positions on screen show where each came from.
class SweepImageTest : public ::testing::Test
{
protected:
    void writeUpTo(std::uint64_t end)
    {
        std::vector<float> ramp;
        for (std::uint64_t index = ring.writeIndex(); index < end; ++index) {
            ramp.push_back(static_cast<float>(index));
        }
        std::array<const float*, 2> channels{ramp.data(), ramp.data()};
        ring.write(channels.data(), 2, static_cast<int>(ramp.size()));
    }

    std::vector<float> screen() const
    {
        std::vector<float> samples(image.length());
        EXPECT_TRUE(image.read(0, 0, samples));
        return samples;
    }

    CaptureRing ring{1024};
    SweepImage image;
};

TEST_F(SweepImageTest, LaysTheWindowOutInScreenOrder)
{
    writeUpTo(100);
    // The cycle started at sample 96: it sits at the left edge, the previous cycle after it.
    ASSERT_TRUE(image.update(ring, {.start = 90, .sampleCount = 10, .rotation = 6}, false));
    EXPECT_EQ(screen(), (std::vector<float>{96, 97, 98, 99, 90, 91, 92, 93, 94, 95}));
}

TEST_F(SweepImageTest, KeepsEarlierDrawingWhenTheClockJumps)
{
    writeUpTo(100);
    ASSERT_TRUE(image.update(ring, {.start = 90, .sampleCount = 10, .rotation = 6}, false));
    writeUpTo(103);
    // The transport restarted at sample 100: a new cycle starts there, at the left edge.
    ASSERT_TRUE(image.update(ring, {.start = 93, .sampleCount = 10, .rotation = 7}, false));
    EXPECT_EQ(screen(), (std::vector<float>{100, 101, 102, 99, 90, 91, 92, 93, 94, 95}))
        << "only the new samples moved in; the rest stayed where it was drawn";
}

TEST_F(SweepImageTest, WrapsNewSamplesAroundTheRightEdge)
{
    writeUpTo(100);
    ASSERT_TRUE(image.update(ring, {.start = 90, .sampleCount = 10, .rotation = 6}, false));
    writeUpTo(103);
    ASSERT_TRUE(image.update(ring, {.start = 93, .sampleCount = 10, .rotation = 8}, false));
    EXPECT_EQ(screen(), (std::vector<float>{101, 102, 98, 99, 90, 91, 92, 93, 94, 100}));
}

TEST_F(SweepImageTest, RebuildsWhenTheLengthChangesOrOnRequest)
{
    writeUpTo(100);
    ASSERT_TRUE(image.update(ring, {.start = 90, .sampleCount = 10, .rotation = 6}, false));
    ASSERT_TRUE(image.update(ring, {.start = 95, .sampleCount = 5, .rotation = 0}, false));
    EXPECT_EQ(screen(), (std::vector<float>{95, 96, 97, 98, 99}));
    ASSERT_TRUE(image.update(ring, {.start = 95, .sampleCount = 5, .rotation = 2}, true));
    EXPECT_EQ(screen(), (std::vector<float>{97, 98, 99, 95, 96}));
}

TEST_F(SweepImageTest, RedrawsEverythingAfterAGapOfAWholeSweep)
{
    writeUpTo(100);
    ASSERT_TRUE(image.update(ring, {.start = 90, .sampleCount = 10, .rotation = 6}, false));
    writeUpTo(200);
    ASSERT_TRUE(image.update(ring, {.start = 190, .sampleCount = 10, .rotation = 3}, false));
    EXPECT_EQ(screen(), (std::vector<float>{193, 194, 195, 196, 197, 198, 199, 190, 191, 192}));
}

TEST_F(SweepImageTest, RefusesReadsBeyondTheScreen)
{
    writeUpTo(100);
    ASSERT_TRUE(image.update(ring, {.start = 90, .sampleCount = 10}, false));
    std::vector<float> samples(4);
    EXPECT_FALSE(image.read(0, 8, samples));
    EXPECT_FALSE(image.read(2, 0, samples));
}

} // namespace
} // namespace orizon::engine
