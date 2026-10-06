#pragma once

#include "engine/Engine.h"
#include "engine/RealtimeSafety.h"
#include "public.sdk/source/vst/vstaudioeffect.h"

#include <cstdint>
#include <memory>

namespace orizon::plugin {

// Audio side of the plugin: passes the main input to the output unchanged (FR-1, QG-2) and
// records it into the capture ring on every block (DESIGN 5.2).
class Processor : public Steinberg::Vst::AudioEffect
{
public:
    Processor();

    static Steinberg::FUnknown* createInstance(void* context);

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override;
    // Saved state (DESIGN 8.4). Called on a non-audio host thread; never throws.
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    // Sends the controller the token of this instance's engine (AD-10).
    Steinberg::tresult PLUGIN_API connect(Steinberg::Vst::IConnectionPoint* other) override;
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs,
                                                     Steinberg::int32 numIns,
                                                     Steinberg::Vst::SpeakerArrangement* outputs,
                                                     Steinberg::int32 numOuts) override;
    Steinberg::tresult PLUGIN_API
    canProcessSampleSize(Steinberg::int32 symbolicSampleSize) override;
    Steinberg::tresult PLUGIN_API
    process(Steinberg::Vst::ProcessData& data) noexcept ORIZON_NONBLOCKING override;

    // Null outside initialize()/terminate().
    [[nodiscard]] const std::shared_ptr<engine::Engine>& engine() const { return engineState; }

private:
    std::shared_ptr<engine::Engine> engineState;
    std::int64_t engineToken = 0;
};

} // namespace orizon::plugin
