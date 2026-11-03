#pragma once

#include "engine/Version.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace orizon::editor {

// Objective-C class names are global to the process: two plugin binaries (two versions, or the
// VST3 and the Audio Unit) loaded together must not register the same name with different code.
// The name carries the version and an address inside this binary (8.7, "Several plugins in one
// process").
inline std::string uniqueClassName(const char* baseName)
{
    static const int kImageMarker = 0;
    std::string version = engine::kVersionString;
    std::ranges::replace(version, '.', '_');
    std::array<char, 32> imageId{};
    std::snprintf(imageId.data(), imageId.size(), "%llx",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&kImageMarker)));
    return std::string("OrizonOscilloscope_") + version + "_" + baseName + "_" + imageId.data();
}

} // namespace orizon::editor
