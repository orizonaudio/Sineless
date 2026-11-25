#pragma once

#include <cstdint>
#include <optional>

namespace orizon::editor {

// Where a picture landed: whole display refreshes from the target of the display link's callback
// that drew it to Metal's presented time, to the nearest. Nothing for a dropped drawable (a
// presented time of 0.0), before the display's period is known (0.0: the first pictures after the
// link starts or the window changes display), and for a time more than half a period before the
// target.
[[nodiscard]] std::optional<int> refreshesAfterTarget(double presentedTime, double target,
                                                      double periodSeconds);

// When the native traces come back to the shorter delay (AD-30). The layer's pictures are shown
// in order, one a refresh: one that missed the window server's deadline is shown a refresh later
// and so is every picture after it, until a display link callback draws nothing and the queue is
// one shorter again. This says when to draw nothing on purpose: once the later state has lasted
// a wait that grows while returns keep losing the shorter delay again, since each such loss is a
// repeated picture the later state would have absorbed.
//
// Told once a callback where the newest presented picture landed; times are the callbacks'
// targets, in seconds. Render thread only.
class DelayReturn
{
public:
    // The shortest a picture lands after its callback's target: one refresh, two after the
    // callback itself (measured at 60 Hz on a built-in Retina display and an external one).
    // Anything more is the later state.
    static constexpr int kShortestRefreshesAfterTarget = 1;
    // How long the later state lasts before the first return: past a hiccup's own length (the
    // editor opening, a window moved). Left alone it lasted 4 to 72 s in a light view in a host.
    static constexpr double kFirstWaitSeconds = 2.0;
    // The shorter delay lost within this long of a return reaching it is lost "again", and
    // doubles the wait; each time it holds this long, however reached, halves it. A light view
    // held it 24 to 176 s between late pictures, a view at the window server's deadline 4 to 50 s.
    // Lost after coming back by itself (a callback with nothing to draw), the wait stays: a busy
    // machine flips the delay that way every few seconds with no return involved (in a host, four
    // times in 80 s while other programs opened), and waiting longer saves none of those repeats.
    static constexpr double kHeldSeconds = 60.0;
    // The longest wait: a view that cannot hold or make the shorter delay climbs to it one
    // doubling a return, and is still tried this often, so one that gets lighter is noticed, at
    // one repeated picture each time. A minute: two left the
    // traces later for too long after a stretch of load (the owner's hand test). As long as
    // kHeldSeconds by chance: that one is time at the shorter delay, this one time at the later.
    static constexpr double kLongestWaitSeconds = 60.0;

    struct Landed
    {
        // The picture's number, counted from 1 as the layer draws them.
        std::uint64_t picture = 0;
        // refreshesAfterTarget of its presented time.
        int refreshes = 0;
    };

    struct Callback
    {
        // The callback's target time.
        double time = 0.0;
        // Whether this callback has a picture to draw.
        bool drawing = false;
        // The number of the last picture drawn; 0 before the first.
        std::uint64_t lastPicture = 0;
        // The newest picture presented, if any has been.
        std::optional<Landed> newest;
    };

    // One display link callback. True: draw nothing at this one (a return).
    [[nodiscard]] bool callback(const Callback& callback);
    // A callback that was to draw drew nothing after all (no drawable was free): the queue
    // drained as at a callback with nothing to draw.
    void drewNothing(std::uint64_t lastPicture);
    // The link paused (hidden, occluded): the pictures before say nothing of those after. The
    // wait is kept.
    void pause();
    // The window is on another display: everything starts again.
    void displayChanged();

    // The wait in use, and how many returns were made. For tests and the benchmark.
    [[nodiscard]] double waitSeconds() const { return wait; }
    [[nodiscard]] std::uint64_t returns() const { return made; }

private:
    void take(const Landed& landed, double time);
    void reach(bool byReturn);

    double wait = kFirstWaitSeconds;
    std::uint64_t made = 0;
    // Pictures numbered up to this were drawn before the queue last drained or the link paused,
    // and say nothing of the pictures after.
    std::uint64_t ignoreUpTo = 0;
    std::uint64_t counted = 0;
    bool restarted = true;
    std::optional<double> lastTime;
    // The last state known was the later one (kept through a callback that drew nothing, so the
    // picture after it can be seen to have reached the shorter delay).
    bool wasLater = false;
    int laterRefreshes = 0;
    // When the newest picture was first seen in the later state.
    std::optional<double> laterSince;
    // A return is being judged: where pictures landed before it.
    std::optional<int> judging;
    // Seconds of drawing callbacks since the shorter delay was reached, and the halvings they
    // have earned; nothing when it was not reached from the later state.
    std::optional<double> held;
    // A return reached it (its judgement found the shortest), not a callback with nothing to draw.
    bool heldByReturn = false;
    int halvings = 0;
};

} // namespace orizon::editor
