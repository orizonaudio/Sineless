#include "editor/DelayReturn.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

namespace orizon::editor {
namespace {

// A display whose pictures are shown in order, one a refresh: each lands `depth` refreshes after
// the target of the callback that drew it, and is known at the first callback after that. A late
// picture makes the queue one deeper for every picture after it; a callback that draws nothing,
// by a return or by itself, makes it one shorter again (unless `stuck`: a display whose shortest
// is a refresh more).
struct Display
{
    explicit Display(double rate) : rate(rate) {}

    double rate;
    DelayReturn policy;
    double time = 1000.0;
    std::uint64_t pictures = 0;
    int depth = DelayReturn::kShortestRefreshesAfterTarget;
    bool stuck = false;
    std::uint64_t returns = 0;
    double lastReturn = 0.0;
    // When a picture later than the shortest was first seen landed, since the last late().
    std::optional<double> laterSeen;

    struct Flying
    {
        std::uint64_t picture;
        double lands;
        int refreshes;
    };
    std::deque<Flying> flying;
    std::optional<DelayReturn::Landed> newest;

    void drain()
    {
        if (!stuck) {
            depth = std::max(depth - 1, DelayReturn::kShortestRefreshesAfterTarget);
        }
    }
    // One callback; true when it was a return.
    bool step(bool drawing = true)
    {
        time += 1.0 / rate;
        while (!flying.empty() && flying.front().lands < time) {
            newest = DelayReturn::Landed{.picture = flying.front().picture,
                                         .refreshes = flying.front().refreshes};
            flying.pop_front();
        }
        if (!laterSeen && newest &&
            newest->refreshes > DelayReturn::kShortestRefreshesAfterTarget) {
            laterSeen = time;
        }
        const bool back = policy.callback(
            {.time = time, .drawing = drawing, .lastPicture = pictures, .newest = newest});
        if (back) {
            ++returns;
            lastReturn = time;
        }
        if (back || !drawing) {
            drain();
        } else {
            ++pictures;
            flying.push_back({.picture = pictures,
                              .lands = time + static_cast<double>(depth) / rate,
                              .refreshes = depth});
        }
        return back;
    }
    // Callbacks for `seconds`; the returns made in them.
    std::uint64_t run(double seconds, bool drawing = true)
    {
        const std::uint64_t before = returns;
        const double until = time + seconds;
        while (time < until) {
            step(drawing);
        }
        return returns - before;
    }
    // A picture misses the window server's deadline: this one and every later one a refresh
    // later.
    void late()
    {
        ++depth;
        laterSeen.reset();
    }
    // A late picture, then callbacks up to the one before the last the wait leaves: the next
    // callback is one period short of the wait, and the one after it would be a return.
    void lateUntilJustBeforeTheReturn()
    {
        late();
        std::optional<double> seen;
        while (!seen) {
            step();
            seen = laterSeen;
        }
        while (time + 2.0 / rate < seen.value_or(time) + policy.waitSeconds() - 1e-9) {
            step();
        }
    }
    // Callbacks until a return; the seconds from now to it. Fails after `limit` seconds.
    double untilReturn(double limit = 400.0)
    {
        const double from = time;
        while (time - from < limit) {
            if (step()) {
                return time - from;
            }
        }
        ADD_FAILURE() << "no return in " << limit << " s";
        return limit;
    }
    // From a late picture to the return that follows it, in seconds, less the few refreshes the
    // late picture takes to land and be seen.
    double lateThenReturn()
    {
        late();
        return untilReturn();
    }
    // How long a late picture takes to land and be seen, and the callback after the wait, at
    // most: a few refreshes.
    [[nodiscard]] double slack() const { return 6.0 / rate; }
};

class DelayReturnAtRate : public ::testing::TestWithParam<double>
{
};

// After a late picture the traces trail by a refresh more than they need to: once that has lasted
// the first wait, one callback draws nothing, and not a callback sooner. A later state the link
// started in (the editor's opening is a late picture) is brought back the same, after the same
// wait.
TEST_P(DelayReturnAtRate, ComesBackOnceTheLaterDelayHasLasted)
{
    for (const bool fromTheStart : {false, true}) {
        SCOPED_TRACE(fromTheStart);
        Display display(GetParam());
        if (!fromTheStart) {
            display.run(5.0);
        }
        EXPECT_EQ(display.returns, 0U);
        const double took = display.lateThenReturn();
        EXPECT_GE(took, DelayReturn::kFirstWaitSeconds);
        EXPECT_LE(took, DelayReturn::kFirstWaitSeconds + display.slack());
        // And it stays back: the pictures land at the shortest again.
        EXPECT_EQ(display.depth, DelayReturn::kShortestRefreshesAfterTarget);
        EXPECT_EQ(display.run(300.0), 0U);
        EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), DelayReturn::kFirstWaitSeconds);
    }
}

