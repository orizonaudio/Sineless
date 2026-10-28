#include "AllocationCounter.h"
#include "AudioUnit.h"
#include "SampleRates.h"
#include "engine/CaptureRing.h"
#include "engine/EngineRegistry.h"
#include "engine/InstanceRegistry.h"
#include "engine/RealtimeSafety.h"
#include "engine/State.h"

#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace orizon::au {
namespace {

using protocol::SettingId;

// The Audio Unit, registered in this process the way a host finds an installed one.
AudioComponent component()
{
    static AudioComponent registered = ausdk::AUBaseFactory<AudioUnit>::Register(
        kComponentDescription.componentType, kComponentDescription.componentSubType,
        kComponentDescription.componentManufacturer, CFSTR("Orizon Audio: Sineless"), 1);
    return registered;
}

// What the host plays into the Audio Unit and tells it about the transport.
struct Host
{
    std::vector<std::vector<float>> input; // One buffer per channel, for the next render.
    bool silent = false;
    OSStatus beatAndTempo = noErr;
    double beat = 0.0;
    double tempo = 120.0;
    OSStatus musicalTime = noErr;
    float numerator = 4.0F;
    UInt32 denominator = 4;
    OSStatus transport = noErr;
    bool playing = true;
    double sampleInTimeline = 0.0;
};

OSStatus provideInput(void* reference, AudioUnitRenderActionFlags* flags,
                      const AudioTimeStamp* /*time*/, UInt32 /*bus*/, UInt32 frames,
                      AudioBufferList* data) noexcept ORIZON_NONBLOCKING
{
    auto* host = static_cast<Host*>(reference);
    for (UInt32 channel = 0; channel < data->mNumberBuffers; ++channel) {
        AudioBuffer& buffer = data->mBuffers[channel]; // NOLINT: AU's flexible array
        std::vector<float>& source = host->input[channel];
        if (buffer.mData == nullptr) {
            buffer.mData = source.data();
        } else {
            std::memcpy(buffer.mData, source.data(), frames * sizeof(float));
        }
    }
    if (host->silent) {
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
    }
    return noErr;
}

OSStatus beatAndTempo(void* reference, Float64* beat, Float64* tempo) noexcept ORIZON_NONBLOCKING
{
    const auto* host = static_cast<const Host*>(reference);
    if (beat != nullptr) {
        *beat = host->beat;
    }
    if (tempo != nullptr) {
        *tempo = host->tempo;
    }
    return host->beatAndTempo;
}

OSStatus musicalTimeLocation(void* reference, UInt32* /*deltaToNextBeat*/, Float32* numerator,
                             UInt32* denominator, Float64* /*downBeat*/) noexcept ORIZON_NONBLOCKING
{
    const auto* host = static_cast<const Host*>(reference);
    if (numerator != nullptr) {
        *numerator = host->numerator;
    }
    if (denominator != nullptr) {
        *denominator = host->denominator;
    }
    return host->musicalTime;
}

OSStatus transportState(void* reference, Boolean* playing, Boolean* /*changed*/,
                        Float64* sampleInTimeline, Boolean* /*cycling*/, Float64* /*cycleStart*/,
                        Float64* /*cycleEnd*/) noexcept ORIZON_NONBLOCKING
{
    const auto* host = static_cast<const Host*>(reference);
    if (playing != nullptr) {
        *playing = host->playing ? 1 : 0;
    }
    if (sampleInTimeline != nullptr) {
        *sampleInTimeline = host->sampleInTimeline;
    }
    return host->transport;
}

AudioStreamBasicDescription floatFormat(double rate, UInt32 channels)
{
    return {.mSampleRate = rate,
            .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked) |
                            static_cast<AudioFormatFlags>(kAudioFormatFlagIsNonInterleaved),
            .mBytesPerPacket = sizeof(float),
            .mFramesPerPacket = 1,
            .mBytesPerFrame = sizeof(float),
            .mChannelsPerFrame = channels,
            .mBitsPerChannel = 32,
            .mReserved = 0};
}

constexpr UInt32 kMaximumFrames = 4096;

