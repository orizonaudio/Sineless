#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"

namespace orizon::plugin {

// Edit-controller side of the plugin. It declares no parameters (AD-6).
class Controller : public Steinberg::Vst::EditController
{
public:
    static Steinberg::FUnknown* createInstance(void* context);

    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;
};

} // namespace orizon::plugin