// A callback with nothing to draw (no newer frame came) drains the queue by itself: drawing
// nothing again on purpose would repeat a picture for nothing. Also when it comes just as the
// wait is over, with pictures drawn before it still on their way and landing late: they are not a
// later state of the pictures after it.
TEST_P(DelayReturnAtRate, StartsCountingAgainWhenACallbackDrewNothing)
{
    Display display(GetParam());
    display.run(5.0);
    display.lateUntilJustBeforeTheReturn();
    EXPECT_FALSE(display.step(false));
    for (int callback = 0; callback < 6; ++callback) {
        EXPECT_FALSE(display.step()) << callback << " callbacks after the one that drew nothing";
    }
    EXPECT_EQ(display.run(300.0), 0U);
    // A picture late again after it waits the whole wait from there.
    const double took = display.lateThenReturn();
    EXPECT_GE(took, display.policy.waitSeconds());
}

// A view that keeps losing the shorter delay soon after getting it is at the window server's
// deadline: each return there adds a repeated picture, so each one waits twice as long as the
// last, up to the longest wait and no further. A loss after it held a minute is not "again".
TEST_P(DelayReturnAtRate, WaitsTwiceAsLongEachTimeItIsLostWithinAMinute)
{
    Display display(GetParam());
    display.run(5.0);
    double expected = DelayReturn::kFirstWaitSeconds;
    for (int loss = 0; loss < 9; ++loss) {
        SCOPED_TRACE(loss);
        const double took = display.lateThenReturn();
        EXPECT_GE(took, expected);
        EXPECT_LE(took, expected + display.slack());
        EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), expected);
        display.run(1.0); // Held a second, then lost again.
        expected = std::min(2.0 * expected, DelayReturn::kLongestWaitSeconds);
    }
    EXPECT_DOUBLE_EQ(expected, DelayReturn::kLongestWaitSeconds);
}

TEST_P(DelayReturnAtRate, LeavesTheWaitWhenItIsLostAfterAMinute)
{
    Display display(GetParam());
    display.run(5.0);
    display.lateThenReturn();
    // Held just over a minute: the first wait cannot halve, and the loss does not double it.
    display.run(DelayReturn::kHeldSeconds + 1.0);
    display.lateThenReturn();
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), DelayReturn::kFirstWaitSeconds);
}

