#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"

namespace orizon::plugin {

// Audio side of the plugin: passes the main input to the output unchanged (FR-1, QG-2).
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
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;
};

} // namespace orizon::plugin
