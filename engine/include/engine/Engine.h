#pragma once

#include "engine/CaptureRing.h"
#include "engine/Settings.h"
#include "engine/Timeline.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace orizon::engine {

// What the host tells an instance about its track, for labels in the multi-track view.
struct TrackInfo
{
    std::string name;
    std::optional<std::uint32_t> colour; // 0xRRGGBB
    std::optional<int> index;            // Position of the track in the host's mixer.

    bool operator==(const TrackInfo&) const = default;
};

// Everything one plugin instance shares between its audio thread and its editor.
// The processor creates it; the controller reaches it through an in-process handle.
class Engine
{
public:
    Engine() = default;

    [[nodiscard]] CaptureRing& captureRing() { return ring; }
    [[nodiscard]] const CaptureRing& captureRing() const { return ring; }

    [[nodiscard]] Timeline& timeline() { return timelineState; }
    [[nodiscard]] const Timeline& timeline() const { return timelineState; }

    [[nodiscard]] SettingsStore& settings() { return settingsStore; }
    [[nodiscard]] const SettingsStore& settings() const { return settingsStore; }

    // Main input channel count (1 or 2), set by the processor on bus negotiation.
    void setInputChannels(int count) { inputChannelCount.store(count, std::memory_order_relaxed); }
    [[nodiscard]] int inputChannels() const
    {
        return inputChannelCount.load(std::memory_order_relaxed);
    }

    // Non-audio threads only.
    void setTrackInfo(TrackInfo info)
    {
        const std::scoped_lock lock(trackMutex);
        track = std::move(info);
    }
    [[nodiscard]] TrackInfo trackInfo() const
    {
        const std::scoped_lock lock(trackMutex);
        return track;
    }

    // The Compare view's other tracks, by lane id (GroupMember::serial); none until the editor
    // first sets them, when the group's first other track is compared. Kept with the instance, not
    // its editor, so that closing and opening the editor keeps them; like zoom, they are not saved,
    // so lane ids, valid only while the plugin is loaded, are enough. Non-audio threads only.
    void setCompared(std::optional<std::vector<std::uint64_t>> ids)
    {
        const std::scoped_lock lock(trackMutex);
        comparedIds = std::move(ids);
    }
    [[nodiscard]] std::optional<std::vector<std::uint64_t>> compared() const
    {
        const std::scoped_lock lock(trackMutex);
        return comparedIds;
    }

    // The editor's size in CSS pixels, as last given (by the host, or the user's drag of the
    // resize grip), or none yet. Kept with the instance, not its editor, so that the editor opens
    // again at the size it was left at; not saved, like the Compare view's tracks. Non-audio
    // threads only.
    struct EditorSize
    {
        double width = 0.0;
        double height = 0.0;
    };
    void setEditorSize(EditorSize size)
    {
        const std::scoped_lock lock(trackMutex);
        editorSizeValue = size;
    }
    [[nodiscard]] std::optional<EditorSize> editorSize() const
    {
        const std::scoped_lock lock(trackMutex);
        return editorSizeValue;
    }

    // Sets the sample rate and sizes the capture for it (CaptureRing::capacityFor), emptying the
    // capture if its size changes. Called by the processor in setupProcessing, before processing
    // starts: it allocates, so never on the audio thread, nor while it writes.
    void prepare(double rate)
    {
        ring.resize(CaptureRing::capacityFor(rate));
        setSampleRate(rate);
    }

    // The sample rate alone, read by the analysis worker; prepare() sets it with the capture.
    void setSampleRate(double rate) { sampleRateValue.store(rate, std::memory_order_relaxed); }
    [[nodiscard]] double sampleRate() const
    {
        return sampleRateValue.load(std::memory_order_relaxed);
    }

private:
    CaptureRing ring;
    std::atomic<double> sampleRateValue{48000.0};
    std::atomic<int> inputChannelCount{2};
    SettingsStore settingsStore;
    Timeline timelineState;
    mutable std::mutex trackMutex;
    TrackInfo track;
    std::optional<std::vector<std::uint64_t>> comparedIds;
    std::optional<EditorSize> editorSizeValue;
};

} // namespace orizon::engine