// One instance of the Audio Unit, set up as a host does: formats, the input callback, the host's
// transport callbacks, then initialised.
class Instance
{
public:
    explicit Instance(Host& host) : host(host)
    {
        EXPECT_EQ(AudioComponentInstanceNew(component(), &unit), noErr);
    }
    ~Instance()
    {
        if (initialized) {
            AudioUnitUninitialize(unit);
        }
        AudioComponentInstanceDispose(unit);
    }
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    // The result of initialising with these formats. A host that renders into its own buffers may
    // tell the Audio Unit not to allocate output buffers (`outputAllocates`); the output then
    // never shares the input's buffers.
    OSStatus open(double rate, UInt32 inputChannels, UInt32 outputChannels,
                  bool outputAllocates = true)
    {
        const AudioStreamBasicDescription input = floatFormat(rate, inputChannels);
        const AudioStreamBasicDescription output = floatFormat(rate, outputChannels);
        if (const OSStatus result =
                AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                     0, &input, sizeof input);
            result != noErr) {
            return result;
        }
        if (const OSStatus result =
                AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                     0, &output, sizeof output);
            result != noErr) {
            return result;
        }
        const UInt32 allocates = outputAllocates ? 1 : 0;
        AudioUnitSetProperty(unit, kAudioUnitProperty_ShouldAllocateBuffer, kAudioUnitScope_Output,
                             0, &allocates, sizeof allocates);
        UInt32 maximum = kMaximumFrames;
        AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                             0, &maximum, sizeof maximum);
        AURenderCallbackStruct callback{.inputProc = provideInput, .inputProcRefCon = &host};
        AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                             &callback, sizeof callback);
        HostCallbackInfo callbacks{};
        callbacks.hostUserData = &host;
        callbacks.beatAndTempoProc = beatAndTempo;
        callbacks.musicalTimeLocationProc = musicalTimeLocation;
        callbacks.transportStateProc = transportState;
        AudioUnitSetProperty(unit, kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0,
                             &callbacks, sizeof callbacks);
        const OSStatus result = AudioUnitInitialize(unit);
        initialized = result == noErr;
        return result;
    }

    // A format change as a host makes it: uninitialise, set the new formats, initialise again.
    OSStatus reopen(double rate, UInt32 channels)
    {
        if (initialized) {
            AudioUnitUninitialize(unit);
            initialized = false;
        }
        return open(rate, channels, channels);
    }

    // Renders `frames` frames of the host's input; `inPlace` lets the Audio Unit pick the output
    // buffers, otherwise they are the host's own. Returns the output, one vector per channel.
    std::vector<std::vector<float>> render(UInt32 frames, bool inPlace,
                                           AudioUnitRenderActionFlags* flagsOut = nullptr)
    {
        const auto channels = static_cast<UInt32>(host.input.size());
        std::vector<std::vector<float>> output(channels, std::vector<float>(frames, -9.0F));
        std::vector<std::byte> storage(offsetof(AudioBufferList, mBuffers) +
                                       sizeof(AudioBuffer) * std::max<UInt32>(channels, 1));
        auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
        list->mNumberBuffers = channels;
        for (UInt32 channel = 0; channel < channels; ++channel) {
            list->mBuffers[channel] = {// NOLINT
                                       .mNumberChannels = 1,
                                       .mDataByteSize = frames * static_cast<UInt32>(sizeof(float)),
                                       .mData = inPlace ? nullptr : output[channel].data()};
        }
        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp time{};
        time.mSampleTime = renderedFrames;
        time.mFlags = kAudioTimeStampSampleTimeValid;
        EXPECT_EQ(AudioUnitRender(unit, &flags, &time, 0, frames, list), noErr);
        renderedFrames += frames;
        for (UInt32 channel = 0; channel < channels; ++channel) {
            const auto* data = static_cast<const float*>(list->mBuffers[channel].mData); // NOLINT
            std::copy_n(data, frames, output[channel].begin());
        }
        if (flagsOut != nullptr) {
            *flagsOut = flags;
        }
        return output;
    }

    // The instance's engine, found as its view finds it: by the token its private property gives.
    [[nodiscard]] std::shared_ptr<engine::Engine> engine() const
    {
        std::int64_t token = 0;
        UInt32 size = sizeof token;
        EXPECT_EQ(AudioUnitGetProperty(unit, kEngineTokenProperty, kAudioUnitScope_Global, 0,
                                       &token, &size),
                  noErr);
        return engine::EngineRegistry::find(token);
    }

    // The host's saved state, and restoring one.
    [[nodiscard]] CFPropertyListRef saveState() const
    {
        CFPropertyListRef state = nullptr;
        UInt32 size = sizeof(CFPropertyListRef);
        EXPECT_EQ(AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global,
                                       0, static_cast<void*>(&state), &size),
                  noErr);
        return state;
    }
    OSStatus restoreState(CFPropertyListRef state)
    {
        return AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0,
                                    static_cast<const void*>(&state), sizeof(CFPropertyListRef));
    }

    void setContextName(CFStringRef name)
    {
        EXPECT_EQ(AudioUnitSetProperty(unit, kAudioUnitProperty_ContextName, kAudioUnitScope_Global,
                                       0, static_cast<const void*>(&name), sizeof(CFStringRef)),
                  noErr);
    }

    [[nodiscard]] ::AudioUnit handle() const { return unit; }

