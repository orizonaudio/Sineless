#pragma once

#include "engine/BlockStats.h"
#include "engine/Engine.h"
#include "engine/FrameEncoder.h"
#include "engine/HoldBuffer.h"
#include "engine/InstanceRegistry.h"
#include "engine/LaneHold.h"
#include "engine/PairAnalysis.h"
#include "engine/Readouts.h"
#include "engine/Reducer.h"
#include "engine/Settings.h"
#include "engine/SweepImage.h"
#include "engine/Trigger.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orizon::engine {

// Each compared track's match is searched again at most this often (seconds of audio), one track
// per frame: a search over a long view is costly, and a track's alignment rarely changes faster.
inline constexpr double kMatchRefreshSeconds = 0.5;

// Levels (peak and energy) are kept per block of this many seconds of audio, so that readouts and
// summing gains cost a pass over blocks; the Compare view judges per block whether tracks sound
// together.
inline constexpr double kStatsBlockSeconds = 0.01;

// At most this many lanes are drawn in the multi-track view: tracks, and in the Compare view
// their sum.
inline constexpr std::size_t kMaxLanes = 8;

// One track of the multi-track view, as the UI labels it.
struct LaneLabel
{
    std::uint64_t id = 0; // Stable while the track exists; the UI names tracks by it.
    std::string name;
    std::optional<std::uint32_t> colour;
    bool own = false;      // The instance whose editor shows the lanes.
    bool compared = false; // Compare view: summed and measured against this track.

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
    // Compare view: the other tracks compared with this one, by lane id, and whether the sum
    // previews the fix. Until set, the group's first other track is compared.
    void setCompared(std::vector<std::uint64_t> ids);
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
        std::optional<std::vector<std::uint64_t>> compared;
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
        double cycleSeconds = 0.0;
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
        bool compared = false;
        // The lane's screen columns, and the part of the sweep and channel count they reduce.
        ColumnReduction reduction;
        std::size_t reducedFirst = 0;
        int reducedChannels = 0;
        // Seconds mode, rolling: the lane's columns, kept as they scroll.
        RollColumns roll;
        // The lane's levels per block of its sweep, for readouts and summing gains, and the
        // channel count they were measured for.
        BlockStats stats;
        int statsChannels = 0;
    };
    struct SumPart;

    Commands takeCommands();
    std::uint64_t selectLanes(const Settings& settings, const Commands& command);
    bool produceLanes(const Settings& settings, const Commands& command,
                      std::vector<std::byte>& frame);
    bool renderLanes(const SampleSource& ownSource, const Window& window, std::uint64_t sampleCount,
                     const Commands& command, std::vector<std::byte>& frame);
    void renderCompare(FrameHeaderValues header, std::size_t first, std::size_t end,
                       std::size_t columns, bool previewFix, std::vector<std::byte>& frame);
    void appendComparison(FrameHeaderValues& header, std::size_t first, std::size_t end,
                          std::uint64_t count, std::size_t columns, bool previewFix);
    void publishLanes(std::vector<LaneLabel> labels, std::size_t hidden);
    [[nodiscard]] bool readoutsDue(std::uint64_t end) const;
    void storeReadouts(const Readouts& values, std::uint64_t end);
    // Puts the latest readouts in `header`, and notes what they were measured under.
    void applyReadouts(FrameHeaderValues& header);
    // Keeps `stats`, the levels of `view` of `image` per block, in step with the samples placed
    // since the last frame; all of them when `fresh`. False if the image could not be read.
    bool updateStats(BlockStats& stats, const SweepImage& image, protocol::ChannelView view,
                     int channels, bool fresh);
    // Readouts of a sweep: its levels from `stats`, and the frequency over the newest second of
    // `view` in `live` (the capture, or what was held on freeze) ending at `end`. False, with
    // `readouts` untouched, if that audio is gone.
    bool measureSweepReadouts(const BlockStats& stats, const SampleSource& live, std::uint64_t end,
                              protocol::ChannelView view, int channels, Readouts& readouts);
    // Tempo-synced Compare frames, over screen positions of the sweep of `length` samples: lane
    // `index`'s Mid from `from` (silent while the lane is not on that length), and the sum of
    // `parts` from `position`.
    void readLaneImage(std::size_t index, std::size_t length, std::uint64_t from,
                       std::span<float> into);
    void readSum(const std::vector<SumPart>& parts, std::size_t length, std::size_t position,
                 std::span<float> into);
    // Keeps `stats`, the levels of the sum of `parts` per block, in step: all of them when `key`
    // differs from `lastKey`, else where compared tracks changed this frame, widened by `reach`.
    void updateSumStats(BlockStats& stats, const std::vector<std::int64_t>& key,
                        std::vector<std::int64_t>& lastKey, const std::vector<SumPart>& parts,
                        std::size_t reach, std::size_t length, std::size_t block);
    // The energies the summing gains over screen positions [first, end) need: each compared
    // lane's, then the sums' as recorded and as fixed, per block inside the part and per piece of
    // a block at its edges, so that the gains cover exactly the visible part.
    void collectEnergies(std::size_t first, std::size_t end, std::size_t length, std::size_t block);
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
    // Compare view: whether this frame compares tracks, and each lane's raw Mid samples of the
    // window, in lane order, with the sums of the compared ones as they are and as fixed.
    bool comparing = false;
    bool lastPreviewFix = false;
    std::vector<std::vector<float>> laneSamples;
    std::vector<std::vector<float>> fixedLanes;
    std::vector<std::span<const float>> rawTracks;
    std::vector<std::span<const float>> fixedTracks;
    std::vector<float> rawSum;
    std::vector<float> fixedSum;
    std::vector<float> matches;
    // Each compared track's last match by serial, with the capture end when it was searched, and
    // the lanes and visible part it was searched over.
    struct MatchState
    {
        PairAnalysis analysis;
        std::uint64_t analysedAt = 0;
    };
    std::map<std::uint64_t, MatchState> matchStates;
    // Tempo-synced Compare frames: screen positions where compared tracks changed this frame, the
    // sum lane's columns with what they sum (each compared lane, shifted and signed as previewed),
    // and the latest matches and summing gains.
    struct SumPart
    {
        std::size_t lane = 0;
        std::ptrdiff_t offset = 0;
        float sign = 1.0f;
    };
    struct ComparisonCache
    {
        std::vector<float> matches;
        float sumGain = 0.0f;
        float fixedGain = 0.0f;
        std::uint64_t at = 0;
        bool valid = false;
    };
    std::vector<SweepImage::Range> frameChanges;
    ColumnReduction sumReduction;
    std::vector<SumPart> rawParts;   // The compared tracks as recorded...
    std::vector<SumPart> fixedParts; // ...and shifted and flipped as matched.
    std::vector<std::int64_t> sumKey;
    std::vector<std::int64_t> lastSumKey;
    std::vector<float> sumScratch;
    // The sums' levels per block, and the parts they were measured for.
    BlockStats rawSumStats;
    BlockStats fixedSumStats;
    std::vector<std::int64_t> rawSumKey;
    std::vector<std::int64_t> lastRawSumKey;
    std::vector<std::int64_t> fixedSumKey;
    std::vector<std::int64_t> lastFixedSumKey;
    // The match search's copies of the visible part, and the gains' energies.
    std::vector<float> matchOwn;
    std::vector<float> matchTrack;
    std::vector<float> pieceScratch;
    std::vector<std::vector<double>> trackEnergies;
    std::vector<std::span<const double>> energySpans;
    std::vector<double> sumEnergies;
    std::vector<double> fixedSumEnergies;
    ComparisonCache comparisonCache;
    std::uint32_t matchLaneSet = 0;
    std::size_t matchFirst = 0;
    std::size_t matchEnd = 0;
    std::size_t matchCount = 0;
    std::uint32_t lastLaneSet = 0;
    mutable std::mutex laneMutex;
    LaneSet published;
    // The last readouts of the multi-track view and the capture end they were measured at.
    struct ReadoutCache
    {
        Readouts values;
        std::uint64_t end = 0;
        bool valid = false;
        // What they were measured under: frames with other settings or freezing measure again.
        std::optional<Settings> settings;
        bool frozen = false;
    };
    ReadoutCache readoutCache;
    // Single mode: each view's columns (tempo-synced sweep, or roll), and what they reduce.
    std::array<ColumnReduction, protocol::kChannelViewCount> viewReductions;
    std::array<RollColumns, protocol::kChannelViewCount> viewRolls;
    struct ViewReduction
    {
        std::size_t first = 0;
        int channels = 0;
        std::uint8_t views = 0;
    };
    ViewReduction viewReduction;
    // Single mode: the trigger view's levels per block of the tempo-synced sweep, for readouts,
    // and what they were measured for.
    BlockStats sweepStats;
    protocol::ChannelView sweepStatsView = protocol::ChannelView::kMid;
    int sweepStatsChannels = 0;
    Settings frameSettings; // The settings of the frame being produced.
    std::vector<float> readScratch;
    std::vector<float> triggerSamples;
    std::vector<float> viewSamples;
    std::vector<float> scratch;
    std::vector<float> reduced;
    std::vector<float> frameData;
};

} // namespace orizon::engine
