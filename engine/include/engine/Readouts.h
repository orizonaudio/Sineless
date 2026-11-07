#pragma once

#include <cstdint>
#include <limits>
#include <span>

namespace orizon::engine {

// Indicative readouts of the channel the display measures (in Single the readout source when drawn,
// else the first view drawn; this track's Mid in Multi): sample peak and RMS over the whole span,
// whatever part of it is on screen. Not standards-compliant metering.
struct Readouts
{
    // Largest absolute sample value, and the RMS; NaN with nothing to measure.
    float peak = std::numeric_limits<float>::quiet_NaN();
    float rms = std::numeric_limits<float>::quiet_NaN();
    // Samples measured around: not finite (NaN or infinite), left out of both.
    std::uint32_t invalid = 0;
};

// Peak and RMS of all of `samples`.
[[nodiscard]] Readouts computeReadouts(std::span<const float> samples);

} // namespace orizon::engine
