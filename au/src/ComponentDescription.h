#pragma once

#include "Branding.h"

#include <AudioToolbox/AudioToolbox.h>

namespace orizon::au {

// The component the AU registers as: an effect, Sineless by Orizon Audio. The codes are saved in
// every project that uses the AU, so they never change. A build without the brand
// (SINELESS_BRANDING=OFF) takes placeholders; both come from cmake/Branding.cmake, as Info.plist's.
inline constexpr AudioComponentDescription kComponentDescription{
    .componentType = kAudioUnitType_Effect,
    .componentSubType = SINELESS_AU_SUBTYPE,
    .componentManufacturer = SINELESS_AU_MANUFACTURER,
    .componentFlags = 0,
    .componentFlagsMask = 0,
};

// The function hosts call to make an instance, exported by the component's binary under this name
// (its Info.plist names it too).
inline constexpr const char* kFactoryFunctionName = "SinelessFactory";

} // namespace orizon::au
