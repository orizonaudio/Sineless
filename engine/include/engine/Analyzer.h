#pragma once

#include "engine/Engine.h"
#include "engine/HoldBuffer.h"
#include "engine/InstanceRegistry.h"
#include "engine/LaneHold.h"
#include "engine/Settings.h"
#include "engine/SweepImage.h"
#include "engine/Trigger.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace orizon::engine {

// At most this many tracks are drawn in the multi-track view.
inline constexpr std::size_t kMaxLanes = 8;

// One track of the multi-track view, as the UI labels it.
struct LaneLabel
{
    std::uint64_t id = 0; // Stable while the track exists; the UI names a partner by it.
    std::string name;
    std::optional<std::uint32_t> colour;
    bool own = false; // The instance whose editor shows the lanes.

    bool operator==(const LaneLabel&) const = default;
};

// The tracks behind the lanes of frames whose laneSet is `id`, in lane order, and how many more
// tracks of the group are not drawn.
struct LaneSet
{
    std::uint32_t id = 0;
    std::vector<LaneLabel> lanes;
    std::size_t hidden = 0;
};

// Per-editor-session analysis: turns the capture into display frames. It is
// created when the editor opens, so the hold buffer is allocated only then.
//
// In the multi-track view it reads the other instances of its group from `registry`.
//
// Commands may come from any non-audio thread; produce() runs on the analysis worker.
class Analyzer
{
public:
    explicit Analyzer(Engine& engine,
                      const InstanceRegistry& registry = InstanceRegistry::shared());

    // Device-pixel width of the trace area; no frames are produced while it is 0.
    void setColumns(std::size_t columns);
    void setFrozen(bool frozen);
    // While frozen: how far back to move the view, in seconds.
    void setPan(double seconds);
    // Single mode: wait for the next trigger again.
    void arm();
    // View zoom: the part of the span on screen, as fractions. Out-of-range or reversed
    // values are ignored. Only that part is reduced to screen columns.
    void setView(double start, double end);
    // Compare view: the partner track, by lane id, and whether the sum previews the fix.
    void setPartner(std::uint64_t id);
    void setPreviewFix(bool on);

    // Writes the next frame into `frame`. Returns false when there is nothing new to show.
    bool produce(std::vector<std::byte>& frame);

    // The lanes of the latest multi-track frames; the id changes whenever they do. Any thread.
    [[nodiscard]] LaneSet laneSet() const;

private:
    struct Commands
    {
        std::size_t columns = 0;
        bool frozen = false;
        double panSeconds = 0.0;
        bool armRequested = false;
        double viewStart = 0.0;
        double viewEnd = 1.0;
        std::optional<std::uint64_t> partner;
        bool previewFix = false;
    };

    // The last window shown while live, which a freeze holds on to.
    struct Window
    {
        double start = 0.0;
        double span = 0.0;
        bool triggered = false;
        bool roll = false;
        // Tempo-synced sweep, read from the sweep image; cursor in 0 to 1.
        bool synced = false;
        bool limited = false;
        double cursor = -1.0;
        double cycleStart = 0.0;
        double spanQuarterNotes = 0.0;
        double barQuarterNotes = 0.0;
        // View zoom: the part of the window reduced into the frame (`crop`, fractions of the
        // window) and the part the frame reports covering (`view`, fractions of the span).
        double cropStart = 0.0;
        double cropEnd = 1.0;
        double viewStart = 0.0;
        double viewEnd = 1.0;
        // Readouts always cover the whole span; 0 means the window itself.
        std::uint64_t readoutStart = 0;
        std::uint64_t readoutCount = 0;
    };

    // One track of the multi-track view: its own persistent sweep, and its audio while frozen.
    struct Lane
    {
        std::shared_ptr<Engine> engine;
        SweepImage image;
        std::optional<SweepWindow> window;
        std::uint64_t end = 0; // Capture end of the last update.
        LaneHold hold;
    };

    Commands takeCommands();
    std::uint64_t selectLanes(const Settings& settings);
    bool produceLanes(const Settings& settings, const Commands& command,
                      std::vector<std::byte>& frame);
    bool renderLanes(const SampleSource& ownSource, const Window& window, std::uint64_t sampleCount,
                     std::size_t columns, std::vector<std::byte>& frame);
    void publishLanes(std::vector<LaneLabel> labels, std::size_t hidden);
    // Auto-freeze: a transport stop freezes the display, and playing again ends that freeze.
    void followTransport(const Settings& settings, Commands& command);
    bool freeze(double sampleRate, std::uint64_t minimumSamples = 0);
    bool render(const SampleSource& source, const Settings& settings, double sampleRate,
                const Window& window, std::uint64_t sampleCount, double samplesPerColumn,
                std::size_t columns, std::vector<std::byte>& frame);

    Engine& engine;
    const InstanceRegistry& registry;
    HoldBuffer hold;
    SweepImage image;
    TriggerState trigger;

    std::mutex commandMutex;
    Commands commands;

    // Worker-only state.
    bool isFrozen = false;
    std::optional<Window> lastLive;
    std::optional<Window> lastShown;
    std::optional<Settings> lastSettings;
    std::size_t lastColumns = 0;
    double lastPan = 0.0;
    double lastViewStart = 0.0;
    double lastViewEnd = 1.0;
    bool lastFrozen = false;
    // Auto-freeze: the transport state of the previous step, and whether the current freeze came
    // from a stop (only such a freeze ends when the transport plays again).
    std::optional<bool> lastPlaying;
    bool autoFrozen = false;
    std::uint32_t sequence = 0;
    // Tempo sync: the clock and capture end when frozen, and the capture end of the last frame.
    MusicalStamp frozenStamp;
    std::uint64_t frozenEnd = 0;
    std::uint64_t lastEnd = 0;
    // Multi-track view: lanes by member serial, and their order on screen.
    std::map<std::uint64_t, std::unique_ptr<Lane>> lanes;
    std::vector<std::uint64_t> laneOrder;
    std::uint64_t ownLane = 0; // Serial of this instance's lane.
    std::uint32_t lastLaneSet = 0;
    mutable std::mutex laneMutex;
    LaneSet published;
    std::vector<float> triggerSamples;
    std::vector<float> viewSamples;
    std::vector<float> scratch;
    std::vector<float> reduced;
    std::vector<float> frameData;
};

} // namespace orizon::engine