// A busy machine flips the delay with no return involved: a late picture, then a callback with
// nothing to draw (its frame came late) that drains the queue, then the next late picture. A longer
// wait saves none of those repeats, so they leave it alone, and the traces come back as soon after
// the load as before it. A loss after a return counts, once: the flip that follows it does not
// count again. A freeze (callbacks with nothing to draw while the pictures are not later) reaches
// nothing, so a late picture after it does not lengthen the wait, and the frozen time does not
// shorten it.
TEST_P(DelayReturnAtRate, LeavesTheWaitWhenItIsLostAfterComingBackByItself)
{
    Display display(GetParam());
    display.run(5.0);
    for (int flip = 0; flip < 4; ++flip) {
        display.late();
        display.run(1.0);
        display.step(false); // Comes back by itself.
        display.run(1.0);
    }
    const double took = display.lateThenReturn();
    EXPECT_LE(took, DelayReturn::kFirstWaitSeconds + display.slack());
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), DelayReturn::kFirstWaitSeconds);

    // A callback with nothing to draw straight after that return, before its first picture
    // landed: which of the two brought the shorter delay is not known, so a loss leaves the wait.
    display.step(false);
    display.run(1.0);
    display.late();
    display.run(1.0);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), DelayReturn::kFirstWaitSeconds);
    display.untilReturn();

    // Lost a second after that return: twice the first wait.
    display.run(1.0);
    display.late();
    display.run(1.0);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), 2.0 * DelayReturn::kFirstWaitSeconds);
    // Back by itself and lost again: still twice.
    display.step(false);
    display.run(1.0);
    display.late();
    display.run(1.0);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), 2.0 * DelayReturn::kFirstWaitSeconds);
    display.untilReturn();

    // Frozen for ten minutes just after: nothing earned, so the wait is as it was.
    display.run(1.0);
    display.run(600.0, false);
    display.run(1.0);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), 2.0 * DelayReturn::kFirstWaitSeconds);

    // A fresh display frozen and unfrozen, then a late picture: the first wait, not twice it.
    Display frozen(GetParam());
    frozen.run(5.0);
    frozen.run(3.0, false);
    frozen.run(1.0);
    frozen.lateThenReturn();
    EXPECT_DOUBLE_EQ(frozen.policy.waitSeconds(), DelayReturn::kFirstWaitSeconds);
}

// A view that got lighter earns the short wait back: half for each minute the shorter delay
// holds, down to the first wait and no further.
TEST_P(DelayReturnAtRate, HalvesTheWaitForEachMinuteItHolds)
{
    Display display(GetParam());
    display.run(5.0);
    for (int loss = 0; loss < 5; ++loss) {
        display.lateThenReturn();
        display.run(1.0);
    }
    ASSERT_DOUBLE_EQ(display.policy.waitSeconds(), 16.0 * DelayReturn::kFirstWaitSeconds);
    display.run(DelayReturn::kHeldSeconds - 2.0);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), 16.0 * DelayReturn::kFirstWaitSeconds);
    display.run(3.0);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), 8.0 * DelayReturn::kFirstWaitSeconds);
    display.run(2.0 * DelayReturn::kHeldSeconds);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), 2.0 * DelayReturn::kFirstWaitSeconds);
    display.run(10.0 * DelayReturn::kHeldSeconds);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), DelayReturn::kFirstWaitSeconds);
}

// A return after which the pictures land no sooner showed a repeated picture for nothing: a view
// that cannot make the shorter delay just then (a burst of load), or a display whose shortest is a
// refresh more. It did not pay, so the next one waits twice as long, as after a return whose gain
// was lost, up to the longest wait and no further. It is still made, so a view that gets lighter
// is noticed.
TEST_P(DelayReturnAtRate, WaitsTwiceAsLongAfterAReturnThatGainedNothing)
{
    Display display(GetParam());
    display.run(5.0);
    display.late();
    display.stuck = true;
    display.untilReturn();
    double expected = DelayReturn::kFirstWaitSeconds;
    for (int tries = 0; tries < 8; ++tries) {
        SCOPED_TRACE(tries);
        expected = std::min(2.0 * expected, DelayReturn::kLongestWaitSeconds);
        const double took = display.untilReturn();
        EXPECT_GE(took, expected);
        EXPECT_LE(took, expected + display.slack());
    }
    EXPECT_DOUBLE_EQ(expected, DelayReturn::kLongestWaitSeconds);
    // The view got lighter: this one gains, and the pictures stay at the shortest.
    display.stuck = false;
    display.untilReturn();
    EXPECT_EQ(display.depth, DelayReturn::kShortestRefreshesAfterTarget);
    EXPECT_EQ(display.run(300.0), 0U);
}

