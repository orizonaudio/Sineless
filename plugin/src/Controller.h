#pragma once

#include "engine/Engine.h"
#include "pluginterfaces/vst/ivstchannelcontextinfo.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <memory>
#include <optional>

namespace orizon::plugin {

// The track details a host reports, as the multi-track view labels them.
[[nodiscard]] engine::TrackInfo trackInfoFrom(Steinberg::Vst::IAttributeList& list);

// Edit-controller side of the plugin. It declares no parameters and reaches the
// processor's engine through the token the processor sends on connect. It also passes on what
// the host says about the track: name, colour and position.
class Controller : public Steinberg::Vst::EditController,
                   public Steinberg::Vst::ChannelContext::IInfoListener
{
public:
    static Steinberg::FUnknown* createInstance(void* context);

    Steinberg::tresult PLUGIN_API
    setChannelContextInfos(Steinberg::Vst::IAttributeList* list) override;

    Steinberg::tresult PLUGIN_API terminate() override;
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override;

    // Null until the processor's token arrives, or if it names no engine in this process.
    [[nodiscard]] const std::shared_ptr<engine::Engine>& engine() const { return engineState; }

    OBJ_METHODS(Controller, EditController)
    DEFINE_INTERFACES
    DEF_INTERFACE(Steinberg::Vst::ChannelContext::IInfoListener)
    END_DEFINE_INTERFACES(EditController)
    REFCOUNT_METHODS(EditController)

private:
    std::shared_ptr<engine::Engine> engineState;
    // Track details that arrived before the engine did.
    std::optional<engine::TrackInfo> track;
};

} // namespace orizon::plugin
