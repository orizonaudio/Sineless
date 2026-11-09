#include "engine/FramePacing.h"

#include "protocol/Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace orizon::engine {

int ticksPerFrame(double rateHertz, std::optional<int> previous)
{
    const auto atMost = [rateHertz](double line) {
        return std::max(1, static_cast<int>(std::ceil(rateHertz / line)));
    };
    const int plain = atMost(protocol::kFullRateUpToHz);
    if (!previous) {
        return plain;
    }
    const int up = atMost(protocol::kFullRateUpToHz * (1.0 + protocol::kLineHysteresis));
    const int down = atMost(protocol::kFullRateUpToHz * (1.0 - protocol::kLineHysteresis));
    if (up > *previous) {
        return up;
    }
    if (down < *previous) {
        return down;
    }
    return *previous;
}

namespace {

template <std::size_t Size> double median(const std::array<double, Size>& values, std::size_t count)
{
    std::array<double, Size> sorted = values;
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count));
    return sorted[count / 2];
}

} // namespace

FramePacing::FramePacing(double screenRateHertz) : rate(screenRateHertz)
{
    n = ticksPerFrame(rate);
}

void FramePacing::restartLead()
{
    maxLead = 0.0;
    leadCount = 0;
    lead.reset();
    display.store(false, std::memory_order_release);
}

void FramePacing::setScreenRate(double screenRateHertz)
{
    rate = screenRateHertz;
    stepCount = 0;
    nextStep = 0;
    callbackStepCount = 0;
    nextCallbackStep = 0;
    lastTarget.reset();
    n = ticksPerFrame(rate, n);
    displayPeriod.store(0.0, std::memory_order_relaxed);
    restartLead();
}

void FramePacing::pause()
{
    display.store(false, std::memory_order_release);
    lastTarget.reset();
}

void FramePacing::lose()
{
    lost = true;
    display.store(false, std::memory_order_release);
}

FramePacing::Decision FramePacing::callback(double target, double now, std::uint64_t engineNow)
{
    const double period = 1.0 / rate;
    // Refresh periods since the last callback's target: one, or more after a skipped callback or
    // with a link the system slows.
    int periods = 1;
    if (lastTarget) {
        const double step = target - *lastTarget;
        if (step > 0.0) {
            periods = std::max(1, static_cast<int>(std::lround(step / period)));
            steps[nextStep] = step / periods;
            nextStep = (nextStep + 1) % kRateSteps;
            stepCount = std::min(stepCount + 1, kRateSteps);
            callbackSteps[nextCallbackStep] = step;
            nextCallbackStep = (nextCallbackStep + 1) % kRateSteps;
            callbackStepCount = std::min(callbackStepCount + 1, kRateSteps);
            if (stepCount == kRateSteps) {
                rate = 1.0 / median(steps, stepCount);
                n = ticksPerFrame(rate, n);
                displayPeriod.store(1.0 / rate, std::memory_order_relaxed);
            }
            callbackPeriodNanos.store(static_cast<std::uint64_t>(std::llround(
                                          median(callbackSteps, callbackStepCount) * 1e9)),
                                      std::memory_order_relaxed);
        }
    }
    lastTarget = target;

    if (!lead) {
        maxLead = std::max(maxLead, target - now);
        if (++leadCount == kLeadCallbacks) {
            lead = maxLead;
        }
    }
    followMaking(callbackPeriodSeconds());
    double requestAfter = requestDelaySeconds();
    if (lead && !lost) {
        // D on the engine's clock: the target less L, as far from now as it is on the link's, then
        // moved on to the moment the frame is asked for (requestDelaySeconds after this callback).
        const double fromNow = target - *lead - now;
        double placedFromNow = fromNow;
        if (fromNow > 0.0) {
            ++early; // A callback earlier than L allowed: placed from now, not further on.
            placedFromNow = 0.0;
        }
        placedFromNow += requestDelaySeconds();
        requestAfter = std::max(0.0, placedFromNow);
        const auto offset = static_cast<std::int64_t>(std::llround(placedFromNow * 1e9));
        const std::uint64_t placed =
            offset >= 0 ? engineNow + static_cast<std::uint64_t>(offset)
                        : engineNow - std::min(engineNow, static_cast<std::uint64_t>(-offset));
        displayNanos.store(std::max(displayNanos.load(std::memory_order_relaxed), placed),
                           std::memory_order_relaxed);
        lastCallbackNanos.store(engineNow, std::memory_order_relaxed);
        display.store(true, std::memory_order_release);
    }

    // Which refreshes show a new frame: every n-th, counted in refresh periods. The request for
    // the next shown refresh is opened one callback ahead, so its frame is ready when it comes.
    periodsSinceShown = frameDue ? 0 : periodsSinceShown + periods;
    const auto callbackPeriods =
        callbackStepCount > 0
            ? std::max(1L, std::lround(median(callbackSteps, callbackStepCount) / period))
            : 1L;
    frameDue = periodsSinceShown + static_cast<int>(callbackPeriods) >= ticks();
    return {.requestFrame = frameDue, .requestAfterSeconds = requestAfter};
}

