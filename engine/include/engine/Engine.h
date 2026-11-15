#pragma once

#include "engine/BlockClock.h"
#include "engine/CaptureRing.h"
#include "engine/MonotonicClock.h"
#include "engine/Settings.h"
#include "engine/State.h"
#include "engine/Timeline.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
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

    // The right edge of each live frame produced in Trigger, Free or Beats: when it was produced
    // (monotonicNanos), the capture index it ended at, and the newest sample captured then. For
    // benchmarks, which report how evenly it moved (Motion.h). The analysis worker records; a
    // benchmark takes them. Keeps the newest kEdgeRecords.
    struct EdgeRecord
    {
        std::uint64_t nanos = 0;
        std::uint64_t edge = 0;
        std::uint64_t newest = 0;
    };
    static constexpr std::size_t kEdgeRecords = 8192;
    void recordEdge(const EdgeRecord& record)
    {
        const std::scoped_lock lock(edgeMutex);
        if (edgeLog.size() == kEdgeRecords) {
            edgeLog.erase(edgeLog.begin(), edgeLog.begin() + kEdgeRecords / 2);
        }
        edgeLog.push_back(record);
    }
    [[nodiscard]] std::vector<EdgeRecord> takeEdges()
    {
        const std::scoped_lock lock(edgeMutex);
        return std::exchange(edgeLog, {});
    }

    // When the newest block arrived, which paces free-running windows.
    [[nodiscard]] BlockStamp& blockStamp() { return stamp; }
    // Audio thread, after capturing a block that had samples: stamps it with the time now.
    // `paced` says the host processes in real time (not offline or ahead of time).
    void stampBlock(bool paced) noexcept ORIZON_NONBLOCKING
    {
        stamp.publish({.index = ring.writeIndex(), .nanos = monotonicNanos(), .paced = paced});
    }
    [[nodiscard]] const BlockStamp& blockStamp() const { return stamp; }

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

    // The other tracks this instance's multi-track view shows, by lane id (Member::serial); none
    // in a new instance, which shows its own track alone. Kept with the instance, not its editor,
    // so that closing and opening the editor keeps them; like zoom, they are not saved, so lane
    // ids, valid only while the plugin is loaded, are enough. Non-audio threads only.
    void setShown(std::vector<std::uint64_t> ids)
    {
        const std::scoped_lock lock(trackMutex);
        shownIds = std::move(ids);
    }
    [[nodiscard]] std::vector<std::uint64_t> shown() const
    {
        const std::scoped_lock lock(trackMutex);
        return shownIds;
    }
    // Forgets the shown tracks that are not among `live`, the instances there are: an instance
    // that went away leaves the selection, in one step with the editor's changes to it.
    void keepShown(const std::vector<std::uint64_t>& live)
    {
        const std::scoped_lock lock(trackMutex);
        std::erase_if(shownIds,
                      [&](std::uint64_t id) { return std::ranges::find(live, id) == live.end(); });
    }

    // The editor's size as the user chose it (EditorCore's ChosenSize), or none: the default.
    // Saved with the project and read when the editor opens, so a state loaded while it is open
    // applies at the next open. Non-audio threads only.
    void setEditorSize(std::optional<EditorSize> size)
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
    BlockStamp stamp;
    std::mutex edgeMutex;
    std::vector<EdgeRecord> edgeLog;
    mutable std::mutex trackMutex;
    TrackInfo track;
    std::vector<std::uint64_t> shownIds;
    std::optional<EditorSize> editorSizeValue;
};

} // namespace orizon::engine
