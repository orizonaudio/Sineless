#include "Processor.h"

#include "EngineRegistry.h"
#include "PluginIds.h"
#include "engine/InstanceRegistry.h"
#include "engine/State.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>

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

// The host's musical position for the block, as far as the process context says it is valid.
engine::HostTimeline hostTimeline(const ProcessContext* context) noexcept ORIZON_NONBLOCKING
{
    engine::HostTimeline host;
    if (context == nullptr) {
        return host;
    }
    host.playing = (context->state & ProcessContext::kPlaying) != 0;
    host.positionValid = (context->state & ProcessContext::kProjectTimeMusicValid) != 0;
    host.quarterNotes = context->projectTimeMusic;
    host.tempoValid = (context->state & ProcessContext::kTempoValid) != 0;
    host.tempo = context->tempo;
    host.timeSignatureValid = (context->state & ProcessContext::kTimeSigValid) != 0;
    host.numerator = context->timeSigNumerator;
    host.denominator = context->timeSigDenominator;
    host.secondsValid = context->sampleRate > 0.0;
    host.seconds = host.secondsValid
                       ? static_cast<double>(context->projectTimeSamples) / context->sampleRate
                       : 0.0;
    return host;
}

// Saved states are a few hundred bytes; anything far larger is not ours.
constexpr std::size_t kMaxStateSize = std::size_t{1} << 20U;

// Reads the stream up to its end or a failed read. A read that reports more than it was asked for
// counts as failed: nothing it left in the buffer can be trusted.
std::string readAll(IBStream* stream)
{
    std::string text;
    std::array<char, 4096> buffer{};
    int32 bytesRead = 0;
    while (text.size() < kMaxStateSize &&
           stream->read(buffer.data(), static_cast<int32>(buffer.size()), &bytesRead) ==
               kResultOk &&
           bytesRead > 0 && static_cast<std::size_t>(bytesRead) <= buffer.size()) {
        text.append(buffer.data(), static_cast<std::size_t>(bytesRead));
    }
    return text;
}

// Writes all of `text`. IBStream::write may take fewer bytes than offered and still succeed, so
// the rest is offered again until the stream has it all. A write that fails, takes nothing or
// reports more than it was offered ends it as a failure: the host then knows the state is cut.
tresult writeAll(IBStream* stream, const std::string& text)
{
    const char* next = text.data();
    std::size_t remaining = text.size();
    while (remaining > 0) {
        const auto offered = static_cast<int32>(
            std::min(remaining, static_cast<std::size_t>(std::numeric_limits<int32>::max())));
        int32 bytesWritten = 0;
        const tresult result = stream->write(const_cast<char*>(next), offered, &bytesWritten);
        if (result != kResultOk) {
            return result;
        }
        if (bytesWritten <= 0 || bytesWritten > offered) {
            return kResultFalse;
        }
        next += bytesWritten;
        remaining -= static_cast<std::size_t>(bytesWritten);
    }
    return kResultOk;
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
    engine::InstanceRegistry::shared().add(engineState);
    addAudioInput(STR16("Input"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
    return kResultOk;
}

tresult PLUGIN_API Processor::terminate()
{
    EngineRegistry::remove(engineToken);
    engineToken = 0;
    if (engineState) {
        engine::InstanceRegistry::shared().remove(*engineState);
    }
    engineState.reset();
    return AudioEffect::terminate();
}

tresult PLUGIN_API Processor::setupProcessing(ProcessSetup& setup)
{
    if (engineState) {
        if (active.load()) {
            // The VST3 rules allow this only while inactive; a host that breaks them may be
            // processing, so the capture keeps its size (holds shrink to fit) rather than move.
            engineState->setSampleRate(setup.sampleRate);
        } else {
            engineState->prepare(setup.sampleRate);
        }
    }
    return AudioEffect::setupProcessing(setup);
}

tresult PLUGIN_API Processor::setActive(TBool state)
{
    active.store(state != 0);
    return AudioEffect::setActive(state);
}

tresult PLUGIN_API Processor::getState(IBStream* state)
{
    if (state == nullptr || !engineState) {
        return kInvalidArgument;
    }
    try {
        const std::string text = engine::serializeState(engineState->settings().get());
        return writeAll(state, text);
    } catch (...) {
        return kInternalError;
    }
}

tresult PLUGIN_API Processor::setState(IBStream* state)
{
    if (state == nullptr || !engineState) {
        return kInvalidArgument;
    }
    try {
        engineState->settings().replace(engine::deserializeState(readAll(state)));
        return kResultOk;
    } catch (...) {
        return kInternalError;
    }
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
    const tresult result = AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    if (result == kResultTrue && engineState) {
        engineState->setInputChannels(SpeakerArr::getChannelCount(inputs[0]));
    }
    return result;
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
    if (engineState) {
        engineState->timeline().advance(hostTimeline(data.processContext), data.numSamples,
                                        processSetup.sampleRate);
    }
    return kResultOk;
}

} // namespace orizon::plugin
