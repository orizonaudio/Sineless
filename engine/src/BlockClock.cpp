#include "engine/BlockClock.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace orizon::engine {

namespace {

constexpr double kNanosPerSecond = 1e9;

} // namespace

void BlockStamp::publish(const BlockStampValue& stamp) noexcept ORIZON_NONBLOCKING
{
    const std::uint64_t start = version.load(std::memory_order_relaxed);
    version.store(start + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    indexValue.store(stamp.index, std::memory_order_relaxed);
    nanosValue.store(stamp.nanos, std::memory_order_relaxed);
    pacedValue.store(stamp.paced, std::memory_order_relaxed);
    publishedValue.store(true, std::memory_order_relaxed);
    version.store(start + 2, std::memory_order_release);
}

std::optional<BlockStampValue> BlockStamp::latest() const
{
    while (true) {
        const std::uint64_t before = version.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue; // A write is in progress.
        }
        const BlockStampValue stamp{.index = indexValue.load(std::memory_order_relaxed),
                                    .nanos = nanosValue.load(std::memory_order_relaxed),
                                    .paced = pacedValue.load(std::memory_order_relaxed)};
        const bool published = publishedValue.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (version.load(std::memory_order_relaxed) == before) {
            return published ? std::optional(stamp) : std::nullopt;
        }
    }
}

void BlockClock::reset()
{
    if (started) {
        ++restartCount;
    }
    started = false;
    updates = 0;
    lead = 0.0;
}

void BlockClock::start(const BlockStampValue& stamp, double sampleRate)
{
    reset();
    started = true;
    rate = sampleRate;
    lastIndex = stamp.index;
    lastArrival = stamp.nanos;
    lastPeriodNanos = 0.0;
    filteredTime = static_cast<double>(stamp.nanos);
    nanosPerSample = kNanosPerSecond / sampleRate;
}

double BlockClock::stallNanos(double periodNanos)
{
    return std::max(4.0 * periodNanos, kGapSeconds * kNanosPerSecond);
}

bool BlockClock::stalled(std::uint64_t nanos) const
{
    return started && static_cast<double>(nanos) - static_cast<double>(lastArrival) >
                          stallNanos(lastPeriodNanos);
}

bool BlockClock::observe(const BlockStampValue& stamp, double sampleRate)
{
    if (!(sampleRate > 0.0) || !std::isfinite(sampleRate) || !stamp.paced) {
        reset();
        return false;
    }
    if (!started || sampleRate != rate || stamp.index < lastIndex) {
        start(stamp, sampleRate);
        return true;
    }
    if (stamp.index == lastIndex) {
        return false; // Seen already, or a block with no samples.
    }
    const auto samples = static_cast<double>(stamp.index - lastIndex);
    const double period = samples * nanosPerSample;
    if (static_cast<double>(stamp.nanos) - static_cast<double>(lastArrival) > stallNanos(period)) {
        start(stamp, sampleRate); // The host stopped calling process for a while.
        return true;
    }
    const double predicted = filteredTime + period;
    const double error = static_cast<double>(stamp.nanos) - predicted;
    if (std::abs(error) > std::max(kJumpPeriods * period, kJumpSeconds * kNanosPerSecond)) {
        start(stamp, sampleRate);
        return true;
    }
    // How far the line ran past the previous block's audio by the time this one came.
    lead = (static_cast<double>(stamp.nanos) - filteredTime) / nanosPerSample;
    // The loop, with the update's own period: ω = 2π·B·T, b = √2·ω, c = ω² (critically damped).
    const double omega = 2.0 * std::numbers::pi * kBandwidthHertz * samples / rate;
    filteredTime = predicted + std::numbers::sqrt2 * omega * error;
    nanosPerSample += omega * omega * error / samples;
    lastIndex = stamp.index;
    lastArrival = stamp.nanos;
    lastPeriodNanos = period;
    ++updates;
    return true;
}

double BlockClock::estimate(std::uint64_t nanos) const
{
    return static_cast<double>(lastIndex) +
           (static_cast<double>(nanos) - filteredTime) / nanosPerSample;
}

void PacedEdge::observe(const std::optional<BlockStampValue>& stamp, double sampleRate)
{
    if (!stamp) {
        return;
    }
    lastPaced = stamp->paced;
    rate = sampleRate;
    const bool updated = clock.observe(*stamp, sampleRate);
    if (clock.restarts() != knownRestarts) {
        // A delay measured before no longer applies: measure it again while the clock locks.
        knownRestarts = clock.restarts();
        delaySamples = 0.0;
        target = 0.0;
        lockingPeak = 0.0;
        hadLock = false;
    }
    if (!updated) {
        return;
    }
    const double lead = clock.lastLead() + kMarginSeconds * sampleRate;
    if (!clock.locked()) {
        lockingPeak = std::max(lockingPeak, lead);
    } else {
        target = std::max(target, lead);
    }
}

std::uint64_t PacedEdge::edge(std::uint64_t nanos, std::uint64_t newest, double capSeconds)
{
    const double cap = capSeconds * rate;
    // Blocks further apart than the largest delay cannot be paced: the edge would wait at the
    // newest sample between them anyway, and a freeze's margin covers no more.
    if (!lastPaced || !clock.locked() || clock.stalled(nanos) ||
        clock.periodSeconds() > capSeconds) {
        pacedLast = false;
        lastEdge = newest;
        lastNanos = nanos;
        return newest;
    }
    if (!hadLock) {
        // Locked: start from the largest lead seen while locking, so the delay seldom rises.
        hadLock = true;
        delaySamples = std::min(lockingPeak, cap);
        target = delaySamples;
    } else {
        const double seconds =
            lastNanos && nanos > *lastNanos
                ? std::min(static_cast<double>(nanos - *lastNanos) / kNanosPerSecond,
                           kLongestStepSeconds)
                : 0.0;
        const double wanted = std::max(target, std::exchange(shortfallTarget, 0.0));
        if (wanted > delaySamples) {
            delaySamples = std::min(wanted, delaySamples + kRiseRate * seconds * rate);
        } else {
            delaySamples -= delaySamples * kDecayRate * seconds;
            target = delaySamples;
        }
    }
    delaySamples = std::min(delaySamples, cap);
    lastNanos = nanos;
    const double wanted = std::max(0.0, std::floor(clock.estimate(nanos) - delaySamples));
    const std::uint64_t floor = std::min(lastEdge, newest);
    const std::uint64_t edge = std::clamp(static_cast<std::uint64_t>(wanted), floor, newest);
    lastEdge = edge;
    pacedLast = true;
    return edge;
}

} // namespace orizon::engine
