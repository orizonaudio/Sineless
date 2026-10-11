#include "Controller.h"

#include "EngineRegistry.h"
#include "editor/Editor.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <array>
#include <cstring>

namespace orizon::plugin {

using namespace Steinberg;

engine::TrackInfo mergeTrackInfo(engine::TrackInfo known, Vst::IAttributeList& list)
{
    namespace context = Vst::ChannelContext;
    engine::TrackInfo info = std::move(known);
    std::array<Vst::TChar, 256> name{};
    if (list.getString(context::kChannelNameKey, name.data(),
                       static_cast<uint32>(name.size() * sizeof(Vst::TChar))) == kResultTrue &&
        name.front() != 0) {
        name.back() = 0;
        info.name = Vst::StringConvert::convert(name.data());
    }
    int64 colour = 0;
    if (list.getInt(context::kChannelColorKey, colour) == kResultTrue) {
        info.colour = static_cast<std::uint32_t>(colour) & 0xffffffU; // Drop the alpha.
    }
    // Positions count from 1 within a namespace (such as inputs, channels, outputs); the
    // namespace's own order ranks it among them. Like the name, either part may come in a report
    // of its own, so the other part is kept from what was known.
    constexpr int64 kNamespaceStep = 100000;
    int64 position = info.index ? *info.index % kNamespaceStep : 0;
    int64 namespaceOrder = info.index ? *info.index / kNamespaceStep : 0;
    const bool hasPosition = list.getInt(context::kChannelIndexKey, position) == kResultTrue;
    const bool hasOrder =
        list.getInt(context::kChannelIndexNamespaceOrderKey, namespaceOrder) == kResultTrue;
    if (hasPosition || (hasOrder && info.index)) {
        info.index = static_cast<int>(namespaceOrder * kNamespaceStep + position);
    }
    return info;
}

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

tresult PLUGIN_API Controller::setChannelContextInfos(Vst::IAttributeList* list)
{
    if (list == nullptr) {
        return kInvalidArgument;
    }
    track = mergeTrackInfo(track.value_or(engine::TrackInfo{}), *list);
    if (engineState) {
        engineState->setTrackInfo(*track);
    }
    return kResultTrue;
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
        if (engineState && track) {
            engineState->setTrackInfo(*track);
        }
    }
    return kResultOk;
}

tresult PLUGIN_API Controller::setComponentState(IBStream* /*state*/)
{
    // All settings live in the processor's engine; the controller keeps no copy.
    return kResultOk;
}

} // namespace orizon::plugin
