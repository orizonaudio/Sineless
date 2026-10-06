#include "engine/Trigger.h"

#include <algorithm>
#include <cmath>

namespace orizon::engine {

using protocol::TriggerMode;

std::pair<std::uint64_t, std::uint64_t> TriggerState::searchRegion(std::uint64_t begin,
                                                                   std::uint64_t end,
                                                                   double spanSamples,
                                                                   double timeoutSamples)
{
    // A visible span plus the timeout, so that "no edge in the region" means "no edge within the
    // timeout", plus one sample to detect an edge at the region start.
    const auto length =
        static_cast<std::uint64_t>(std::ceil(spanSamples + std::max(spanSamples, timeoutSamples))) +
        1;
    const std::uint64_t first = end - begin > length ? end - length : begin;
    return {first, end};
}

TriggerDecision TriggerState::decide(std::span<const float> samples, std::uint64_t firstIndex,
                                     double spanSamples, double timeoutSamples,
                                     const TriggerParameters& parameters, TriggerMode mode)
{
    const auto end = static_cast<double>(firstIndex + samples.size());
    const double untriggeredStart = end - spanSamples;
    if (mode == TriggerMode::kFree) {
        return {.action = TriggerDecision::Action::kDraw, .windowStart = untriggeredStart};
    }

    // An edge can be shown once the span before and after it is in the region.
    const double preTrigger = kTriggerPosition * spanSamples;
    const double latestUsable = end - (spanSamples - preTrigger);
    const double earliestUsable = static_cast<double>(firstIndex) + preTrigger;

    // Follow the hold-off chain forward from the last trigger; the newest accepted edge wins.
    if (last && *last < static_cast<double>(firstIndex)) {
        last.reset(); // Too old to continue the chain from.
    }
    std::optional<double> accepted = last;
    forEachEdge(samples, firstIndex, parameters, [&](double edge) {
        if (edge < earliestUsable || edge > latestUsable) {
            return;
        }
        if (!accepted || edge >= *accepted + std::max(parameters.holdOffSamples, 1e-9)) {
            accepted = edge;
        }
    });

    const bool newTrigger = accepted && (!last || *accepted > *last);
    if (newTrigger) {
        last = accepted;
        if (mode == TriggerMode::kSingle) {
            if (!singleArmed) {
                return {};
            }
            singleArmed = false;
            return {.action = TriggerDecision::Action::kDraw,
                    .windowStart = *accepted - preTrigger,
                    .triggered = true,
                    .freeze = true};
        }
        return {.action = TriggerDecision::Action::kDraw,
                .windowStart = *accepted - preTrigger,
                .triggered = true};
    }

    if (mode == TriggerMode::kAuto) {
        const double timeout = std::max(spanSamples, timeoutSamples);
        const double sinceLast =
            last ? latestUsable - *last : end - static_cast<double>(firstIndex);
        if (sinceLast >= timeout) {
            return {.action = TriggerDecision::Action::kDraw, .windowStart = untriggeredStart};
        }
    }
    return {}; // Normal, Single, or Auto within the timeout: keep the last trace.
}

} // namespace orizon::engine
