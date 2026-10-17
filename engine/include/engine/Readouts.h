#pragma once

#include "protocol/Protocol.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <span>

namespace orizon::engine {

// Indicative readouts of the channel the display measures (the trigger source in Single mode in
// Seconds, this track's Mid otherwise): sample peak and RMS over the whole span, whatever part of
// it is on screen, and the frequency over its newest second. Not standards-compliant metering.
struct Readouts
{
    // Largest absolute sample value, and the RMS; NaN with nothing to measure.
    float peak = std::numeric_limits<float>::quiet_NaN();
    float rms = std::numeric_limits<float>::quiet_NaN();
    // From the spacing of rising crossings of the mean, with hysteresis at 10% of the
    // peak-to-peak range. Empty with fewer than two crossings.
    std::optional<double> frequencyHz;
    // Samples measured around: not finite (NaN or infinite), left out of all of the above.
    std::uint32_t invalid = 0;
};

// The frequency is estimated over the newest second of the span, so that it follows what plays
// now and costs the same however long the span.
inline constexpr double kFrequencyWindowSeconds = protocol::kFrequencyWindowSeconds;

// Peak and RMS of all of `samples`; the frequency of their newest kFrequencyWindowSeconds.
[[nodiscard]] Readouts computeReadouts(std::span<const float> samples, double sampleRate);

// The frequency of `samples` from the spacing of rising crossings of their mean, with hysteresis
// at kCrossingHysteresis of the peak-to-peak range. Empty for a flat signal, with fewer than two
// crossings, or when the signal does not repeat steadily: fewer than 60% of its periods within
// kPeriodTolerance of the median period (noise, or several pitches at once).
[[nodiscard]] std::optional<double> estimateFrequency(std::span<const float> samples,
                                                      double sampleRate);

// How many of a span of `count` samples the frequency is estimated over: the newest
// kFrequencyWindowSeconds, or all of them if there are fewer.
[[nodiscard]] std::size_t frequencyWindow(std::size_t count, double sampleRate);

} // namespace orizon::engine
