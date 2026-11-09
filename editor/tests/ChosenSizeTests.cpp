// The size the user chose for the editor, apart from what the window shows: which sizes from the
// host are the user's, at every display scale Windows offers in steps (macOS's is always 1, and
// neither editor test fixture can change the scale), and how the lanes' minimum bounds them.

#include "editor/ChosenSize.h"

#include <gtest/gtest.h>
#include <optional>
#include <string>

namespace orizon::editor {
namespace {

// Twelve stacked lanes hold the window at this height (DESIGN.md 1.5).
constexpr int kLanesMinimum = 564;
constexpr int kNoLanes = ChosenSize::kMinimumHeight;

class ChosenSizeAtScale : public ::testing::TestWithParam<double>
{
protected:
    [[nodiscard]] double scale() const { return GetParam(); }
    [[nodiscard]] WindowSize units(double width, double height) const
    {
        return {.width = toWindowUnits(width, scale()), .height = toWindowUnits(height, scale())};
    }
    // An editor whose window has opened at the chosen size 1200 × 500.
    [[nodiscard]] static ChosenSize opened(double width = 1200, double height = 500)
    {
        ChosenSize chosen(engine::EditorSize{.width = width, .height = height});
        chosen.open();
        return chosen;
    }
};

TEST_P(ChosenSizeAtScale, TheWindowGrowsForLanesAndReturnsToTheChosenSize)
{
    ChosenSize chosen = opened();
    EXPECT_EQ(chosen.target(scale(), kLanesMinimum), units(1200, kLanesMinimum));
    EXPECT_EQ(chosen.target(scale(), kNoLanes), units(1200, 500));
}

TEST_P(ChosenSizeAtScale, AGrowForLanesNeverBecomesTheChosenSize)
{
    ChosenSize chosen = opened();
    const WindowSize grown = chosen.target(scale(), kLanesMinimum);
    chosen.requested(grown, false);
    EXPECT_FALSE(chosen.take(grown, scale(), kLanesMinimum));
    EXPECT_FALSE(chosen.take(grown, scale(), kLanesMinimum)) << "the host sends the reply again";
    const WindowSize back = chosen.target(scale(), kNoLanes);
    chosen.requested(back, false);
    EXPECT_FALSE(chosen.take(back, scale(), kNoLanes));
    EXPECT_EQ(chosen.width(), 1200);
    EXPECT_EQ(chosen.height(), 500);
}

// The lanes go before the host grants the room asked for them: the window should have the chosen
// size again, which it still has, so nothing more is asked. The host's late grant is still the
// reply to that request, not the user's resize.
TEST_P(ChosenSizeAtScale, ALateGrantForLanesThatHaveGoneIsNotTheUsers)
{
    ChosenSize chosen = opened();
    const WindowSize grown = chosen.target(scale(), kLanesMinimum);
    chosen.requested(grown, false);
    EXPECT_FALSE(chosen.take(grown, scale(), kNoLanes));
    EXPECT_EQ(chosen.width(), 1200);
    EXPECT_EQ(chosen.height(), 500);
}

TEST_P(ChosenSizeAtScale, TheGripsSizeIsTheUsersOnceTheHostGrantsIt)
{
    ChosenSize chosen = opened();
    const WindowSize dragged = units(1400, 700);
    chosen.requested(dragged, true);
    EXPECT_EQ(chosen.width(), 1200) << "not before the host grants it";
    EXPECT_TRUE(chosen.take(dragged, scale(), kNoLanes));
    EXPECT_EQ(chosen.target(scale(), kNoLanes), dragged);
}

TEST_P(ChosenSizeAtScale, TheHostsEdgeDragIsTheUsers)
{
    ChosenSize chosen = opened();
    const WindowSize dragged = units(1400, 700);
    EXPECT_TRUE(chosen.take(dragged, scale(), kNoLanes));
    EXPECT_EQ(chosen.target(scale(), kNoLanes), dragged) << "a window size comes back the same";
    EXPECT_FALSE(chosen.take(dragged, scale(), kNoLanes)) << "the host sends it again";
}

TEST_P(ChosenSizeAtScale, TheWindowAtItsSizeSentAgainIsNotAChange)
{
    ChosenSize chosen = opened();
    EXPECT_FALSE(chosen.take(chosen.target(scale(), kLanesMinimum), scale(), kLanesMinimum));
    EXPECT_FALSE(chosen.take(chosen.target(scale(), kNoLanes), scale(), kNoLanes));
    EXPECT_EQ(chosen.height(), 500);
}

// The example in issue #383: twelve stacked tracks hold 1200 × 500 at 1200 × 564; the user drags
// the window wider; leaving Stacked returns it to 1400 × 500.
TEST_P(ChosenSizeAtScale, ASidewaysDragAtTheLanesMinimumKeepsTheChosenHeight)
{
    ChosenSize chosen = opened();
    EXPECT_TRUE(chosen.take(units(1400, kLanesMinimum), scale(), kLanesMinimum));
    EXPECT_EQ(chosen.target(scale(), kNoLanes), units(1400, 500));
}

TEST_P(ChosenSizeAtScale, ADragDownToTheLanesMinimumLowersTheChosenHeightToIt)
{
    ChosenSize chosen = opened(1200, 700);
    EXPECT_TRUE(chosen.take(units(1200, kLanesMinimum), scale(), kLanesMinimum));
    EXPECT_EQ(chosen.target(scale(), kNoLanes), units(1200, kLanesMinimum));
}

TEST_P(ChosenSizeAtScale, ADragAboveTheLanesMinimumTakesTheHeight)
{
    ChosenSize chosen = opened();
    const WindowSize dragged = {.width = toWindowUnits(1200, scale()),
                                .height = toWindowUnits(kLanesMinimum, scale()) + 1};
    EXPECT_TRUE(chosen.take(dragged, scale(), kLanesMinimum));
    EXPECT_EQ(chosen.target(scale(), kNoLanes), dragged);
}

// An Audio Unit host may make the window smaller than the minimum while the user drags; the editor
// returns to the minimum when the drag ends. The user went as low as they could: the minimum.
TEST_P(ChosenSizeAtScale, ADragBelowTheMinimumStopsTheChosenSizeAtTheMinimum)
{
    ChosenSize chosen = opened(1200, 700);
    EXPECT_TRUE(chosen.take(units(600, 300), scale(), kLanesMinimum));
    EXPECT_EQ(chosen.width(), ChosenSize::kMinimumWidth);
    EXPECT_EQ(chosen.height(), kLanesMinimum);
    EXPECT_FALSE(
        chosen.take(units(ChosenSize::kMinimumWidth, kLanesMinimum), scale(), kLanesMinimum))
        << "the return to the minimum is the size the window should have";

    ChosenSize forced = opened(1200, 500); // The lanes were holding the height.
    forced.take(units(1200, 300), scale(), kLanesMinimum);
    EXPECT_EQ(forced.height(), 500) << "never raised to the minimum";
}

TEST_P(ChosenSizeAtScale, TheHostsPlacementBeforeTheWindowOpensIsNotTheUsers)
{
    ChosenSize chosen(engine::EditorSize{.width = 1200, .height = 500});
    const WindowSize remembered = units(1200, kLanesMinimum); // An old forced height.
    EXPECT_FALSE(chosen.take(remembered, scale(), kNoLanes));
    EXPECT_EQ(chosen.height(), 500);
    chosen.open();
    EXPECT_TRUE(chosen.take(remembered, scale(), kNoLanes)) << "once open, the user's";
    chosen.close();
    EXPECT_FALSE(chosen.take(units(1300, 600), scale(), kNoLanes)) << "closed again";
}

TEST_P(ChosenSizeAtScale, ADisplayChangeIsNotTheUsers)
{
    ChosenSize chosen = opened();
    const double next = scale() * 2.0;
    chosen.scaleChanged(scale());
    EXPECT_FALSE(chosen.take(chosen.target(next, kNoLanes), next, kNoLanes))
        << "the host follows the change itself";
    EXPECT_FALSE(chosen.take(chosen.target(scale(), kNoLanes), next, kNoLanes))
        << "a late size in the old display's units";
    EXPECT_EQ(chosen.width(), 1200);
    EXPECT_EQ(chosen.height(), 500);
}

TEST_P(ChosenSizeAtScale, AnotherSizeEndsTheRequestSoTheSameSizeLaterIsTheUsers)
{
    ChosenSize chosen = opened();
    const WindowSize grown = chosen.target(scale(), kLanesMinimum);
    chosen.requested(grown, false);
    EXPECT_TRUE(chosen.take(units(1300, 700), scale(), kLanesMinimum));
    const WindowSize sameAsRequest = {.width = grown.width, .height = grown.height + 10};
    EXPECT_TRUE(chosen.take(sameAsRequest, scale(), kLanesMinimum));
    EXPECT_TRUE(chosen.take(grown, scale(), kNoLanes)) << "the request no longer holds";
}

INSTANTIATE_TEST_SUITE_P(ChosenSize, ChosenSizeAtScale,
                         ::testing::Values(1.0, 1.25, 1.5, 1.75, 2.0),
                         [](const ::testing::TestParamInfo<double>& info) {
                             return "Scale" + std::to_string(static_cast<int>(info.param * 100));
                         });

TEST(ChosenSize, OpensAtTheDefaultWithoutASavedSize)
{
    const ChosenSize chosen(std::nullopt);
    EXPECT_EQ(chosen.width(), ChosenSize::kDefaultWidth);
    EXPECT_EQ(chosen.height(), ChosenSize::kDefaultHeight);
}

TEST(ChosenSize, OpensASavedSizeWithinTheMinimumAndTheLargest)
{
    for (const engine::EditorSize saved :
         {engine::EditorSize{.width = 900, .height = 300}, engine::EditorSize{},
          engine::EditorSize{.width = -5, .height = -5}}) {
        const ChosenSize chosen(saved);
        EXPECT_EQ(chosen.width(), ChosenSize::kMinimumWidth);
        EXPECT_EQ(chosen.height(), ChosenSize::kMinimumHeight);
    }
    const ChosenSize huge(engine::EditorSize{.width = 1e9, .height = 1e9});
    EXPECT_EQ(huge.width(), ChosenSize::kLargestSize);
    EXPECT_EQ(huge.height(), ChosenSize::kLargestSize);
}

} // namespace
} // namespace orizon::editor
