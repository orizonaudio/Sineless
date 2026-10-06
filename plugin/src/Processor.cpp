#include "Processor.h"

#include "EngineRegistry.h"
#include "PluginIds.h"

#include <algorithm>
#include <array>
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
void copyChannels(Sample** source, Sample** destination, int32 numChannels,
                  int32 numSamples) noexcept ORIZON_NONBLOCKING
{
    for (int32 channel = 0; channel < numChannels; ++channel) {
        if (source[channel] != destination[channel]) {
            std::memcpy(destination[channel], source[channel],
                        static_cast<size_t>(numSamples) * sizeof(Sample));
        }
    }
}

// Records the input into the ring. Channels flagged silent are recorded as zeros, whatever
// their buffers hold.
template <typename Sample>
void capture(engine::CaptureRing& ring, Sample** channels, int32 numChannels, uint64 silenceFlags,
             int32 numSamples) noexcept ORIZON_NONBLOCKING
{
    std::array<const Sample*, engine::CaptureRing::kMaxChannels> sources{};
    const int32 count = std::min(numChannels, int32{engine::CaptureRing::kMaxChannels});
    for (int32 channel = 0; channel < count; ++channel) {
        const bool silent = (silenceFlags & (uint64{1} << static_cast<uint64>(channel))) != 0;
        sources[static_cast<size_t>(channel)] = silent ? nullptr : channels[channel];
    }
    ring.write(sources.data(), count, numSamples);
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
    engineState = std::make_shared<engine::Engine>();
    engineToken = EngineRegistry::add(engineState);
    addAudioInput(STR16("Input"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
    return kResultOk;
}

tresult PLUGIN_API Processor::terminate()
{
    EngineRegistry::remove(engineToken);
    engineToken = 0;
    engineState.reset();
    return AudioEffect::terminate();
}

tresult PLUGIN_API Processor::connect(IConnectionPoint* other)
{
    const tresult result = AudioEffect::connect(other);
    if (result != kResultOk || engineToken == 0) {
        return result;
    }
    if (IPtr<IMessage> message = owned(allocateMessage())) {
        message->setMessageID(kEngineTokenMessage);
        message->getAttributes()->setInt(kEngineTokenAttribute, engineToken);
        sendMessage(message);
    }
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

tresult PLUGIN_API Processor::process(ProcessData& data) noexcept ORIZON_NONBLOCKING
{
    if (data.numInputs == 0 || data.numOutputs == 0 || data.numSamples <= 0) {
        return kResultOk;
    }
    AudioBusBuffers& input = data.inputs[0];
    AudioBusBuffers& output = data.outputs[0];
    const int32 numChannels = std::min(input.numChannels, output.numChannels);

    if (data.symbolicSampleSize == kSample64) {
        copyChannels(input.channelBuffers64, output.channelBuffers64, numChannels, data.numSamples);
        if (engineState) {
            capture(engineState->captureRing(), input.channelBuffers64, input.numChannels,
                    input.silenceFlags, data.numSamples);
        }
    } else {
        copyChannels(input.channelBuffers32, output.channelBuffers32, numChannels, data.numSamples);
        if (engineState) {
            capture(engineState->captureRing(), input.channelBuffers32, input.numChannels,
                    input.silenceFlags, data.numSamples);
        }
    }
    output.silenceFlags = input.silenceFlags;
    return kResultOk;
}

} // namespace orizon::plugin
