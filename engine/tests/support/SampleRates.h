#pragma once

#include "engine/CaptureRing.h"

#include <array>

namespace orizon::engine {

// The sample rates hosts run at, from CD audio to the highest any of them offers. The protocol
// does not list them: nothing the engine and the UI share depends on one.
inline constexpr std::array kSampleRates{44100.0,  48000.0,  88200.0,  96000.0,
                                         176400.0, 192000.0, 352800.0, 384000.0};
static_assert(kSampleRates.back() == CaptureRing::kHighestSampleRate,
              "the capture is sized for the highest rate tested");

} // namespace orizon::engine
