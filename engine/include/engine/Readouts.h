#pragma once

#include <optional>
#include <span>

namespace orizon::engine {

// Indicative readouts of the trigger channel over the visible span. Not standards-
// compliant metering: sample peak only, and frequency from level crossings.
struct Readouts
{
    float peak = 0.0f; // Largest absolute sample value.
    float rms = 0.0f;
    // From the spacing of rising crossings of the mean, with hysteresis at 10% of the
    // peak-to-peak range. Empty with fewer than two crossings in the span.
    std::optional<double> frequencyHz;
};

[[nodiscard]] Readouts computeReadouts(std::span<const float> samples, double sampleRate);

} // namespace orizon::engine
