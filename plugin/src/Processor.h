#pragma once

#include "engine/CaptureRing.h"
#include "engine/RealtimeSafety.h"
#include "public.sdk/source/vst/vstaudioeffect.h"

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
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs,
                                                     Steinberg::int32 numIns,
                                                     Steinberg::Vst::SpeakerArrangement* outputs,
                                                     Steinberg::int32 numOuts) override;
    Steinberg::tresult PLUGIN_API
    canProcessSampleSize(Steinberg::int32 symbolicSampleSize) override;
    Steinberg::tresult PLUGIN_API
    process(Steinberg::Vst::ProcessData& data) noexcept ORIZON_NONBLOCKING override;

    // Null until initialize(); then lives as long as the processor.
    [[nodiscard]] const engine::CaptureRing* captureRing() const { return ring.get(); }

private:
    std::unique_ptr<engine::CaptureRing> ring;
};

} // namespace orizon::plugin
