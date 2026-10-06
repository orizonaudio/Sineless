#include "Controller.h"

#include "editor/Editor.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <os/log.h>
#include <string>

namespace orizon::plugin {

using namespace Steinberg;

FUnknown* Controller::createInstance(void* /*context*/)
{
    return static_cast<Vst::IEditController*>(new Controller());
}

namespace {

// T-005 spike: answers "ping" with "pong" and a 4096-byte test pattern (byte i == i mod 256).
// T-006 spike: logs requestAnimationFrame rate samples.
void handleSpikeMessage(editor::WebView& view, std::string_view message)
{
    if (message.starts_with("raf:")) {
        // T-006: frame-rate samples, read back with `log show --predicate 'eventMessage
        // CONTAINS "OrizonRaf"'`.
        const std::string sample(message);
        os_log(OS_LOG_DEFAULT, "OrizonRaf %{public}s", sample.c_str());
        return;
    }
    if (message != "ping") {
        return;
    }
    view.postMessage("pong");
    std::array<std::byte, 4096> pattern{};
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<std::byte>(i % 256);
    }
    view.postBinary(pattern);
}

} // namespace

IPlugView* PLUGIN_API Controller::createView(FIDString name)
{
    if (name == nullptr || std::strcmp(name, Vst::ViewType::kEditor) != 0) {
        return nullptr;
    }
    return new editor::Editor(&handleSpikeMessage);
}

tresult PLUGIN_API Controller::setComponentState(IBStream* /*state*/)
{
    // All settings live in the processor's engine (DESIGN 5.1); the controller keeps no copy.
    return kResultOk;
}

} // namespace orizon::plugin
