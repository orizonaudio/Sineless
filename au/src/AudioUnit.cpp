#include "AudioUnit.h"

#include "engine/CaptureRing.h"
#include "engine/EngineRegistry.h"
#include "engine/InstanceRegistry.h"
#include "engine/PreciseFloatingPoint.h"
#include "engine/State.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>

namespace orizon::au {

namespace {

constexpr std::array<AUChannelInfo, 2> kChannelConfigurations{
    {{.inChannels = 1, .outChannels = 1}, {.inChannels = 2, .outChannels = 2}}};

// Live hosts its Audio Units in its own process, under its own name.
bool hostIsLive()
{
    return std::string_view(getprogname()) == "Live";
}

std::optional<std::string> toString(CFStringRef text)
{
    if (text == nullptr) {
        return std::nullopt;
    }
    const CFIndex length = CFStringGetLength(text);
    const CFIndex size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!CFStringGetCString(text, result.data(), size, kCFStringEncodingUTF8)) {
        return std::nullopt;
    }
    result.resize(std::strlen(result.c_str()));
    return result;
}

} // namespace

std::string trackNameFrom(std::string_view contextName, bool live)
{
    if (live) {
        const auto slash = contextName.find('/');
        if (slash != std::string_view::npos) {
            contextName.remove_prefix(slash + 1);
        }
    }
    return std::string(contextName);
}

AudioUnit::AudioUnit(AudioComponentInstance instance)
    : AUEffectBase(instance), engineState(std::make_shared<engine::Engine>())
{
    engineToken = engine::EngineRegistry::add(engineState);
    engine::InstanceRegistry::shared().add(engineState);
}

AudioUnit::~AudioUnit()
{
    engine::EngineRegistry::remove(engineToken);
    engine::InstanceRegistry::shared().remove(*engineState);
}

OSStatus AudioUnit::Initialize()
{
    const OSStatus result = AUEffectBase::Initialize();
    if (result != noErr) {
        return result;
    }
    // Allocates: the host initialises before it renders, and again after any format change.
    engineState->prepare(GetSampleRate());
    engineState->setInputChannels(static_cast<int>(GetNumberOfChannels()));
    return noErr;
}

UInt32 AudioUnit::SupportedNumChannels(const AUChannelInfo** outInfo)
{
    if (outInfo != nullptr) {
        *outInfo = kChannelConfigurations.data();
    }
    return static_cast<UInt32>(kChannelConfigurations.size());
}

OSStatus AudioUnit::GetPropertyInfo(AudioUnitPropertyID id, AudioUnitScope scope,
                                    AudioUnitElement element, UInt32& outDataSize,
                                    bool& outWritable)
{
    if (id == kEngineTokenProperty && scope == kAudioUnitScope_Global) {
        outDataSize = sizeof(std::int64_t);
        outWritable = false;
        return noErr;
    }
    return AUEffectBase::GetPropertyInfo(id, scope, element, outDataSize, outWritable);
}

OSStatus AudioUnit::GetProperty(AudioUnitPropertyID id, AudioUnitScope scope,
                                AudioUnitElement element, void* outData)
{
    if (id == kEngineTokenProperty && scope == kAudioUnitScope_Global) {
        std::memcpy(outData, &engineToken, sizeof engineToken);
        return noErr;
    }
    return AUEffectBase::GetProperty(id, scope, element, outData);
}

void AudioUnit::PropertyChanged(AudioUnitPropertyID id, AudioUnitScope scope,
                                AudioUnitElement element)
{
    AUEffectBase::PropertyChanged(id, scope, element);
    // The SDK keeps the host's context name itself, so its change is the moment to read it.
    if (id == kAudioUnitProperty_ContextName) {
        engine::TrackInfo track = engineState->trackInfo();
        const auto context = toString(GetContextName());
        track.name = context ? trackNameFrom(*context, hostIsLive()) : std::string();
        engineState->setTrackInfo(track);
    }
}

OSStatus AudioUnit::SaveState(CFPropertyListRef* outData)
{
    const OSStatus result = AUEffectBase::SaveState(outData);
    if (result != noErr || outData == nullptr || *outData == nullptr) {
        return result;
    }
    // The SDK's state is a mutable dictionary it has just made.
    auto* dictionary = static_cast<CFMutableDictionaryRef>(const_cast<void*>(*outData));
    const std::string json = engine::serializeState(engineState->settings().get());
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(json.data()),
                                  static_cast<CFIndex>(json.size()));
    if (data == nullptr) {
        return result;
    }
    CFStringRef key = CFStringCreateWithCString(nullptr, kStateKey, kCFStringEncodingUTF8);
    CFDictionarySetValue(dictionary, key, data);
    CFRelease(key);
    CFRelease(data);
    return result;
}

