#pragma once

#include "Branding.h"
#include "pluginterfaces/base/funknown.h"

namespace orizon::plugin {

// Fixed forever once released: hosts store these in saved projects. A build without the brand
// (SINELESS_BRANDING=OFF) takes placeholders, so a fork never shares Sineless's IDs; a fork that
// releases replaces them with its own.
#if SINELESS_BRANDED
inline DECLARE_UID(kProcessorUid, 0xB704D89D, 0x96B24108, 0x878CA953,
                   0x316956A4) inline DECLARE_UID(kControllerUid, 0x46B0AF97, 0x85904BB3,
                                                  0x99DC138A, 0x997DE554)
#else
inline DECLARE_UID(kProcessorUid, 0x5C0BE000, 0x00000000, 0x00000000,
                   0x00000001) inline DECLARE_UID(kControllerUid, 0x5C0BE000, 0x00000000,
                                                  0x00000000, 0x00000002)
#endif

    constexpr const char* kVendorName = branding::kVendorName;
constexpr const char* kVendorUrl = "";
constexpr const char* kVendorEmail = "";
constexpr const char* kProductName = branding::kProductName;
constexpr const char* kSubCategories = "Fx|Analyzer";

// Message the processor sends to the controller on connect, with its engine's token
// (engine::EngineRegistry) as attribute.
constexpr const char* kEngineTokenMessage = "OrizonEngineToken";
constexpr const char* kEngineTokenAttribute = "token";

} // namespace orizon::plugin
