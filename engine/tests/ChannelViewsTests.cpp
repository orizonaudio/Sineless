#include "engine/CaptureRing.h"
#include "engine/ChannelViews.h"

#include <array>
#include <bit>
#include <gtest/gtest.h>
#include <string>
#include <utility>
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

// The views checked for samples that are not finite say whether any drawn view holds one: with L
// broken, R broken or both, for every set of views drawn, a view checked holds one exactly when a
// view drawn does, and only views drawn are checked.
TEST(ChannelViewSelection, ChecksViewsThatShowEveryBrokenChannel)
{
    // Whether each view holds a sample that is not finite, by the channels that do: L and R their
    // own, Mid and Side either.
    const auto broken = [](unsigned view, bool left, bool right) {
        switch (static_cast<ChannelView>(view)) {
        case ChannelView::kLeft:
            return left;
        case ChannelView::kRight:
            return right;
        case ChannelView::kMid:
        case ChannelView::kSide:
            return left || right;
        }
        return false;
    };
    for (unsigned drawn = 1; drawn < (1U << protocol::kChannelViewCount); ++drawn) {
        const unsigned checked = invalidCheckViews(static_cast<std::uint8_t>(drawn));
        SCOPED_TRACE(::testing::Message() << "drawn " << drawn << ", checked " << checked);
        EXPECT_NE(checked, 0U);
        EXPECT_EQ(checked & ~drawn, 0U) << "only views drawn";
        for (const auto [left, right] : {std::pair{true, false}, {false, true}, {true, true}}) {
            bool anyDrawn = false;
            bool anyChecked = false;
            for (unsigned view = 0; view < protocol::kChannelViewCount; ++view) {
                if (broken(view, left, right)) {
                    anyDrawn = anyDrawn || ((drawn >> view) & 1U) != 0;
                    anyChecked = anyChecked || ((checked >> view) & 1U) != 0;
                }
            }
            EXPECT_EQ(anyChecked, anyDrawn) << "left " << left << ", right " << right;
        }
    }
}

// The readouts measure only a trace on screen: the readout source when it is drawn, else the
// first view drawn in L, R, M, S order. Every source against every set of views drawn.
class ReadoutViewSelection : public ::testing::TestWithParam<ChannelView>
{
};

TEST_P(ReadoutViewSelection, MeasuresTheSourceOnlyWhenDrawn)
{
    const ChannelView requested = GetParam();
    const auto bit = static_cast<unsigned>(1U << static_cast<unsigned>(requested));
    for (unsigned drawn = 1; drawn < (1U << protocol::kChannelViewCount); ++drawn) {
        SCOPED_TRACE(drawn);
        const auto first = static_cast<ChannelView>(std::countr_zero(drawn));
        EXPECT_EQ(effectiveReadoutView(requested, static_cast<std::uint8_t>(drawn)),
                  (drawn & bit) != 0 ? requested : first);
    }
}

INSTANTIATE_TEST_SUITE_P(EverySource, ReadoutViewSelection,
                         ::testing::Values(ChannelView::kLeft, ChannelView::kRight,
                                           ChannelView::kMid, ChannelView::kSide),
                         [](const ::testing::TestParamInfo<ChannelView>& info) {
                             return std::string(protocol::kChannelViewNames.at(
                                 static_cast<std::size_t>(info.param)));
                         });

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
