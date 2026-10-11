#include "engine/CaptureRing.h"
#include "engine/ChannelViews.h"

#include <array>
#include <gtest/gtest.h>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::ChannelView;

class ChannelViews : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const std::vector<float> left{1.0f, 0.5f, -1.0f, 0.0f};
        const std::vector<float> right{1.0f, -0.5f, 1.0f, 0.25f};
        std::array<const float*, 2> channels{left.data(), right.data()};
        ring.write(channels.data(), 2, 4);
    }

    std::vector<float> view(ChannelView which, int inputChannels = 2)
    {
        std::vector<float> destination(4);
        std::vector<float> scratch(4);
        EXPECT_TRUE(readView(ring, inputChannels, which, 0, destination, scratch));
        return destination;
    }

    CaptureRing ring{1024};
};

TEST_F(ChannelViews, DerivesLeftRightMidAndSide)
{
    EXPECT_EQ(view(ChannelView::kLeft), (std::vector<float>{1.0f, 0.5f, -1.0f, 0.0f}));
    EXPECT_EQ(view(ChannelView::kRight), (std::vector<float>{1.0f, -0.5f, 1.0f, 0.25f}));
    EXPECT_EQ(view(ChannelView::kMid), (std::vector<float>{1.0f, 0.0f, 0.0f, 0.125f}));
    EXPECT_EQ(view(ChannelView::kSide), (std::vector<float>{0.0f, 0.5f, -1.0f, -0.125f}));
}

TEST_F(ChannelViews, MonoInputShowsItsSingleChannelForEveryView)
{
    for (ChannelView which :
         {ChannelView::kLeft, ChannelView::kRight, ChannelView::kMid, ChannelView::kSide}) {
        EXPECT_EQ(view(which, 1), (std::vector<float>{1.0f, 0.5f, -1.0f, 0.0f}));
    }
}

TEST(ChannelViewSelection, KeepsTheRequestedSetForStereoOnly)
{
    EXPECT_EQ(effectiveViews(0b1100, 2), 0b1100);
    EXPECT_EQ(effectiveViews(0b1100, 1), 0b0001);
}

TEST(ChannelViewSelection, TriggersOnTheSingleChannelForMono)
{
    EXPECT_EQ(effectiveTriggerSource(ChannelView::kSide, 2), ChannelView::kSide);
    EXPECT_EQ(effectiveTriggerSource(ChannelView::kSide, 1), ChannelView::kLeft);
}

TEST_F(ChannelViews, FailsWhenTheRangeIsUnavailable)
{
    std::vector<float> destination(4);
    std::vector<float> scratch(4);
    EXPECT_FALSE(readView(ring, 2, ChannelView::kMid, 2, destination, scratch));
}

TEST_F(ChannelViews, ReadsWhatIsHeldAndSilenceForTheRest)
{
    std::vector<float> destination(4, 9.0f);
    std::vector<float> scratch(4);
    EXPECT_FALSE(readViewHeld(ring, 2, ChannelView::kLeft, 2, destination, scratch));
    EXPECT_EQ(destination, (std::vector<float>{-1.0f, 0.0f, 0.0f, 0.0f})) << "held, then silence";
    EXPECT_TRUE(readViewHeld(ring, 2, ChannelView::kLeft, 0, destination, scratch));
    EXPECT_EQ(destination, (std::vector<float>{1.0f, 0.5f, -1.0f, 0.0f}));
}

} // namespace
} // namespace orizon::engine
