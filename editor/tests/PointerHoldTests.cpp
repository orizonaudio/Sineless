#include "editor/PointerHold.h"

#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace orizon::editor {
namespace {

// The pointer as a hold sees it, with what the hold did to it, in order. Like the real one it stays
// where it is while frozen: tests move it only before the hold starts.
class FakePointer final : public PointerControl
{
public:
    ScreenPoint position() override { return at; }
    bool primaryButtonDown() override { return buttonDown; }
    double now() override { return clock; }
    void hide() override { calls.emplace_back("hide"); }
    void show() override { calls.emplace_back("show"); }
    void freeze() override { calls.emplace_back("freeze"); }
    void unfreeze() override { calls.emplace_back("unfreeze"); }
    void warp(ScreenPoint point) override
    {
        warpedTo = point;
        calls.emplace_back("warp");
    }

    ScreenPoint at{.x = 100, .y = 200};
    bool buttonDown = true;
    double clock = 10;
    std::vector<std::string> calls;
    ScreenPoint warpedTo{};
};

struct Sent
{
    double travel;
    bool fine;
};

class PointerHoldTest : public ::testing::Test
{
protected:
    void deliverAll()
    {
        while (!pending.empty()) {
            auto done = std::move(pending.front());
            pending.erase(pending.begin());
            done();
        }
    }

    // Presses at `point`, drags to `to` and starts the hold, as the page asks on its first step.
    void holdFrom(ScreenPoint point, ScreenPoint to)
    {
        hold.pressed(point);
        pointer.at = to;
        hold.start();
    }