// The pictures drawn before a return are still on their way when it is made and land as late as
// the ones before them: they say nothing of whether it gained.
TEST_P(DelayReturnAtRate, JudgesAReturnByThePicturesDrawnAfterIt)
{
    Display display(GetParam());
    display.run(5.0);
    display.lateThenReturn();
    // Past the landing of every picture drawn before the return.
    display.run(1.0);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), DelayReturn::kFirstWaitSeconds);
    EXPECT_EQ(display.returns, 1U);
}

// A picture late straight after a return lands where the pictures before it did: the return
// gained nothing that lasted a picture, and the next waits twice as long, as it would had the
// picture been late a moment later.
TEST_P(DelayReturnAtRate, TakesALatePictureStraightAfterAReturnAsNothingGained)
{
    Display display(GetParam());
    display.run(5.0);
    display.lateThenReturn();
    display.late();
    display.run(1.0);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), 2.0 * DelayReturn::kFirstWaitSeconds);
    // A late picture again while that one is counted on changes nothing more. The next return
    // gains; lost a second later, it doubles the wait once more, each return once.
    display.late();
    display.run(1.0);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), 2.0 * DelayReturn::kFirstWaitSeconds);
    display.untilReturn();
    display.untilReturn();
    display.run(1.0);
    EXPECT_EQ(display.depth, DelayReturn::kShortestRefreshesAfterTarget);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), 2.0 * DelayReturn::kFirstWaitSeconds);
    display.late();
    display.run(1.0);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), 4.0 * DelayReturn::kFirstWaitSeconds);
}

// Two refreshes late, a return gains one and is still late: it is brought back again after the
// same wait, with nothing reached in between.
TEST_P(DelayReturnAtRate, ComesBackAgainFromTwoRefreshesLate)
{
    Display display(GetParam());
    display.run(5.0);
    display.late();
    display.late();
    display.untilReturn();
    const double took = display.untilReturn();
    EXPECT_GE(took, DelayReturn::kFirstWaitSeconds);
    EXPECT_LE(took, DelayReturn::kFirstWaitSeconds + display.slack());
    display.run(1.0);
    EXPECT_EQ(display.depth, DelayReturn::kShortestRefreshesAfterTarget);
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), DelayReturn::kFirstWaitSeconds);
}

// Hidden and shown again, the layer starts over: the later state before the pause is not counted
// on, the pictures after it reach nothing, and a return that was being judged is forgotten (a
// late picture after the pause is not "gained nothing"). What the view has shown of itself, the
// wait, is kept.
TEST_P(DelayReturnAtRate, StartsAgainAfterAPause)
{
    Display display(GetParam());
    display.run(5.0);
    for (int loss = 0; loss < 2; ++loss) {
        display.lateThenReturn();
        display.run(1.0);
    }
    // Late a third time, and paused before the wait is over: counted from the start after it.
    display.late();
    display.run(3.0);
    const double wait = display.policy.waitSeconds();
    ASSERT_DOUBLE_EQ(wait, 4.0 * DelayReturn::kFirstWaitSeconds);
    display.policy.pause();
    display.time += 30.0;
    const double took = display.untilReturn();
    EXPECT_GE(took, wait);
    EXPECT_LE(took, wait + display.slack());
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), wait);

    // Paused with that return still being judged, and late again when shown: the wait is not the
    // longest, and the pictures at the shortest before the late one reached nothing.
    display.policy.pause();
    display.time += 30.0;
    display.run(1.0);
    display.lateThenReturn();
    EXPECT_DOUBLE_EQ(display.policy.waitSeconds(), wait);
}

TEST_P(DelayReturnAtRate, StartsFromTheFirstWaitOnAnotherDisplay)
{
    Display display(GetParam());
    display.run(5.0);
    for (int loss = 0; loss < 4; ++loss) {
        display.lateThenReturn();
        display.run(1.0);
    }
    ASSERT_GT(display.policy.waitSeconds(), DelayReturn::kFirstWaitSeconds);
    display.policy.displayChanged();
    display.time += 1.0;
    display.run(1.0);
    const double took = display.lateThenReturn();
    EXPECT_LE(took, DelayReturn::kFirstWaitSeconds + display.slack());
}

