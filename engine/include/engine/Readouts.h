#pragma once

#include <cstddef>
#include <optional>
#include <span>

namespace orizon::engine {

// Indicative readouts of the trigger channel: sample peak and RMS over the visible span, and the
// frequency over its newest second. Not standards-compliant metering.
struct Readouts
{
    float peak = 0.0f; // Largest absolute sample value.
    float rms = 0.0f;
    // From the spacing of rising crossings of the mean, with hysteresis at 10% of the
    // peak-to-peak range. Empty with fewer than two crossings.
    std::optional<double> frequencyHz;
};

// The frequency is estimated over the newest second of the span, so that it follows what plays
// now and costs the same however long the span.
inline constexpr double kFrequencyWindowSeconds = 1.0;

// Peak and RMS of all of `samples`; the frequency of their newest kFrequencyWindowSeconds.
[[nodiscard]] Readouts computeReadouts(std::span<const float> samples, double sampleRate);

// The frequency of `samples` from the spacing of rising crossings of their mean, with hysteresis
// at 10% of the peak-to-peak range. Empty with fewer than two crossings, or for a flat signal.
[[nodiscard]] std::optional<double> estimateFrequency(std::span<const float> samples,
                                                      double sampleRate);

// How many of a span of `count` samples the frequency is estimated over: the newest
// kFrequencyWindowSeconds, or all of them if there are fewer.
[[nodiscard]] std::size_t frequencyWindow(std::size_t count, double sampleRate);

} // namespace orizon::engine
