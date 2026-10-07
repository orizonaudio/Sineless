#pragma once

#include "protocol/Protocol.h"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace orizon::engine {

// Edge trigger and trigger modes. Positions are absolute, fractional sample indices.

// The trigger point sits this fraction of the span from the left edge.
inline constexpr double kTriggerPosition = 0.1;
// Auto mode draws untriggered once no trigger arrived for max(span, this).
inline constexpr double kAutoTimeoutSeconds = 0.1;

struct TriggerParameters
{
    double level = 0.0;
    double hysteresis = 0.0;
    protocol::TriggerSlope slope = protocol::TriggerSlope::kRising;
    double holdOffSamples = 0.0;
};

// Positions of the edges in `samples` (samples[0] has index `firstIndex`), oldest first.
// A rising edge needs the signal to fall to level - hysteresis first, then cross level going up;
// falling edges mirror this. The crossing point is interpolated linearly between samples.
template <typename Callback>
void forEachEdge(std::span<const float> samples, std::uint64_t firstIndex,
                 const TriggerParameters& parameters, Callback&& onEdge)
{
    const double sign = parameters.slope == protocol::TriggerSlope::kRising ? 1.0 : -1.0;
    const double level = sign * parameters.level;
    const double armLevel = level - parameters.hysteresis;
    bool armed = false;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double current = sign * samples[i];
        if (armed && i > 0 && current >= level) {
            const double previous = sign * samples[i - 1];
            if (previous < level) {
                const double fraction = (level - previous) / (current - previous);
                onEdge(static_cast<double>(firstIndex + i - 1) + fraction);
                armed = false;
            }
        }
        if (current <= armLevel) {
            armed = true;
        }
    }
}

// What a frame shows: a window of the signal, or nothing new (keep the last trace).
struct TriggerDecision
{
    enum class Action : std::uint8_t
    {
        kDraw,
        kHold,
    };
    Action action = Action::kHold;
    double windowStart = 0.0; // First sample of the visible span, when drawing.
    bool triggered = false;
    bool freeze = false; // Single mode captured its frame: freeze now.
};

// Per-session trigger state across frames.
class TriggerState
{
public:
    // The samples [first, end) the next decide() needs, given the readable range
    // [begin, end) and the span and timeout in samples.
    [[nodiscard]] static std::pair<std::uint64_t, std::uint64_t>
    searchRegion(std::uint64_t begin, std::uint64_t end, double spanSamples, double timeoutSamples);

    // `samples` is the trigger source over the search region; its last sample is the newest.
    TriggerDecision decide(std::span<const float> samples, std::uint64_t firstIndex,
                           double spanSamples, double timeoutSamples,
                           const TriggerParameters& parameters, protocol::TriggerMode mode);

    // Single mode: wait for the next trigger again.
    void arm() { singleArmed = true; }

    [[nodiscard]] std::optional<double> lastTrigger() const { return last; }

private:
    std::optional<double> last;
    bool singleArmed = true;
};

} // namespace orizon::engine
