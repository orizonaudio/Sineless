#include "Controller.h"

namespace orizon::plugin {

using namespace Steinberg;

FUnknown* Controller::createInstance(void* /*context*/)
{
    return static_cast<Vst::IEditController*>(new Controller());
}

tresult PLUGIN_API Controller::setComponentState(IBStream* /*state*/)
{
    // All settings live in the processor's engine (DESIGN 5.1); the controller keeps no copy.
    return kResultOk;
}

} // namespace orizon::plugin
