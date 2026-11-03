#include "engine/FramePacing.h"

#include "protocol/Protocol.h"

#include <algorithm>
#include <cmath>

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
    if (lead && !lost) {
        // D on the engine's clock: the target less L, as far from now as it is on the link's.
        const double fromNow = target - *lead - now;
        std::uint64_t placed = engineNow;
        if (fromNow > 0.0) {
            ++early; // A callback earlier than L allowed: placed now, not in the future.
        } else {
            const auto back = static_cast<std::uint64_t>(std::llround(-fromNow * 1e9));
            placed = back < engineNow ? engineNow - back : 0;
        }
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
    return {.requestFrame = frameDue};
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
            value = displayNanos.load(std::memory_order_relaxed);
        }
    }
    lastReturned = std::max(lastReturned, value);
    return lastReturned;
}

} // namespace orizon::engine
