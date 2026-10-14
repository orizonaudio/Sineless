#include "engine/CaptureRing.h"
#include "engine/SweepImage.h"

#include <array>
#include <gtest/gtest.h>
#include <vector>

namespace orizon::engine {
namespace {

// A ring whose left samples hold their own index, so positions on screen show where each came
// from, and whose right samples hold its negative, so the channels can be told apart.
class SweepImageTest : public ::testing::Test
{
protected:
    void writeUpTo(std::uint64_t end)
    {
        std::vector<float> ramp;
        std::vector<float> negated;
        for (std::uint64_t index = ring.writeIndex(); index < end; ++index) {
            ramp.push_back(static_cast<float>(index));
            negated.push_back(-static_cast<float>(index));
        }
        std::array<const float*, 2> channels{ramp.data(), negated.data()};
        ring.write(channels.data(), 2, static_cast<int>(ramp.size()));
    }

    std::vector<float> screen(int channel = 0) const
    {
        std::vector<float> samples(image.length());
        EXPECT_TRUE(image.read(channel, 0, samples));
        return samples;
    }

    // The right channel of what the left shows.
    static std::vector<float> negated(std::vector<float> left)
    {
        for (float& value : left) {
            value = -value;
        }
        return left;
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

TEST_F(SweepImageTest, FillsWhatTheSourceHoldsAndCompletesOverAPass)
{
    writeUpTo(2000); // The ring of 1024 holds samples 976 to 1999.
    ASSERT_TRUE(image.update(ring, {.start = 800, .sampleCount = 1200, .rotation = 0}, false));
    EXPECT_FALSE(image.complete());
    EXPECT_EQ(screen()[0], 0.0f) << "not held by the ring: silent until the playhead passes";
    EXPECT_EQ(screen()[176], 976.0f) << "the oldest sample held";
    EXPECT_EQ(screen()[1199], 1999.0f);

    writeUpTo(2400);
    ASSERT_TRUE(image.update(ring, {.start = 1200, .sampleCount = 1200, .rotation = 0}, false));
    EXPECT_TRUE(image.complete()) << "one whole pass has been drawn";
    EXPECT_EQ(screen()[799], 1599.0f) << "earlier drawing stays";
    EXPECT_EQ(screen()[800], 2000.0f) << "the new samples";
    EXPECT_EQ(screen()[1199], 2399.0f);
}

TEST_F(SweepImageTest, HoldsTheMidWithOneChannel)
{
    std::vector<float> left(100);
    std::vector<float> right(100);
    for (std::size_t i = 0; i < 100; ++i) {
        left[i] = static_cast<float>(i);
        right[i] = 3.0f * static_cast<float>(i); // Mid: twice the index.
    }
    std::array<const float*, 2> channels{left.data(), right.data()};
    ring.write(channels.data(), 2, 100);
    SweepImage mono(1);
    ASSERT_TRUE(mono.update(ring, {.start = 90, .sampleCount = 10, .rotation = 6}, false, 2));
    std::vector<float> samples(10);
    ASSERT_TRUE(mono.read(0, 0, samples));
    EXPECT_EQ(samples, (std::vector<float>{192, 194, 196, 198, 180, 182, 184, 186, 188, 190}));
    EXPECT_FALSE(mono.read(1, 0, samples)) << "one channel only";
    EXPECT_EQ(mono.channels(), 1);
    mono.clear();
    EXPECT_EQ(mono.length(), 0u);
}

TEST_F(SweepImageTest, KeepsItsLengthThroughASmallTempoChange)
{
    writeUpTo(3024); // The ring holds samples 2000 to 3023.
    // A 1024-sample sweep whose cycle began at 2500.
    ASSERT_TRUE(image.update(ring, {.start = 2000, .sampleCount = 1024, .rotation = 500}, false));
    EXPECT_EQ(screen()[0], 2500.0f);
    EXPECT_EQ(screen()[524], 2000.0f);
    writeUpTo(3074);
    // The tempo moved the length by one sample: no rebuild, and the new samples follow the cycle.
    ASSERT_TRUE(image.update(ring, {.start = 2049, .sampleCount = 1025, .rotation = 451}, false));
    EXPECT_EQ(image.length(), 1024u);
    EXPECT_EQ(screen()[0], 2500.0f);
    EXPECT_EQ(screen()[524], 3024.0f) << "placed relative to the cycle start";
    EXPECT_EQ(screen()[573], 3073.0f);
    EXPECT_EQ(screen()[574], 2050.0f) << "earlier drawing stays";
    // A larger change rebuilds.
    ASSERT_TRUE(image.update(ring, {.start = 2034, .sampleCount = 1040, .rotation = 466}, false));
    EXPECT_EQ(image.length(), 1040u);
}

// A lane takes this track's sweep length, which can be a sample off its own window's; for a short
// sweep that is more than the image would keep on its own, but the adopted length must stay.
TEST_F(SweepImageTest, KeepsALengthItWasGivenEvenForAShortSweep)
{
    writeUpTo(1000);
    ASSERT_TRUE(image.update(ring, {.start = 500, .sampleCount = 500}, false, 2, 499));
    EXPECT_EQ(image.length(), 499u);
    writeUpTo(1010);
    image.clearChanges();
    ASSERT_TRUE(image.update(ring, {.start = 510, .sampleCount = 500}, false, 2, 499));
    EXPECT_EQ(image.length(), 499u);
    std::uint64_t placed = 0;
    for (const SweepImage::Range& range : image.changes()) {
        placed += range.end - range.first;
    }
    EXPECT_EQ(placed, 10u) << "only the new samples placed, not rebuilt";
}

// A part shows over its positions instead of the image's samples, which stay underneath: reads
// and the changed positions follow it while it is shown and after it is cleared, and the next
// update clears it.
TEST_F(SweepImageTest, ShowsAPartOverItsPositionsAndKeepsTheImage)
{
    writeUpTo(100);
    ASSERT_TRUE(image.update(ring, {.start = 90, .sampleCount = 10}, false));
    const std::vector<float> drawn = screen();
    image.clearChanges();

    SweepImage part;
    ASSERT_TRUE(part.update(ring, {.start = 50, .sampleCount = 4}, false));
    ASSERT_TRUE(part.complete());
    image.showPart(part, 3);
    EXPECT_TRUE(image.hasPart());
    EXPECT_TRUE(image.partComplete());
    EXPECT_TRUE(image.showsPart(3, 4));
    EXPECT_FALSE(image.showsPart(3, 5));
    EXPECT_EQ(screen(), (std::vector<float>{90, 91, 92, 50, 51, 52, 53, 97, 98, 99}));
    EXPECT_EQ(screen(1), negated(screen())) << "each channel shows its own part";
    // A read starting inside the part, or ending in it.
    std::array<float, 3> inside{};
    ASSERT_TRUE(image.read(0, 5, inside));
    EXPECT_EQ(inside, (std::array<float, 3>{52, 53, 97}));
    ASSERT_TRUE(image.read(0, 1, inside));
    EXPECT_EQ(inside, (std::array<float, 3>{91, 92, 50}));
    ASSERT_EQ(image.changes().size(), 1U);
    EXPECT_EQ(image.changes().front().first, 3U);
    EXPECT_EQ(image.changes().front().end, 7U);
    image.clearChanges();

    image.clearPart();
    EXPECT_FALSE(image.hasPart());
    EXPECT_FALSE(image.partComplete());
    EXPECT_EQ(screen(), drawn);
    ASSERT_EQ(image.changes().size(), 1U);
    EXPECT_EQ(image.changes().front().first, 3U);
    EXPECT_EQ(image.changes().front().end, 7U);

    image.showPart(part, 3);
    writeUpTo(101);
    ASSERT_TRUE(image.update(ring, {.start = 91, .sampleCount = 10}, false));
    EXPECT_FALSE(image.hasPart()) << "an update clears the part";
}

// A part longer than the positions left is cut at the right edge; one of other channels, or from
// beyond the edge, is not shown; one still filling in shows as such.
TEST_F(SweepImageTest, ShowsOnlyAPartThatFits)
{
    writeUpTo(100);
    ASSERT_TRUE(image.update(ring, {.start = 90, .sampleCount = 10}, false));

    SweepImage part;
    ASSERT_TRUE(part.update(ring, {.start = 40, .sampleCount = 6}, false));
    image.showPart(part, 7);
    EXPECT_TRUE(image.showsPart(7, 3));
    EXPECT_EQ(screen(), (std::vector<float>{90, 91, 92, 93, 94, 95, 96, 40, 41, 42}));
    EXPECT_EQ(screen(1), negated(screen())) << "each channel shows its own part";

    image.showPart(part, 10);
    EXPECT_FALSE(image.hasPart()) << "beyond the edge";
    SweepImage mid(1);
    ASSERT_TRUE(mid.update(ring, {.start = 40, .sampleCount = 6}, false));
    image.showPart(mid, 2);
    EXPECT_FALSE(image.hasPart()) << "other channels";

    SweepImage filling;
    ASSERT_TRUE(filling.update(ring, {.start = -2, .sampleCount = 6}, false));
    ASSERT_FALSE(filling.complete());
    image.showPart(filling, 2);
    EXPECT_TRUE(image.hasPart());
    EXPECT_FALSE(image.partComplete());
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