double FramePacing::callbackPeriodSeconds() const
{
    const auto callbackNanos = callbackPeriodNanos.load(std::memory_order_relaxed);
    return callbackNanos > 0 ? static_cast<double>(callbackNanos) / 1e9
                             : static_cast<double>(ticks()) / rate;
}

double FramePacing::requestDelaySeconds() const
{
    return std::max(0.0, callbackPeriodSeconds() - margin);
}

void FramePacing::frameMade(std::uint64_t nanos)
{
    // The slowest since the callback last took one: a frame that missed its refresh is reported
    // just before the next frame, and must not be lost behind it. Stored one more, so 0 is none.
    const std::uint64_t stored = nanos + 1;
    std::uint64_t slowest = slowestMade.load(std::memory_order_relaxed);
    while (slowest < stored &&
           !slowestMade.compare_exchange_weak(slowest, stored, std::memory_order_relaxed)) {
    }
}

void FramePacing::followMaking(double callbackPeriod)
{
    const std::uint64_t made = slowestMade.exchange(0, std::memory_order_relaxed);
    if (made != 0) {
        makeTimes[nextMake] = static_cast<double>(made - 1) / 1e9;
        nextMake = (nextMake + 1) % kMakeHistory;
        makeCount = std::min(makeCount + 1, kMakeHistory);
    }
    if (makeCount == 0) {
        return;
    }
    const double slowest = *std::max_element(
        makeTimes.begin(), makeTimes.begin() + static_cast<std::ptrdiff_t>(makeCount));
    // Beyond a callback period the frame is asked for at the callback anyway: more would only
    // place D earlier.
    const double wanted =
        std::min(callbackPeriod, std::max(kHandOffMarginSeconds, slowest + kHandOffSlackSeconds));
    margin = std::clamp(wanted, margin - kMarginSlewSeconds, margin + kMarginSlewSeconds);
}

std::uint64_t FramePacing::clock(std::uint64_t engineNow)
{
    reads.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t value = engineNow;
    if (display.load(std::memory_order_acquire)) {
        const std::uint64_t since =
            engineNow - std::min(engineNow, lastCallbackNanos.load(std::memory_order_relaxed));
        const auto stall =
            static_cast<double>(callbackPeriodNanos.load(std::memory_order_relaxed)) *
            kStallCallbackPeriods;
        if (stall <= 0.0 || static_cast<double>(since) < stall) {
            // Never in the future: a read before the frame's moment gets the time now.
            value = std::min(engineNow, displayNanos.load(std::memory_order_relaxed));
        }
    }
    lastReturned = std::max(lastReturned, value);
    return lastReturned;
}

} // namespace orizon::engine