// No drawable was free at a callback that was to draw: nothing was drawn, which drains the queue
// as any callback with nothing to draw does, also just as the wait is over.
TEST_P(DelayReturnAtRate, TakesARefusedDrawableAsACallbackThatDrewNothing)
{
    Display display(GetParam());
    display.run(5.0);
    display.lateUntilJustBeforeTheReturn();
    // The callback meant to draw, and was refused its drawable.
    display.time += 1.0 / display.rate;
    EXPECT_FALSE(display.policy.callback({.time = display.time,
                                          .drawing = true,
                                          .lastPicture = display.pictures,
                                          .newest = display.newest}));
    display.policy.drewNothing(display.pictures);
    display.drain();
    for (int callback = 0; callback < 6; ++callback) {
        EXPECT_FALSE(display.step()) << callback << " callbacks after the refused drawable";
    }
    EXPECT_EQ(display.run(300.0), 0U);
}

INSTANTIATE_TEST_SUITE_P(Rates, DelayReturnAtRate, ::testing::Values(60.0, 75.0, 90.0));

// Above 95 Hz a frame is shown for two refreshes or more (AD-12), so every other callback has
// nothing to draw and the queue drains by itself each frame: the later state cannot hold, and a
// return there would only repeat a picture. None is made, however late the pictures land.
TEST(DelayReturnAtHighRates, NeverComesBackWhereEveryOtherCallbackDrawsNothing)
{
    for (const double rate : {120.0, 144.0, 240.0}) {
        SCOPED_TRACE(rate);
        Display display(rate);
        display.stuck = true; // Every picture lands late, whatever drains.
        display.late();
        const int callbacksAFrame = rate > 200.0 ? 3 : 2;
        for (int callback = 0; callback < static_cast<int>(300.0 * rate); ++callback) {
            EXPECT_FALSE(display.step(callback % callbacksAFrame == 0));
        }
        EXPECT_EQ(display.returns, 0U);
    }
}

// Where a picture landed is read to the nearest refresh: presented times wander a hundredth of
// a period either side of a whole one, and a reading that took the nearer side for the further
// would see a later state where there is none, or none where there is one.
TEST(RefreshesAfterTarget, ReadsWhereAPictureLanded)
{
    for (const double rate : {60.0, 75.0, 90.0}) {
        SCOPED_TRACE(rate);
        const double period = 1.0 / rate;
        const double target = 5000.0;
        for (const int refreshes : {0, 1, 2, 3}) {
            for (const double off : {-0.49, -0.01, 0.0, 0.01, 0.49}) {
                if (refreshes == 0 && off < -0.4) {
                    continue; // Before the target: below.
                }
                EXPECT_EQ(
                    refreshesAfterTarget(target + (static_cast<double>(refreshes) + off) * period,
                                         target, period),
                    refreshes)
                    << refreshes << " " << off;
            }
            EXPECT_EQ(
                refreshesAfterTarget(target + (static_cast<double>(refreshes) + 0.51) * period,
                                     target, period),
                refreshes + 1);
        }
    }
}

// A dropped drawable has no presented time, the first pictures after the link starts are drawn
// before the display's period is measured, and a time well before the target is no landing:
// none says where a picture landed.
TEST(RefreshesAfterTarget, ReadsNothingWithoutAPresentedTimeOrAPeriod)
{
    const double period = 1.0 / 60.0;
    EXPECT_FALSE(refreshesAfterTarget(0.0, 5000.0, period));
    EXPECT_FALSE(refreshesAfterTarget(5000.0 + period, 5000.0, 0.0));
    EXPECT_FALSE(refreshesAfterTarget(5000.0 - 0.6 * period, 5000.0, period));
    EXPECT_EQ(refreshesAfterTarget(5000.0 - 0.4 * period, 5000.0, period), 0);
}

} // namespace
} // namespace orizon::editor
