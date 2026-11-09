#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace orizon::engine {

// AD-12: frames are drawn on every n-th display refresh, n the smallest whole number keeping the
// refresh rate divided by n at or below protocol::kFullRateUpToHz. Once n is set (`previous`), it
// changes only when the rate is protocol::kLineHysteresis past a line. The page's ticksPerFrame
// (ui/src/render/render-loop.ts) is the same rule; both read
// protocol/tests/data/ticks-per-frame.json.
[[nodiscard]] int ticksPerFrame(double rateHertz, std::optional<int> previous = std::nullopt);

// The macOS editor's display clock (AD-30). A display link calls back on its own thread once per
// refresh with the time that refresh will be shown (its target); from those, FramePacing keeps the
// time every frame and every sweep update is placed for (D), decides which refreshes show a new
// frame, and gives the analysis worker its clock.
//
// D steps once per callback to the target less a fixed lead L (the callbacks' lead over their
// targets, measured over the first kLeadCallbacks after the link starts or the display changes),
// plus a callback period less the margin: the frame a callback asks for is made that much later,
// just before the next callback, which draws it, and is placed at the moment it is made rather
// than a refresh earlier. The margin follows how long frames take to make (frameMade), slowly
// enough that D steps by refreshes to within QS-14 (exactly, while it holds). D sits at or just
// before that moment; a read before it gets the time now, never a later one. Frames and sweeps read
// the same D, so no read is later than a frame made after it. Until L is known, while paused,
// during a stall and once lost, the clock reads the engine's monotonic time; it never goes back.
//
// Times: `target` and `now` are on the display link's clock in seconds (CACurrentMediaTime);
// `engineNow` is monotonicNanos at the same instant. Only differences cross between the two.
//
// Threads: callback, setScreenRate, pause and lose on the display link's thread (or with it
// stopped); clock and frameMade on the analysis worker. They share only atomics.
class FramePacing
{
public:
    // Callbacks whose lead over their targets is measured to set L.
    static constexpr int kLeadCallbacks = 30;
    // Steps between targets whose median gives the refresh rate; until then, the screen's rate.
    static constexpr std::size_t kRateSteps = 15;
    // Callback periods with no callback after which the clock follows the monotonic time, so the
    // sweeps keep advancing (FR-9.5) if the link stops without a pause. Above a skipped callback
    // (two periods) and the render thread's scheduling jitter.
    static constexpr double kStallCallbackPeriods = 4.0;
    // How long before the next callback the frame it draws is asked for, at least: room for a light
    // view's frame to be made (about a millisecond for a busy Multi view) and for the request's
    // timer to fire late (about a millisecond, measured). A heavier view's margin follows how long
    // its frames take (frameMade).
    static constexpr double kHandOffMarginSeconds = 0.004;
    // Above the slowest recent frame's time from when its request was due to ready (the timer's
    // lateness and the worker's wait included): room for one slower still. The floor less the
    // light view's millisecond.
    static constexpr double kHandOffSlackSeconds = 0.003;
    // The frames whose times set the margin, the slowest of them: a second of frames at 60 Hz
    // (Chromium's compositor keeps its stages' last 60 durations). A busy view's times have a long
    // tail (Compare with 15 tracks at 384 kHz: 5.8 ms at the 90th percentile, 7.3 at the 98th,
    // 12.4 at most), and a margin from the 90th or 97th percentile left 1 % of its frames missing
    // their refresh; a light view's one slow frame then asks earlier for a second.
    static constexpr std::size_t kMakeHistory = 60;
    // How far the margin moves per callback toward what the making times ask: D carries the
    // margin, so a change moves the frame's placement by as much. A quarter of QS-14's 1 ms per
    // frame (n callbacks a frame: up to n times this).
    static constexpr double kMarginSlewSeconds = 0.00025;

    // `screenRateHertz`: the display's maximum rate (NSScreen.maximumFramesPerSecond).
    explicit FramePacing(double screenRateHertz);