private:
    Host& host;
    ::AudioUnit unit = nullptr;
    bool initialized = false;
    double renderedFrames = 0.0;
};

std::vector<std::vector<float>> noise(UInt32 channels, UInt32 frames, unsigned seed)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> sample(-1.0F, 1.0F);
    std::vector<std::vector<float>> signal(channels, std::vector<float>(frames));
    for (auto& channel : signal) {
        std::ranges::generate(channel, [&] { return sample(random); });
    }
    return signal;
}

std::string readTestFile(const std::string& name)
{
    std::ifstream file(std::string(ORIZON_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// A copy of `state` (a dictionary the Audio Unit saved) whose Sineless entry is `value`, or none.
CFPropertyListRef withEntry(CFPropertyListRef state, CFTypeRef value)
{
    CFMutableDictionaryRef copy =
        CFDictionaryCreateMutableCopy(nullptr, 0, static_cast<CFDictionaryRef>(state));
    CFStringRef key =
        CFStringCreateWithCString(nullptr, AudioUnit::kStateKey, kCFStringEncodingUTF8);
    if (value == nullptr) {
        CFDictionaryRemoveValue(copy, key);
    } else {
        CFDictionarySetValue(copy, key, value);
    }
    CFRelease(key);
    return copy;
}

CFDataRef dataOf(const std::string& bytes)
{
    return CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bytes.data()),
                        static_cast<CFIndex>(bytes.size()));
}

// Settings that differ from the defaults in every way a reset would undo.
engine::Settings notDefaults()
{
    engine::Settings settings;
    settings.set(SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kSeconds));
    settings.set(SettingId::kDisplayMode, static_cast<double>(protocol::DisplayMode::kMulti));
    settings.set(SettingId::kGroup, static_cast<double>(protocol::Group::kC));
    return settings;
}

// Settings that differ from the defaults, saved, then padded with whitespace to `bytes`: still a
// readable state, so only its size can make it load the defaults.
std::string savedAndPaddedTo(std::size_t bytes)
{
    std::string json = engine::serializeState(notDefaults());
    json.resize(bytes, ' ');
    return json;
}

// --- Pass-through: every channel layout, sample rate and block size, in place and not ---------

// Whose buffers the output is rendered into.
enum class Buffers : std::uint8_t
{
    kAudioUnits,          // The host passes none: the output is the input's buffers.
    kHosts,               // The host passes its own; the Audio Unit copies its output there.
    kHostsWithoutOutputs, // The host's own, and the Audio Unit allocates no output buffers.
};

struct PassThroughCase
{
    double rate = 48000.0;
    UInt32 channels = 2;
    UInt32 frames = 512;
    Buffers buffers = Buffers::kAudioUnits;
};

std::string nameOf(const PassThroughCase& c)
{
    return std::to_string(static_cast<int>(c.rate)) + "Hz_" + std::to_string(c.channels) + "ch_" +
           std::to_string(c.frames) + "_" +
           (c.buffers == Buffers::kAudioUnits ? "AudioUnitsBuffers"
            : c.buffers == Buffers::kHosts    ? "HostsBuffers"
                                              : "HostsBuffersWithoutOutputs");
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const PassThroughCase& c, std::ostream* out)
{
    *out << nameOf(c);
}

class PassesThrough : public ::testing::TestWithParam<PassThroughCase>
{
};

// The Audio Unit never changes the audio: what a track plays through it is what it plays without.
TEST_P(PassesThrough, InputUnchangedToTheOutput)
{
    const PassThroughCase c = GetParam();
    Host host;
    Instance instance(host);
    ASSERT_EQ(
        instance.open(c.rate, c.channels, c.channels, c.buffers != Buffers::kHostsWithoutOutputs),
        noErr);
    for (unsigned block = 0; block < 3; ++block) {
        host.input = noise(c.channels, c.frames, block);
        const auto output = instance.render(c.frames, c.buffers == Buffers::kAudioUnits);
        for (UInt32 channel = 0; channel < c.channels; ++channel) {
            ASSERT_EQ(output[channel], host.input[channel]) << "channel " << channel;
        }
    }
}

