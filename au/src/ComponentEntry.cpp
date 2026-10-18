#include "AudioUnit.h"

#include <AudioUnitSDK/AUBase.h>

// The component's entry point: hosts load Sineless.component, look this function up by the name
// its Info.plist gives (kFactoryFunctionName) and call it for each instance. Written out rather
// than through the SDK's AUSDK_COMPONENT_ENTRY, which pastes the class name into the function's
// and so cannot take a namespaced class; named as AU factories are.
// NOLINTBEGIN(readability-identifier-naming)
extern "C" __attribute__((visibility("default"))) void*
SinelessFactory(const AudioComponentDescription* description)
{
    return ausdk::AUBaseFactory<orizon::au::AudioUnit>::Factory(description);
}
// NOLINTEND(readability-identifier-naming)
