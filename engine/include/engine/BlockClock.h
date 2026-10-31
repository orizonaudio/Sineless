#pragma once

#include "engine/RealtimeSafety.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>

namespace orizon::engine {

// When a block of audio arrived: the capture index one past its last sample, the monotonic time
// (monotonicNanos) the audio thread recorded it at, and whether the host processes in real time.
// Offline and prefetch processing run at no steady rate, so their blocks are not paced.
struct BlockStampValue
{
    std::uint64_t index = 0;
    std::uint64_t nanos = 0;
    bool paced = false;
};

// The newest block's stamp. The audio thread publishes it after each block it captured; any other
// thread reads the latest without locks (a seqlock, as Timeline's).
class BlockStamp
{
public:
    // Audio thread.
    void publish(const BlockStampValue& stamp) noexcept ORIZON_NONBLOCKING;

    // Any non-audio thread; none before the first block.
    [[nodiscard]] std::optional<BlockStampValue> latest() const;

private:
    // Odd while the writer updates the fields.
    std::atomic<std::uint64_t> version{0};
    std::atomic<std::uint64_t> indexValue{0};
    std::atomic<std::uint64_t> nanosValue{0};
    std::atomic<bool> pacedValue{false};
    std::atomic<bool> publishedValue{false};

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

// The steady clock behind the audio: which capture index is due at a given time, from when blocks
// arrived. A delay-locked loop (Adriaensen, "Using a DLL to filter time", 2005; JACK2's frame
// timer and its coefficients) filters the arrival times of the stamps it is given, so the jitter
// of block delivery does not reach the estimate. Analysis thread; a plain class fed explicit
// stamps, so tests drive it with simulated hosts.
class BlockClock
{
public:
    // The loop's bandwidth (Hz), as JACK2's frame timer: low enough to smooth jittery deliveries,
    // high enough to follow a clock drift within seconds.
    static constexpr double kBandwidthHertz = 0.125;
    // Updates without a re-initialisation before the estimate is used: the loop has settled.
    static constexpr int kLockUpdates = 16;
    // A gap with no stamp above the larger of this and four periods means the host stopped calling
    // process: the loop starts over.
    static constexpr double kGapSeconds = 0.05;
    // An arrival this far from where the loop expects it, or two periods if longer, is a jump (a
    // block size change, a host that resumed): the loop starts over from it. Two periods, as a host
    // that delivers blocks in pairs delivers one a whole period late, every other period.
    static constexpr double kJumpSeconds = 0.005;
    static constexpr double kJumpPeriods = 2.0;

    // Feeds the latest stamp (the same one again changes nothing; blocks between two stamps fold
    // into one update). Returns whether it was a new update.
    bool observe(const BlockStampValue& stamp, double sampleRate);
    // Starts over, unlocked.
    void reset();

    [[nodiscard]] bool locked() const { return updates >= kLockUpdates; }
    // The time the last update spanned: about a block, or the blocks folded into it.
    [[nodiscard]] double periodSeconds() const { return lastPeriodNanos / 1e9; }
    // Whether the host has stopped delivering: no stamp for a gap at `nanos`.
    [[nodiscard]] bool stalled(std::uint64_t nanos) const;
    // How long with no block, in nanoseconds, means a host delivering blocks `periodNanos` apart
    // stopped calling process.
    [[nodiscard]] static double stallNanos(double periodNanos);
    // The capture index due at `nanos`, on the filtered line.
    [[nodiscard]] double estimate(std::uint64_t nanos) const;
    // At the last update: how far the line had run past the audio when the block came, in
    // samples (about one block; more for a late one).
    [[nodiscard]] double lastLead() const { return lead; }
    // Re-initialisations so far: a delay measured before one no longer applies.
    [[nodiscard]] std::uint64_t restarts() const { return restartCount; }

private:
    void start(const BlockStampValue& stamp, double sampleRate);

