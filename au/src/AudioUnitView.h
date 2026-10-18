#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <optional>
#include <string>
#include <string_view>

// The Audio Unit's view: hosts load the component bundle, ask it for the view factory class by
// name and ask the factory for a view each time they open the plugin's window. The view holds the
// editor (editor::EditorCore), as the VST3 view does.

namespace orizon::au {

// The view factory class's name (it conforms to AUCocoaUIBase). Unique to this build, so that
// two copies or versions of Sineless in one host never share the class.
[[nodiscard]] const char* viewFactoryClassName();

// The bundle that holds the binary at `imagePath` (".../Sineless.component" for
// ".../Sineless.component/Contents/MacOS/Sineless"), or none if the binary is not in one.
[[nodiscard]] std::optional<std::string> bundlePathFor(std::string_view imagePath);

// For kAudioUnitProperty_CocoaUI: this binary's bundle and the factory class's name, each
// retained for the host to release. kAudioUnitErr_InvalidProperty when the binary is not in a
// bundle, where no host could load the view.
OSStatus copyCocoaViewInfo(AudioUnitCocoaViewInfo& info);
// The same for the binary at `imagePath`, which copyCocoaViewInfo finds from the factory class.
OSStatus copyCocoaViewInfo(std::string_view imagePath, AudioUnitCocoaViewInfo& info);

} // namespace orizon::au