    // Declared before the hold, which ends on destruction and so outlives them.
    FakePointer pointer;
    std::vector<Sent> sent;
    std::vector<std::function<void()>> pending;
    // Reports are delivered when the test says so, as the page answers a script later.
    PointerHold hold{pointer, [this](double travel, bool fine, std::function<void()> done) {
                         sent.push_back({.travel = travel, .fine = fine});
                         pending.push_back(std::move(done));
                     }};
};

TEST_F(PointerHoldTest, HidesAndFreezesThePointerWhenTheValueIsPressed)
{
    holdFrom({.x = 100, .y = 200}, {.x = 100, .y = 191});
    EXPECT_TRUE(hold.holding());
    EXPECT_EQ(pointer.calls, (std::vector<std::string>{"hide", "freeze"}));
}

TEST_F(PointerHoldTest, HoldsNothingWithoutAPressOrWithTheButtonUp)
{
    hold.start();
    EXPECT_FALSE(hold.holding());
    hold.pressed({.x = 100, .y = 200});
    pointer.buttonDown = false; // Released before the page's start arrived.
    hold.start();
    EXPECT_FALSE(hold.holding());
    EXPECT_TRUE(pointer.calls.empty());
    EXPECT_TRUE(sent.empty());
    // The press is still the one to hold if its button is down after all.
    pointer.buttonDown = true;
    hold.start();
    EXPECT_TRUE(hold.holding());
}

TEST_F(PointerHoldTest, HoldsEachPressOnce)
{
    holdFrom({.x = 100, .y = 200}, {.x = 100, .y = 191});
    hold.start(); // Twice: nothing more.
    EXPECT_EQ(pointer.calls, (std::vector<std::string>{"hide", "freeze"}));
    hold.end();
    pointer.calls.clear();
    hold.start(); // No new press.
    EXPECT_FALSE(hold.holding());
    EXPECT_TRUE(pointer.calls.empty());
}

TEST_F(PointerHoldTest, ReportsTheTravelSinceThePressAtOnceThenAddsTheMotion)
{
    holdFrom({.x = 100, .y = 200}, {.x = 103, .y = 191.5});
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_DOUBLE_EQ(sent[0].travel, -8.5); // Up is negative, as the page's clientY counts.
    deliverAll();
    hold.moved(-0.4, false, 11);
    hold.moved(-7.2, true, 12);
    deliverAll();
    ASSERT_GE(sent.size(), 2U);
    // Unrounded, so the page steps at the same 8 px as from clientY.
    EXPECT_DOUBLE_EQ(sent.back().travel, -16.1);
    EXPECT_TRUE(sent.back().fine);
}

TEST_F(PointerHoldTest, CountsNoMotionTheHandOverAlreadyRead)
{
    pointer.clock = 20;
    holdFrom({.x = 100, .y = 200}, {.x = 100, .y = 190});
    deliverAll();
    // Generated before the position was read, dispatched after: its motion is in the position.
    hold.moved(-5, false, 19.99);
    hold.moved(-5, false, 20);
    EXPECT_EQ(sent.size(), 1U);
    hold.moved(-5, false, 20.01);
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_DOUBLE_EQ(sent.back().travel, -15);
}

TEST_F(PointerHoldTest, ReportsNothingWhenNothingChanged)
{
    holdFrom({.x = 100, .y = 200}, {.x = 100, .y = 191});
    deliverAll();
    hold.moved(0, false, 11);
    EXPECT_EQ(sent.size(), 1U);
    hold.moved(0, true, 12); // Shift alone changes what a step is.
    EXPECT_EQ(sent.size(), 2U);
}

TEST_F(PointerHoldTest, SendsOneReportAtATimeAndThenTheLatest)
{
    holdFrom({.x = 100, .y = 200}, {.x = 100, .y = 191});
    for (int event = 0; event < 50; ++event) {
        hold.moved(-1, false, 11 + event * 0.001);
    }
    EXPECT_EQ(sent.size(), 1U); // The first is still on its way.
    deliverAll();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_DOUBLE_EQ(sent.back().travel, -59);
    deliverAll();
    EXPECT_EQ(sent.size(), 2U); // Nothing newer.
    hold.moved(-1, false, 12);
    EXPECT_EQ(sent.size(), 3U);
}

TEST_F(PointerHoldTest, FlushSendsTheLatestTravelAtOnce)
{
    holdFrom({.x = 100, .y = 200}, {.x = 100, .y = 191});
    hold.moved(-4, false, 11);
    EXPECT_EQ(sent.size(), 1U);
    hold.flush();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_DOUBLE_EQ(sent.back().travel, -13);
    deliverAll();
    EXPECT_EQ(sent.size(), 2U); // The flushed values are not sent again.
}

TEST_F(PointerHoldTest, ReturnsThePointerToThePressThenFreesAndShowsIt)
{
    holdFrom({.x = 100, .y = 200}, {.x = 104, .y = 180});
    pointer.calls.clear();
    hold.end();
    EXPECT_FALSE(hold.holding());
    // Moved while still frozen, as JUCE warps, so the pointer does not jump on unfreezing.
    EXPECT_EQ(pointer.calls, (std::vector<std::string>{"warp", "unfreeze", "show"}));
    EXPECT_DOUBLE_EQ(pointer.warpedTo.x, 100);
    EXPECT_DOUBLE_EQ(pointer.warpedTo.y, 200);
}

// A click holds from its press: a pointer that never moved is not warped, so nothing is posted that
// could delay a double-click's second press.
TEST_F(PointerHoldTest, LeavesAPointerWithinAPointOfThePressWhereItIs)
{
    holdFrom({.x = 100, .y = 200},
             {.x = 100.6, .y = 199.4}); // As a fresh read may differ from the press event.
    hold.end();
    EXPECT_EQ(pointer.calls, (std::vector<std::string>{"hide", "freeze", "unfreeze", "show"}));
}

TEST_F(PointerHoldTest, ReturnsAPointerMovedAPointInEitherAxis)
{
    for (const ScreenPoint moved :
         {ScreenPoint{.x = 101, .y = 200}, ScreenPoint{.x = 100, .y = 199}}) {
        pointer.calls.clear();
        holdFrom({.x = 100, .y = 200}, moved);
        hold.end();
        EXPECT_EQ(pointer.calls,
                  (std::vector<std::string>{"hide", "freeze", "warp", "unfreeze", "show"}));
    }
}

TEST_F(PointerHoldTest, EndsOnlyAHoldAndKeepsAPressNotYetHeld)
{
    hold.end();
    EXPECT_TRUE(pointer.calls.empty());
    holdFrom({.x = 100, .y = 200}, {.x = 100, .y = 191});
    hold.end();
    pointer.calls.clear();
    hold.end(); // Twice: the page's end after the release already ended it.
    EXPECT_TRUE(pointer.calls.empty());
    // A late end from the last drag leaves the next drag's press.
    hold.pressed({.x = 50, .y = 60});
    hold.end();
    hold.start();
    EXPECT_TRUE(hold.holding());
}

TEST_F(PointerHoldTest, IgnoresMotionAndPollsWithoutAHold)
{
    hold.moved(-20, false, 11);
    hold.poll();
    EXPECT_TRUE(sent.empty());
    EXPECT_TRUE(pointer.calls.empty());
}

TEST_F(PointerHoldTest, SendsNothingAfterTheHoldEnds)
{
    holdFrom({.x = 100, .y = 200}, {.x = 100, .y = 191});
    hold.moved(-4, false, 11); // Waits for the first report.
    hold.end();
    deliverAll();
    hold.moved(-4, false, 12);
    EXPECT_EQ(sent.size(), 1U);
}

// A report the last hold sent, answered during the next, must not free the next one's own.
TEST_F(PointerHoldTest, KeepsSendingOneReportAtATimeWhenTheLastHoldsReportArrivesLate)
{
    holdFrom({.x = 100, .y = 200}, {.x = 100, .y = 191});
    hold.end(); // Its report still on its way.
    holdFrom({.x = 100, .y = 200}, {.x = 100, .y = 191});
    ASSERT_EQ(sent.size(), 2U);
    pending.front()(); // The first hold's report arrives.
    pending.erase(pending.begin());
    hold.moved(-4, false, 11);
    EXPECT_EQ(sent.size(), 2U); // Still waiting for its own.
    deliverAll();
    EXPECT_EQ(sent.size(), 3U);
}

TEST_F(PointerHoldTest, EndsWhenThePollFindsTheButtonUp)
{
    holdFrom({.x = 100, .y = 200}, {.x = 100, .y = 191});
    hold.poll();
    EXPECT_TRUE(hold.holding());
    pointer.buttonDown = false; // Released where the release was never seen.
    hold.poll();
    EXPECT_FALSE(hold.holding());
    EXPECT_EQ(pointer.calls.back(), "show");
}

TEST(PointerHoldLifetime, NeverLeavesThePointerHiddenWhenDestroyed)
{
    FakePointer pointer;
    {
        PointerHold hold(pointer, [](double, bool, const std::function<void()>&) {});
        hold.pressed({.x = 100, .y = 200});
        pointer.at = {.x = 100, .y = 190};
        hold.start();
    }
    EXPECT_EQ(pointer.calls,
              (std::vector<std::string>{"hide", "freeze", "warp", "unfreeze", "show"}));
}

TEST(PointerHoldLifetime, IgnoresAReportDeliveredAfterItsHoldIsGone)
{
    std::vector<std::function<void()>> late;
    FakePointer pointer;
    {
        PointerHold hold(pointer, [&](double, bool, std::function<void()> done) {
            late.push_back(std::move(done));
        });
        hold.pressed({.x = 100, .y = 200});
        hold.start();
    }
    ASSERT_EQ(late.size(), 1U);
    late.front()(); // Must not touch the destroyed hold.
}

// A display left of or above the main one has negative coordinates.
TEST_F(PointerHoldTest, WorksOnADisplayLeftOfAndAboveTheMainOne)
{
    holdFrom({.x = -1500, .y = -300}, {.x = -1500, .y = -309.5});
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_DOUBLE_EQ(sent[0].travel, -9.5);
    hold.end();
    EXPECT_DOUBLE_EQ(pointer.warpedTo.x, -1500);
    EXPECT_DOUBLE_EQ(pointer.warpedTo.y, -300);
}

} // namespace
} // namespace orizon::editor
