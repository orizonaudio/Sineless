#include "Controller.h"

#include "EngineRegistry.h"
#include "editor/Editor.h"

#include <cstring>

namespace orizon::plugin {

using namespace Steinberg;

FUnknown* Controller::createInstance(void* /*context*/)
{
    return static_cast<Vst::IEditController*>(new Controller());
}

IPlugView* PLUGIN_API Controller::createView(FIDString name)
{
    if (name == nullptr || std::strcmp(name, Vst::ViewType::kEditor) != 0) {
        return nullptr;
    }
    return new editor::Editor(engineState);
}

tresult PLUGIN_API Controller::terminate()
{
    engineState.reset();
    return EditController::terminate();
}

tresult PLUGIN_API Controller::notify(Vst::IMessage* message)
{
    if (message == nullptr || std::strcmp(message->getMessageID(), kEngineTokenMessage) != 0) {
        return EditController::notify(message);
    }
    int64 token = 0;
    if (message->getAttributes()->getInt(kEngineTokenAttribute, token) == kResultOk) {
        engineState = EngineRegistry::find(token);
    }
    return kResultOk;
}

tresult PLUGIN_API Controller::setComponentState(IBStream* /*state*/)
{
    // All settings live in the processor's engine; the controller keeps no copy.
    return kResultOk;
}

} // namespace orizon::plugin