OSStatus AudioUnit::RestoreState(CFPropertyListRef plist)
{
    // Another Audio Unit's state, or not a state at all: refused, and the settings stay.
    const OSStatus result = AUEffectBase::RestoreState(plist);
    if (result != noErr) {
        return result;
    }
    try {
        CFStringRef key = CFStringCreateWithCString(nullptr, kStateKey, kCFStringEncodingUTF8);
        const void* value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(plist), key);
        CFRelease(key);
        std::string json;
        if (value != nullptr && CFGetTypeID(value) == CFDataGetTypeID()) {
            const auto* data = static_cast<CFDataRef>(value);
            if (CFDataGetLength(data) <= kMaxStateBytes) {
                json.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                            static_cast<std::size_t>(CFDataGetLength(data)));
            }
        }
        // Missing, of the wrong type, too large or unreadable: every default, as in VST3.
        engineState->settings().replace(engine::deserializeState(json));
    } catch (...) {
        return kAudioUnitErr_InvalidPropertyValue;
    }
    return noErr;
}

engine::HostTimeline AudioUnit::hostTimeline(double sampleRate) const noexcept ORIZON_NONBLOCKING
{
    engine::HostTimeline host;
    // The AU contract has hosts make these callbacks safe to call during render, which is the
    // only time they may be called; the SDK's wrappers are not annotated as nonblocking.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
    Float64 beat = 0.0;
    Float64 tempo = 0.0;
    if (CallHostBeatAndTempo(&beat, &tempo) == noErr) {
        host.positionValid = true;
        host.quarterNotes = beat;
        host.tempoValid = tempo > 0.0;
        host.tempo = host.tempoValid ? tempo : host.tempo;
    }
    Float32 numerator = 0.0F;
    UInt32 denominator = 0;
    if (CallHostMusicalTimeLocation(nullptr, &numerator, &denominator, nullptr) == noErr &&
        numerator >= 1.0F && denominator >= 1) {
        host.timeSignatureValid = true;
        host.numerator = static_cast<int>(numerator);
        host.denominator = static_cast<int>(denominator);
    }
    Boolean playing = 0;
    Float64 sampleInTimeline = 0.0;
    if (CallHostTransportState(&playing, nullptr, &sampleInTimeline, nullptr, nullptr, nullptr) ==
        noErr) {
        host.playing = playing != 0;
        host.secondsValid = sampleRate > 0.0;
        host.seconds = host.secondsValid ? sampleInTimeline / sampleRate : 0.0;
    }
#pragma clang diagnostic pop
    return host;
}

OSStatus AudioUnit::ProcessBufferLists(AudioUnitRenderActionFlags& ioActionFlags,
                                       const AudioBufferList& inBuffer, AudioBufferList& outBuffer,
                                       UInt32 frames) noexcept AUSDK_RTSAFE
{
    const UInt32 channels = std::min(inBuffer.mNumberBuffers, outBuffer.mNumberBuffers);
    std::array<const float*, engine::CaptureRing::kMaxChannels> sources{};
    for (UInt32 channel = 0; channel < channels; ++channel) {
        const AudioBuffer& source = inBuffer.mBuffers[channel];       // NOLINT: AU's flexible array
        const AudioBuffer& destination = outBuffer.mBuffers[channel]; // NOLINT
        if (destination.mData != source.mData) {
            std::memcpy(destination.mData, source.mData, frames * sizeof(float));
        }
        if (channel < sources.size()) {
            sources[channel] = static_cast<const float*>(source.mData);
        }
    }
    // A silent input passes on as silent output (the flag stays as the host set it), and is
    // recorded as silence.
    const bool silent = (ioActionFlags & kAudioUnitRenderAction_OutputIsSilence) != 0;
    const std::uint64_t silentChannels = silent ? ~std::uint64_t{0} : 0;
    const auto count = static_cast<int>(std::min<UInt32>(channels, sources.size()));
    engine::captureBlock(engineState->captureRing(), sources.data(), count, silentChannels,
                         static_cast<int>(frames));
    const double sampleRate = GetSampleRate();
    engineState->timeline().advance(hostTimeline(sampleRate), static_cast<int>(frames), sampleRate);
    return noErr;
}

} // namespace orizon::au
