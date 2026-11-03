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
// so it steps by exact refreshes and sits at or just before the callback. Frames and sweeps read
// the same D, so no read is later than a frame made after it. Until L is known, while paused,
// during a stall and once lost, the clock reads the engine's monotonic time; it never goes back.
//
// Times: `target` and `now` are on the display link's clock in seconds (CACurrentMediaTime);
// `engineNow` is monotonicNanos at the same instant. Only differences cross between the two.
//
// Threads: callback, setScreenRate, pause and lose on the display link's thread (or with it
// stopped); clock on the analysis worker. They share only atomics.
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

    // `screenRateHertz`: the display's maximum rate (NSScreen.maximumFramesPerSecond).
    explicit FramePacing(double screenRateHertz);

    struct Decision
    {
        // Open a request for the frame the next refresh shows.
        bool requestFrame = false;
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

    // The analyzer's clock: D, or the monotonic time as above; never less than it last returned.
    // Analysis worker only.
    [[nodiscard]] std::uint64_t clock(std::uint64_t engineNow);

    // n, frames per refresh. Display link thread.
    [[nodiscard]] int ticks() const { return n.value_or(1); }
    // The refresh rate in use. Display link thread.
    [[nodiscard]] double rateHertz() const { return rate; }
    // Callbacks earlier than L allowed (D set to the monotonic time instead). For perf.
    [[nodiscard]] std::uint64_t earlyCallbacks() const { return early; }

private:
    void restartLead();

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

    // Shared with the worker.
    std::atomic<std::uint64_t> displayNanos{0};
    std::atomic<std::uint64_t> lastCallbackNanos{0};
    std::atomic<std::uint64_t> callbackPeriodNanos{0};
    std::atomic<bool> display{false};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    // Worker only.
    std::uint64_t lastReturned = 0;
};

} // namespace orizon::engine
