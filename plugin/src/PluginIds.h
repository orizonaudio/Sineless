#pragma once

#include "pluginterfaces/base/funknown.h"

namespace orizon::plugin {

// Fixed forever once released: hosts store these in saved projects.
inline DECLARE_UID(kProcessorUid, 0xB704D89D, 0x96B24108, 0x878CA953,
                   0x316956A4) inline DECLARE_UID(kControllerUid, 0x46B0AF97, 0x85904BB3,
                                                  0x99DC138A, 0x997DE554)

    constexpr const char* kVendorName = "Orizon Audio";
constexpr const char* kVendorUrl = "";
constexpr const char* kVendorEmail = "";
constexpr const char* kProductName = "Sineless";
constexpr const char* kSubCategories = "Fx|Analyzer";

} // namespace orizon::plugin