    bool started = false;
    double rate = 0.0;
    std::uint64_t lastIndex = 0;   // The index of the last update.
    std::uint64_t lastArrival = 0; // When it arrived (raw).
    double lastPeriodNanos = 0.0;  // The time its update spanned.
    double filteredTime = 0.0;     // The loop's time for lastIndex (ns).
    double nanosPerSample = 0.0;   // The loop's period per sample.
    int updates = 0;
    double lead = 0.0;
    std::uint64_t restartCount = 0;
};

// The right edge of a paced window: the clock's estimate, a measured delay behind, so it moves
// evenly between blocks, never past the audio captured and never backwards. Unpaced (before the
// clock locks, offline, the host stalled): the newest sample, as before.
class PacedEdge
{
public:
    // While the delay has to grow (a block later than any before), the edge slows by this fraction
    // rather than stalling.
    static constexpr double kRiseRate = 0.1;
    // While it can shrink, it does by this fraction of itself per second: the edge speeds up by a
    // hundredth of the delay per second, 1 ms in a second at the largest delay.
    static constexpr double kDecayRate = 0.01;
    // Room for the analysis thread seeing a block up to one of its 2 ms steps late.
    static constexpr double kMarginSeconds = 0.0005;
    // The delay follows the time between edges up to this: frames come at display rate, and a
    // longer gap (a freeze, a span not paced) is time nothing was drawn, which the delay does not
    // fall by.
    static constexpr double kLongestStepSeconds = 0.1;

    // Feeds the newest stamp, at every analysis step.
    void observe(const std::optional<BlockStampValue>& stamp, double sampleRate);
    // The edge at `nanos`, with `newest` the index one past the newest sample captured, for a
    // view at `sampleRate` whose largest delay is `capSeconds`.
    [[nodiscard]] std::uint64_t edge(std::uint64_t nanos, std::uint64_t newest, double capSeconds);
    // The view showed the newest sample at `nanos`, pacing not applying to it (a span shorter than
    // pacedSpanMinimumSeconds): once it applies again, the edge goes on from there, never back.
    std::uint64_t showNewest(std::uint64_t nanos, std::uint64_t newest)
    {
        pacedLast = false;
        lastEdge = newest;
        lastNanos = nanos;
        return newest;
    }
    // Whether the last edge was paced.
    [[nodiscard]] bool paced() const { return pacedLast; }
    // Another track drawn with this edge had `samples` less audio than the edge asked for (its
    // blocks come later): the next edge's delay rises to cover it, as it does for a late block of
    // its own, while the shortfall lasts.
    void coverShortfall(double samples)
    {
        shortfallTarget = std::max(shortfallTarget, delaySamples + samples);
    }
    // The delay in samples (0 unpaced).
    [[nodiscard]] double delay() const { return delaySamples; }
    // Whether this track's own blocks have stopped coming by `nanos` (the host stopped calling
    // process), as the block clock judges a stall.
    [[nodiscard]] bool stalled(std::uint64_t nanos) const { return clock.stalled(nanos); }
    // Whether another track, whose newest block arrived at `arrival`, stopped (suspended or
    // bypassed) by `nanos`, judged by this track's block period as a stall of its own is.
    [[nodiscard]] bool stopped(std::uint64_t arrival, std::uint64_t nanos) const
    {
        return static_cast<double>(nanos) - static_cast<double>(arrival) >
               BlockClock::stallNanos(clock.periodSeconds() * 1e9);
    }
    // Shares another edge's delay (a lane follows the own track's).
    void followDelay(double samples) { delaySamples = samples; }

private:
    BlockClock clock;
    bool lastPaced = false; // The last stamp's paced flag.
    double rate = 0.0;
    std::uint64_t knownRestarts = 0;
    double delaySamples = 0.0;
    double target = 0.0;          // What the delay rises toward.
    double shortfallTarget = 0.0; // What another track's shortfall asks of the next edge.
    double lockingPeak = 0.0;     // The largest lead seen while the clock locked.
    bool hadLock = false;
    std::optional<std::uint64_t> lastNanos;
    std::uint64_t lastEdge = 0;
    bool pacedLast = false;
};

} // namespace orizon::engine
