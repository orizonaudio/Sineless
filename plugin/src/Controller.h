#pragma once

#include "engine/Engine.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <memory>

namespace orizon::plugin {

// Edit-controller side of the plugin. It declares no parameters (AD-6) and reaches the
// processor's engine through the token the processor sends on connect (AD-10).
class Controller : public Steinberg::Vst::EditController
{
public:
    static Steinberg::FUnknown* createInstance(void* context);

    Steinberg::tresult PLUGIN_API terminate() override;
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override;

    // Null until the processor's token arrives, or if it names no engine in this process.
    [[nodiscard]] const std::shared_ptr<engine::Engine>& engine() const { return engineState; }

private:
    std::shared_ptr<engine::Engine> engineState;
};

} // namespace orizon::plugin