std::vector<PassThroughCase> passThroughCases()
{
    std::vector<PassThroughCase> cases;
    for (const double rate : engine::kSampleRates) {
        for (const UInt32 channels : {1U, 2U}) {
            for (const UInt32 frames : {1U, 64U, 1000U, kMaximumFrames}) {
                for (const Buffers buffers :
                     {Buffers::kAudioUnits, Buffers::kHosts, Buffers::kHostsWithoutOutputs}) {
                    cases.push_back(
                        {.rate = rate, .channels = channels, .frames = frames, .buffers = buffers});
                }
            }
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(AudioUnit, PassesThrough, ::testing::ValuesIn(passThroughCases()),
                         [](const ::testing::TestParamInfo<PassThroughCase>& info) {
                             return nameOf(info.param);
                         });

// --- Channel layouts
// ------------------------------------------------------------------------------

// Sineless takes a mono or a stereo track, in and out alike; a host offering anything else is
// refused at initialisation rather than processed wrongly.
TEST(AudioUnit, AcceptsOnlyMonoAndStereo)
{
    for (const auto& [in, out, accepted] : std::vector<std::tuple<UInt32, UInt32, bool>>{
             {1, 1, true}, {2, 2, true}, {1, 2, false}, {2, 1, false}, {6, 6, false}}) {
        Host host;
        Instance instance(host);
        EXPECT_EQ(instance.open(48000.0, in, out) == noErr, accepted)
            << in << " in, " << out << " out";
    }
}

// --- Sample rate ---------------------------------------------------------------------------------

class PreparesForTheRate : public ::testing::TestWithParam<double>
{
};

// The engine runs at the host's rate, so times and frequencies read true, and its capture is
// sized for that rate, so a freeze holds its whole span up to 384 kHz.
TEST_P(PreparesForTheRate, TheHostSetsUp)
{
    const double rate = GetParam();
    Host host;
    Instance instance(host);
    ASSERT_EQ(instance.open(rate, 2, 2), noErr);
    EXPECT_EQ(instance.engine()->sampleRate(), rate);
    EXPECT_EQ(instance.engine()->captureRing().capacity(), engine::CaptureRing::capacityFor(rate));
}

INSTANTIATE_TEST_SUITE_P(AudioUnit, PreparesForTheRate, ::testing::ValuesIn(engine::kSampleRates),
                         [](const ::testing::TestParamInfo<double>& info) {
                             return std::to_string(static_cast<int>(info.param)) + "Hz";
                         });

// A host changes the rate by uninitialising, setting the new format and initialising again: the
// engine follows each change, and the capture is resized for it.
TEST(AudioUnit, FollowsAChangeOfRate)
{
    Host host;
    Instance instance(host);
    ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
    for (const double rate : {384000.0, 44100.0}) {
        SCOPED_TRACE(rate);
        ASSERT_EQ(instance.reopen(rate, 2), noErr);
        EXPECT_EQ(instance.engine()->sampleRate(), rate);
        EXPECT_EQ(instance.engine()->captureRing().capacity(),
                  engine::CaptureRing::capacityFor(rate));
        host.input = noise(2, 512, 4);
        const auto output = instance.render(512, true);
        EXPECT_EQ(output, host.input);
    }
}

// The editor learns how many channels the track has, so mono tracks show L alone.
TEST(AudioUnit, TellsTheEngineTheChannelCount)
{
    for (const UInt32 channels : {1U, 2U}) {
        Host host;
        Instance instance(host);
        ASSERT_EQ(instance.open(48000.0, channels, channels), noErr);
        EXPECT_EQ(instance.engine()->inputChannels(), static_cast<int>(channels));
    }
}

// --- Capture
// ----------------------------------------------------------------------------------------

// What passes through is what the scope shows: the capture holds the input, sample for sample.
TEST(AudioUnit, CapturesTheInput)
{
    for (const UInt32 channels : {1U, 2U}) {
        Host host;
        Instance instance(host);
        ASSERT_EQ(instance.open(96000.0, channels, channels), noErr);
        host.input = noise(channels, 777, 7);
        instance.render(777, true);
        const auto engine = instance.engine();
        ASSERT_EQ(engine->captureRing().writeIndex(), 777U);
        for (UInt32 channel = 0; channel < channels; ++channel) {
            std::vector<float> captured(777);
            ASSERT_TRUE(engine->captureRing().read(static_cast<int>(channel), 0, captured));
            EXPECT_EQ(captured, host.input[channel]) << "channel " << channel;
        }
    }
}

// Each rendered block is stamped with when it arrived, which paces free-running windows; while the
// host renders offline (kAudioUnitProperty_OfflineRender), blocks come at no steady rate and are
// not paced. The property reads back as set.
TEST(AudioUnit, StampsEachBlockPacedOnlyWhenNotOffline)
{
    Host host;
    Instance instance(host);
    ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
    host.input = noise(2, 256, 3);
    const auto engine = instance.engine();
    EXPECT_FALSE(engine->blockStamp().latest()) << "no block yet";
    std::uint64_t written = 0;
    for (const UInt32 offline : {0U, 1U, 0U}) {
        SCOPED_TRACE(offline);
        ASSERT_EQ(AudioUnitSetProperty(instance.handle(), kAudioUnitProperty_OfflineRender,
                                       kAudioUnitScope_Global, 0, &offline, sizeof offline),
                  noErr);
        UInt32 read = 7;
        UInt32 size = sizeof read;
        ASSERT_EQ(AudioUnitGetProperty(instance.handle(), kAudioUnitProperty_OfflineRender,
                                       kAudioUnitScope_Global, 0, &read, &size),
                  noErr);
        EXPECT_EQ(read, offline);
        instance.render(256, true);
        written += 256;
        const auto stamp = engine->blockStamp().latest();
        ASSERT_TRUE(stamp.has_value());
        EXPECT_EQ(stamp->index, written);
        EXPECT_EQ(stamp->paced, offline == 0U);
    }
}

// A value of the wrong size for kAudioUnitProperty_OfflineRender is refused and changes nothing.
TEST(AudioUnit, RefusesAnOfflineRenderValueOfTheWrongSize)
{
    Host host;
    Instance instance(host);
    ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
    const UInt32 offline = 1;
    ASSERT_EQ(AudioUnitSetProperty(instance.handle(), kAudioUnitProperty_OfflineRender,
                                   kAudioUnitScope_Global, 0, &offline, sizeof offline),
              noErr);
    const std::uint16_t tooShort = 0;
    EXPECT_NE(AudioUnitSetProperty(instance.handle(), kAudioUnitProperty_OfflineRender,
                                   kAudioUnitScope_Global, 0, &tooShort, sizeof tooShort),
              noErr);
    UInt32 read = 7;
    UInt32 size = sizeof read;
    ASSERT_EQ(AudioUnitGetProperty(instance.handle(), kAudioUnitProperty_OfflineRender,
                                   kAudioUnitScope_Global, 0, &read, &size),
              noErr);
    EXPECT_EQ(read, 1U) << "still offline";
}

// A host that marks its input silent may leave old samples in the buffers: the scope shows
// silence, and the silence passes on to the host.
TEST(AudioUnit, CapturesSilenceAndPassesItOnWhenTheInputIsSilent)
{
    Host host;
    Instance instance(host);
    ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
    host.input = noise(2, 256, 3);
    host.silent = true;
    AudioUnitRenderActionFlags flags = 0;
    instance.render(256, false, &flags);
    EXPECT_NE(flags & kAudioUnitRenderAction_OutputIsSilence, 0U);
    const auto engine = instance.engine();
    for (int channel = 0; channel < 2; ++channel) {
        std::vector<float> captured(256);
        ASSERT_TRUE(engine->captureRing().read(channel, 0, captured));
        EXPECT_EQ(captured, std::vector<float>(256, 0.0F)) << "channel " << channel;
    }
}

// A bypassed Sineless passes the audio on untouched and captures nothing, as a VST3 host that
// bypasses it by not calling it: the scope does not draw what the track no longer runs through it.
TEST(AudioUnit, PassesAudioAndCapturesNothingWhenBypassed)
{
    Host host;
    Instance instance(host);
    ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
    const UInt32 bypass = 1;
    ASSERT_EQ(AudioUnitSetProperty(instance.handle(), kAudioUnitProperty_BypassEffect,
                                   kAudioUnitScope_Global, 0, &bypass, sizeof bypass),
              noErr);
    host.input = noise(2, 512, 9);
    const auto output = instance.render(512, true);
    EXPECT_EQ(output, host.input);
    EXPECT_EQ(instance.engine()->captureRing().writeIndex(), 0U);
}

// Sineless delays nothing, so a host has no latency to compensate on its track; and its output
// ends with its input, so a host may stop calling it as soon as the track goes quiet. Hosts (and
// auval) expect both properties to be answered.
TEST(AudioUnit, ReportsNoLatencyAndNoTail)
{
    Host host;
    Instance instance(host);
    ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
    for (const AudioUnitPropertyID property :
         {kAudioUnitProperty_Latency, kAudioUnitProperty_TailTime}) {
        Float64 seconds = -1.0;
        UInt32 size = sizeof seconds;
        ASSERT_EQ(AudioUnitGetProperty(instance.handle(), property, kAudioUnitScope_Global, 0,
                                       &seconds, &size),
                  noErr)
            << property;
        EXPECT_EQ(seconds, 0.0) << property;
    }
}

// --- Transport
// ---------------------------------------------------------------------------------------

struct TransportCase
{
    const char* name = "";
    Host host;
};

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const TransportCase& c, std::ostream* out)
{
    *out << c.name;
}

class FollowsTheTransport : public ::testing::TestWithParam<TransportCase>
{
};

// Beats mode and the multi-track lanes follow the host's position, tempo and time signature as
// the host reports them through its callbacks; a callback a host does not offer counts as unknown,
// as a VST3 host's missing process context does.
TEST_P(FollowsTheTransport, AsTheHostReportsIt)
{
    for (const double rate : engine::kSampleRates) {
        SCOPED_TRACE(rate);
        Host host = GetParam().host;
        Instance instance(host);
        ASSERT_EQ(instance.open(rate, 2, 2), noErr);
        host.input = noise(2, 512, 1);
        instance.render(512, true);

        engine::HostTimeline expected;
        expected.positionValid = host.beatAndTempo == noErr;
        expected.quarterNotes = host.beat;
        expected.tempoValid = host.beatAndTempo == noErr;
        expected.tempo = host.tempo;
        expected.timeSignatureValid = host.musicalTime == noErr;
        expected.numerator = static_cast<int>(host.numerator);
        expected.denominator = static_cast<int>(host.denominator);
        expected.playing = host.transport == noErr && host.playing;
        expected.secondsValid = host.transport == noErr;
        expected.seconds = host.sampleInTimeline / rate;
        engine::Timeline reference;
        reference.advance(expected, 512, rate);

        const engine::MusicalStamp got = instance.engine()->timeline().latest();
        const engine::MusicalStamp want = reference.latest();
        EXPECT_EQ(got.playing, want.playing);
        EXPECT_DOUBLE_EQ(got.quarterNotes, want.quarterNotes);
        EXPECT_DOUBLE_EQ(got.seconds, want.seconds);
        EXPECT_DOUBLE_EQ(got.tempo, want.tempo);
        EXPECT_EQ(got.numerator, want.numerator);
        EXPECT_EQ(got.denominator, want.denominator);
    }
}

std::vector<TransportCase> transportCases()
{
    const auto make = [](const char* name, const auto& set) {
        TransportCase c{.name = name, .host = {}};
        set(c.host);
        return c;
    };
    return {
        make("Playing44",
             [](Host& h) {
                 h.beat = 17.25;
                 h.tempo = 128.0;
                 h.sampleInTimeline = 352800.0;
             }),
        make("PlayingThreeFour",
             [](Host& h) {
                 h.beat = 6.0;
                 h.tempo = 90.0;
                 h.numerator = 3.0F;
                 h.sampleInTimeline = 176400.0;
             }),
        make("Stopped",
             [](Host& h) {
                 h.beat = 8.0;
                 h.tempo = 100.0;
                 h.playing = false;
             }),
        make("PreRoll",
             [](Host& h) {
                 h.beat = -2.0;
                 h.sampleInTimeline = -44100.0;
             }),
        make("NoBeatAndTempo",
             [](Host& h) {
                 h.beatAndTempo = -1;
                 h.beat = 4.0;
                 h.tempo = 140.0;
             }),
        make("NoMusicalTime",
             [](Host& h) {
                 h.musicalTime = -1;
                 h.numerator = 7.0F;
                 h.denominator = 8;
             }),
        make("NoTransport",
             [](Host& h) {
                 h.transport = -1;
                 h.beat = 3.0;
             }),
    };
}

INSTANTIATE_TEST_SUITE_P(AudioUnit, FollowsTheTransport, ::testing::ValuesIn(transportCases()),
                         [](const ::testing::TestParamInfo<TransportCase>& info) {
                             return std::string(info.param.name);
                         });

// --- Saved state
// ----------------------------------------------------------------------------------

// A project saved with Sineless reopens with every setting as it was.
TEST(AudioUnit, ReopensWithTheSettingsSaved)
{
    Host host;
    CFPropertyListRef state = nullptr;
    {
        Instance saved(host);
        ASSERT_EQ(saved.open(48000.0, 2, 2), noErr);
        saved.engine()->settings().replace(notDefaults());
        state = saved.saveState();
    }
    Instance reopened(host);
    ASSERT_EQ(reopened.open(48000.0, 2, 2), noErr);
    ASSERT_EQ(reopened.restoreState(state), noErr);
    EXPECT_EQ(reopened.engine()->settings().get(), notDefaults());
    CFRelease(state);
}

// Settings saved by any release load through the Audio Unit as they do through the VST3 plugin:
// the same golden states, the same migrations.
TEST(AudioUnit, LoadsEveryGoldenState)
{
    for (const char* file :
         {"state-v1.json", "state-v1-beats.json", "state-v2.json", "state-v2-0.5.json"}) {
        Host host;
        Instance instance(host);
        ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
        const std::string json = readTestFile(file);
        ASSERT_FALSE(json.empty()) << file;
        CFPropertyListRef saved = instance.saveState();
        CFDataRef data = dataOf(json);
        CFPropertyListRef state = withEntry(saved, data);
        ASSERT_EQ(instance.restoreState(state), noErr) << file;
        EXPECT_EQ(instance.engine()->settings().get(), engine::deserializeState(json)) << file;
        CFRelease(state);
        CFRelease(data);
        CFRelease(saved);
    }
}

// A state whose Sineless entry is missing, of the wrong type, far too large or unreadable loads
// every default, as a failed VST3 state does, rather than keeping settings that were never saved.
TEST(AudioUnit, LoadsDefaultsFromAnUnreadableEntry)
{
    CFDataRef garbled = dataOf("{ not json");
    CFDataRef oversized =
        dataOf(savedAndPaddedTo(static_cast<std::size_t>(AudioUnit::kMaxStateBytes) + 1));
    CFStringRef wrongType = CFStringCreateWithCString(
        nullptr, engine::serializeState(notDefaults()).c_str(), kCFStringEncodingUTF8);
    for (const CFTypeRef entry : std::vector<CFTypeRef>{nullptr, garbled, oversized, wrongType}) {
        Host host;
        Instance instance(host);
        ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
        CFPropertyListRef saved = instance.saveState();
        instance.engine()->settings().replace(notDefaults());
        CFPropertyListRef state = withEntry(saved, entry);
        ASSERT_EQ(instance.restoreState(state), noErr);
        EXPECT_EQ(instance.engine()->settings().get(), engine::Settings{});
        CFRelease(state);
        CFRelease(saved);
    }
    CFRelease(garbled);
    CFRelease(oversized);
    CFRelease(wrongType);
}

// The size limit only turns away entries larger than any saved state: one at the limit loads.
TEST(AudioUnit, LoadsAStateAtTheSizeLimit)
{
    Host host;
    Instance instance(host);
    ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
    CFPropertyListRef saved = instance.saveState();
    CFDataRef atLimit =
        dataOf(savedAndPaddedTo(static_cast<std::size_t>(AudioUnit::kMaxStateBytes)));
    CFPropertyListRef state = withEntry(saved, atLimit);
    ASSERT_EQ(instance.restoreState(state), noErr);
    EXPECT_EQ(instance.engine()->settings().get(), notDefaults());
    CFRelease(state);
    CFRelease(atLimit);
    CFRelease(saved);
}

// Another Audio Unit's state is refused, and the settings stay as they are.
TEST(AudioUnit, RefusesAnotherAudioUnitsState)
{
    Host host;
    Instance instance(host);
    ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
    CFPropertyListRef saved = instance.saveState();
    CFMutableDictionaryRef other =
        CFDictionaryCreateMutableCopy(nullptr, 0, static_cast<CFDictionaryRef>(saved));
    const SInt32 subtype = 'Othr';
    CFNumberRef number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &subtype);
    CFDictionarySetValue(other, CFSTR(kAUPresetSubtypeKey), number);
    instance.engine()->settings().replace(notDefaults());
    EXPECT_NE(instance.restoreState(other), noErr);
    EXPECT_EQ(instance.engine()->settings().get(), notDefaults());
    CFRelease(number);
    CFRelease(other);
    CFRelease(saved);
}

// --- Track names
// ------------------------------------------------------------------------------------

// The lane shows the host's track name, and follows a rename; with none, the page shows "Track n".
TEST(AudioUnit, NamesTheLaneAfterTheTrack)
{
    Host host;
    Instance instance(host);
    ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
    instance.setContextName(CFSTR("Audio 1"));
    EXPECT_EQ(instance.engine()->trackInfo().name, "Audio 1");
    instance.setContextName(CFSTR("Kick Mic"));
    EXPECT_EQ(instance.engine()->trackInfo().name, "Kick Mic");
    instance.setContextName(CFSTR(""));
    EXPECT_EQ(instance.engine()->trackInfo().name, "");
    instance.setContextName(CFSTR("Kick Mic"));
    instance.setContextName(nullptr);
    EXPECT_EQ(instance.engine()->trackInfo().name, "");
}

// Live names the context "<device name>/<track name>"; only the track name labels the lane, even
// for a renamed device or a track name that holds a slash. Other hosts send the track name alone.
TEST(AudioUnit, TakesTheTrackNameFromLivesContextName)
{
    EXPECT_EQ(trackNameFrom("Sineless/3-Audio", true), "3-Audio");
    EXPECT_EQ(trackNameFrom("Sineless/Kick Mic", true), "Kick Mic");
    EXPECT_EQ(trackNameFrom("Scope on bus/Drums", true), "Drums");
    EXPECT_EQ(trackNameFrom("Sineless/AC/DC", true), "AC/DC");
    EXPECT_EQ(trackNameFrom("Kick", true), "Kick");
    EXPECT_EQ(trackNameFrom("AC/DC", false), "AC/DC");
    EXPECT_EQ(trackNameFrom("", false), "");
}

// --- Registries
// -----------------------------------------------------------------------------------

// While an instance lives its editor finds its engine and the others in its group see its lane;
// once the host disposes of it, neither finds it any more.
TEST(AudioUnit, IsFoundOnlyWhileItLives)
{
    Host host;
    std::int64_t token = 0;
    std::weak_ptr<engine::Engine> engine;
    {
        Instance instance(host);
        ASSERT_EQ(instance.open(48000.0, 2, 2), noErr);
        UInt32 size = sizeof token;
        ASSERT_EQ(AudioUnitGetProperty(instance.handle(), kEngineTokenProperty,
                                       kAudioUnitScope_Global, 0, &token, &size),
                  noErr);
        engine = engine::EngineRegistry::find(token);
        ASSERT_FALSE(engine.expired());
        const auto group =
            static_cast<protocol::Group>(engine.lock()->settings().get().value(SettingId::kGroup));
        const auto members = engine::InstanceRegistry::shared().members(group);
        EXPECT_TRUE(std::ranges::any_of(
            members, [&](const auto& member) { return member.engine == engine.lock(); }));
    }
    EXPECT_EQ(engine::EngineRegistry::find(token), nullptr);
    EXPECT_TRUE(engine.expired());
}

// --- Real-time safety
// -------------------------------------------------------------------------------

// Rendering never allocates: an allocation on the audio thread can stall playback (dropouts).
TEST(AudioUnit, RendersWithoutAllocating)
{
    if (!testing::kCountsAllocations) {
        GTEST_SKIP() << "allocations are not counted under ThreadSanitizer";
    }
    for (const double rate : engine::kSampleRates) {
        for (const UInt32 channels : {1U, 2U}) {
            Host host;
            Instance instance(host);
            ASSERT_EQ(instance.open(rate, channels, channels), noErr);
            host.input = noise(channels, 512, 5);
            std::vector<std::byte> storage(offsetof(AudioBufferList, mBuffers) +
                                           sizeof(AudioBuffer) * channels);
            auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
            list->mNumberBuffers = channels;
            for (UInt32 channel = 0; channel < channels; ++channel) {
                list->mBuffers[channel] = {.mNumberChannels = 1, // NOLINT
                                           .mDataByteSize = 512 * sizeof(float),
                                           .mData = nullptr};
            }
            AudioTimeStamp time{};
            time.mFlags = kAudioTimeStampSampleTimeValid;
            std::size_t allocations = 0;
            for (int block = 0; block < 4; ++block) {
                AudioUnitRenderActionFlags flags = 0;
                const testing::AllocationCounter counter;
                ASSERT_EQ(AudioUnitRender(instance.handle(), &flags, &time, 0, 512, list), noErr);
                allocations += counter.allocations();
                time.mSampleTime += 512;
                for (UInt32 channel = 0; channel < channels; ++channel) {
                    list->mBuffers[channel].mData = nullptr; // NOLINT
                }
            }
            EXPECT_EQ(allocations, 0U) << rate << " Hz, " << channels << " channels";
        }
    }
}

} // namespace
} // namespace orizon::au