    struct Decision
    {
        // Open a request for the frame the next refresh shows.
        bool requestFrame = false;
        // When, in seconds from this callback, to open it: just before the next callback, counted
        // from when this one was due (its target less L), so a callback that ran late still asks
        // in time; requestDelaySeconds() until L is known.
        double requestAfterSeconds = 0.0;
    };

    // One display link callback. Steps D and says whether to ask for the next refresh's frame.
    Decision callback(double target, double now, std::uint64_t engineNow);

    // The window moved to another display: its rate, and L measured again.
    void setScreenRate(double screenRateHertz);
    // The link paused (hidden, occluded): the clock follows the monotonic time until the next
    // callback.
    void pause();
    // Native drawing stopped for good: the clock follows the monotonic time from now on.
    void lose();

    // How long the last frame asked for took, from the request to being ready. Any thread.
    void frameMade(std::uint64_t nanos);

    // The analyzer's clock: D, or the monotonic time as above; never less than it last returned.
    // Analysis worker only.
    [[nodiscard]] std::uint64_t clock(std::uint64_t engineNow);

    // How long after a callback to ask for the frame the next callback draws: a callback period
    // (as measured, n refreshes) less the margin, or none. Display link thread.
    [[nodiscard]] double requestDelaySeconds() const;
    // The margin in use: how long before the next callback its frame is asked for. Display link
    // thread.
    [[nodiscard]] double marginSeconds() const { return margin; }
    // n, frames per refresh. Display link thread.
    [[nodiscard]] int ticks() const { return n.value_or(1); }
    // The refresh rate in use. Display link thread.
    [[nodiscard]] double rateHertz() const { return rate; }
    // The display's refresh period as measured from the link's targets, in seconds: a refresh,
    // not the step between callbacks (n refreshes at n > 1); 0 until measured. Any thread.
    [[nodiscard]] double displayPeriodSeconds() const
    {
        return displayPeriod.load(std::memory_order_relaxed);
    }
    // Callbacks earlier than L allowed (D set to the monotonic time instead). For perf.
    [[nodiscard]] std::uint64_t earlyCallbacks() const { return early; }
    // How often the clock has been read: the analyzer reads it for every frame it places. Any
    // thread; for tests.
    [[nodiscard]] std::uint64_t clockReads() const { return reads.load(std::memory_order_relaxed); }

private:
    void restartLead();
    // Takes a time frameMade reported and moves the margin toward what the recent times ask.
    void followMaking(double callbackPeriod);
    [[nodiscard]] double callbackPeriodSeconds() const;

    // Display link thread.
    double rate;
    std::optional<int> n;
    std::optional<double> lastTarget;
    std::array<double, kRateSteps> steps{};
    std::size_t stepCount = 0;
    std::size_t nextStep = 0;
    std::array<double, kRateSteps> callbackSteps{};
    std::size_t callbackStepCount = 0;
    std::size_t nextCallbackStep = 0;
    double maxLead = 0.0;
    int leadCount = 0;
    std::optional<double> lead;
    int periodsSinceShown = 0;
    bool frameDue = false;
    std::uint64_t early = 0;
    bool lost = false;
    std::array<double, kMakeHistory> makeTimes{};
    std::size_t makeCount = 0;
    std::size_t nextMake = 0;
    double margin = kHandOffMarginSeconds;

    // Shared with the worker.
    std::atomic<std::uint64_t> displayNanos{0};
    std::atomic<std::uint64_t> lastCallbackNanos{0};
    std::atomic<std::uint64_t> callbackPeriodNanos{0};
    std::atomic<double> displayPeriod{0.0};
    // frameMade's: the slowest time reported since the callback last took one, plus 1 (0: none).
    std::atomic<std::uint64_t> slowestMade{0};
    std::atomic<bool> display{false};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    // Worker only.
    std::uint64_t lastReturned = 0;
    std::atomic<std::uint64_t> reads{0};
};

} // namespace orizon::engine
