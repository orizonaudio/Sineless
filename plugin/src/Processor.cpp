#include "Processor.h"

#include "PluginIds.h"

#include <algorithm>
#include <cstring>

namespace orizon::plugin {

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

bool isSupportedArrangement(SpeakerArrangement arrangement)
{
    return arrangement == SpeakerArr::kMono || arrangement == SpeakerArr::kStereo;
}

template <typename Sample>
void copyChannels(Sample** source, Sample** destination, int32 numChannels, int32 numSamples)
{
    for (int32 channel = 0; channel < numChannels; ++channel) {
        if (source[channel] != destination[channel]) {
            std::memcpy(destination[channel], source[channel],
                        static_cast<size_t>(numSamples) * sizeof(Sample));
        }
    }
}

} // namespace

Processor::Processor()
{
    setControllerClass(kControllerUid);
}

FUnknown* Processor::createInstance(void* /*context*/)
{
    return static_cast<IAudioProcessor*>(new Processor());
}

tresult PLUGIN_API Processor::initialize(FUnknown* context)
{
    const tresult result = AudioEffect::initialize(context);
    if (result != kResultOk) {
        return result;
    }
    addAudioInput(STR16("Input"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
    return kResultOk;
}

tresult PLUGIN_API Processor::setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                                 SpeakerArrangement* outputs, int32 numOuts)
{
    if (numIns != 1 || numOuts != 1 || inputs[0] != outputs[0] ||
        !isSupportedArrangement(inputs[0])) {
        return kResultFalse;
    }
    return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
}

tresult PLUGIN_API Processor::canProcessSampleSize(int32 symbolicSampleSize)
{
    return symbolicSampleSize == kSample32 || symbolicSampleSize == kSample64 ? kResultTrue
                                                                              : kResultFalse;
}

tresult PLUGIN_API Processor::process(ProcessData& data)
{
    if (data.numInputs == 0 || data.numOutputs == 0 || data.numSamples <= 0) {
        return kResultOk;
    }
    AudioBusBuffers& input = data.inputs[0];
    AudioBusBuffers& output = data.outputs[0];
    const int32 numChannels = std::min(input.numChannels, output.numChannels);

    if (data.symbolicSampleSize == kSample64) {
        copyChannels(input.channelBuffers64, output.channelBuffers64, numChannels, data.numSamples);
    } else {
        copyChannels(input.channelBuffers32, output.channelBuffers32, numChannels, data.numSamples);
    }
    output.silenceFlags = input.silenceFlags;
    return kResultOk;
}

} // namespace orizon::plugin
