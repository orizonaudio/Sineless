#pragma once

#include "ComponentDescription.h"
#include "engine/Engine.h"
#include "engine/RealtimeSafety.h"
#include "engine/Timeline.h"

#include <AudioUnitSDK/AUEffectBase.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace orizon::au {

// A private property (Apple reserves IDs below 64000) through which the AU's view, in the same
// process, finds this instance's engine: its token in engine::EngineRegistry, as an int64.
inline constexpr AudioUnitPropertyID kEngineTokenProperty = 64300;

// The track name for a host's context name. Live sends "<device name>/<track name>" and the other
// hosts the track name alone. Empty means no name: the page shows "Track n".
[[nodiscard]] std::string trackNameFrom(std::string_view contextName, bool hostIsLive);

// The Audio Unit: passes its input to its output unchanged and records it for the editor, as the
// VST3 processor does. One per instance; its engine lives as long as it does.
class AudioUnit final : public ausdk::AUEffectBase
{
public:
    explicit AudioUnit(AudioComponentInstance instance);
    ~AudioUnit() override;
    AudioUnit(const AudioUnit&) = delete;
    AudioUnit& operator=(const AudioUnit&) = delete;

    // Host thread. Sizes the capture for the sample rate and takes the channel count.
    OSStatus Initialize() override;
    UInt32 SupportedNumChannels(const AUChannelInfo** outInfo) override;
    // The output ends with the input: a tail of zero (the SDK's GetTailTime), offered as hosts
    // expect.
    bool SupportsTail() noexcept AUSDK_RTSAFE override { return true; }
    OSStatus GetPropertyInfo(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                             UInt32& outDataSize, bool& outWritable) override;
    OSStatus GetProperty(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                         void* outData) override;
    void PropertyChanged(AudioUnitPropertyID id, AudioUnitScope scope,
                         AudioUnitElement element) override;
    // Saved state: the SDK's dictionary, with the settings as JSON under kStateKey.
    OSStatus SaveState(CFPropertyListRef* outData) override;
    OSStatus RestoreState(CFPropertyListRef plist) override;

    // Audio thread.
    OSStatus ProcessBufferLists(AudioUnitRenderActionFlags& ioActionFlags,
                                const AudioBufferList& inBuffer, AudioBufferList& outBuffer,
                                UInt32 frames) noexcept AUSDK_RTSAFE override;

    [[nodiscard]] const std::shared_ptr<engine::Engine>& engine() const { return engineState; }

    static constexpr const char* kStateKey = "sineless";
    // Saved states are a few hundred bytes; anything far larger is not ours.
    static constexpr CFIndex kMaxStateBytes = CFIndex{1024} * 1024;

private:
    // The host's musical position for the block, from its callbacks (AU's process context).
    [[nodiscard]] engine::HostTimeline
    hostTimeline(double sampleRate) const noexcept ORIZON_NONBLOCKING;

    std::shared_ptr<engine::Engine> engineState;
    std::int64_t engineToken = 0;
};

} // namespace orizon::au
