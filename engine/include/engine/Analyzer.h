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

#include <algorithm>
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

// A compared track's match is searched over the kMatchWindowSeconds of audio before the playhead
// (or the newest of the view, zoomed away from it), continuing at the right edge into the end of
// the previous pass when the playhead is near the left one: a match is a property of the routing,
// not of the whole span, and a search over a long view would stall the frames. Each track's match
// is searched again at most every kMatchRefreshSeconds of audio, one track per frame and at most
// one every kMatchSpacingSeconds, so that many tracks share the analysis thread.
inline constexpr double kMatchWindowSeconds = protocol::kMatchWindowSeconds;
inline constexpr double kMatchRefreshSeconds = 0.5;
inline constexpr double kMatchSpacingSeconds = 0.1;
// A match search that could not read all its audio is tried again this soon.
inline constexpr double kMatchRetrySeconds = 0.05;

// Levels (peak and energy) are kept per block of this many seconds of audio, so that readouts and
// summing gains cost a pass over blocks; the Compare view judges per block whether tracks sound
// together.
inline constexpr double kStatsBlockSeconds = 0.01;

// When the parts of a Compare sum change (a track checked, or a match moved), its levels and
// columns are summed again over the whole sweep, at most this many samples of compared tracks per
// frame: a long sweep of many tracks is redone over a few frames instead of stalling one.
inline constexpr std::size_t kSumRefreshSamples = std::size_t{1} << 21U;

// At most this many lanes are drawn in the multi-track view: tracks, and in the Compare view
// their sum.
inline constexpr std::size_t kMaxLanes = protocol::kMaxLanes;

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
    // Places the audio captured since the last frame into the tempo-synced sweeps without
    // producing a frame: for when no frame is wanted, so that nothing is missed meanwhile however
    // short the capture. Runs on the analysis worker, like produce().
    void advance();

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
        bool partial = false; // The sweep's first pass is still filling in.
        // The part of the source holding the span shown: less than all of it while frozen on a
        // shorter span cropped from a longer sweep.
        double subStart = 0.0;
        double subEnd = 1.0;
        // View zoom: the part of the window reduced into the frame (`crop`, fractions of the
        // window) and the part the frame reports covering (`view`, fractions of the span).
        double cropStart = 0.0;
        double cropEnd = 1.0;
        double viewStart = 0.0;
        double viewEnd = 1.0;
        // Frozen in Seconds mode: how far back the view can be panned (seconds), to the start of
        // the held audio.
        double panReach = 0.0;
        // Readouts always cover the whole span; 0 means the window itself.
        std::uint64_t readoutStart = 0;
        std::uint64_t readoutCount = 0;
    };

    // A sum's recomputation spread over frames: `remaining` positions from `cursor`, wrapping.
    struct SumRefresh
    {
        std::size_t cursor = 0;
        std::size_t remaining = 0;
    };

    // One track of the multi-track view: its own persistent sweep, and its audio while frozen.
    struct Lane
    {
        std::shared_ptr<Engine> engine;
        SweepImage image{1}; // The track's Mid.
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
        // Tempo-synced Compare, while the fix shifts this track: its levels per block as shifted,
        // for the fixed summing gain, the shift they were measured for and their pending
        // recomputation (see updateSumStats).
        BlockStats fixedStats;
        SumRefresh fixedRefresh;
        std::vector<std::int64_t> fixedKey;
        std::vector<std::int64_t> lastFixedKey;
    };
    struct SumPart;
    // Screen or window positions [first, end).
    struct Positions
    {
        std::size_t first = 0;
        std::size_t end = 0;
    };

    Commands takeCommands();
    Commands peekCommands();
    std::uint64_t selectLanes(const Settings& settings, const Commands& command);
    // Live: places each lane's new audio into its sweep. True if any lane changed.
    [[nodiscard]] bool inputsChanged(const Settings& settings, const Commands& command,
                                     std::uint32_t laneSetId) const;
    void rememberInputs(const Settings& settings, const Commands& command, std::uint32_t laneSetId);
    bool placeLanes(const Settings& settings, protocol::BeatSpan span);
    [[nodiscard]] std::uint64_t sharedLength(std::uint64_t serial, const SweepWindow& sweep) const;
    void catchUpToHold(Lane& lane, protocol::BeatSpan span);
    bool showFrozenParts(protocol::BeatSpan span, double start, double end);
    [[nodiscard]] std::uint32_t sumLaneBit(std::uint32_t laneBits) const;
    bool produceLanes(const Settings& settings, const Commands& command,
                      std::vector<std::byte>& frame);
    bool renderLanes(const SampleSource& ownSource, const Window& window, std::uint64_t sampleCount,
                     const Commands& command, std::vector<std::byte>& frame);
    // Seconds mode: the Compare frame of `window` (`sampleCount` samples from `start` of this
    // track, visible over [first, end), `rolling` on fixed column boundaries), from the lanes'
    // sources in `laneSources`.
    void renderCompare(FrameHeaderValues header, const Window& window, std::uint64_t start,
                       std::uint64_t sampleCount, std::size_t first, std::size_t end, bool rolling,
                       const Commands& command, std::vector<std::byte>& frame);
    // `playhead` is the sweep's playhead as a screen position, where the newest audio is.
    void appendComparison(FrameHeaderValues& header, std::size_t first, std::size_t end,
                          std::uint64_t count, std::size_t columns, bool previewFix,
                          std::size_t playhead);
    // The compared track whose match to search this frame, as a lane index, if any.
    std::optional<std::size_t> matchToSearch(std::uint32_t laneSet, std::size_t first,
                                             std::size_t end, std::size_t length,
                                             std::uint64_t now);
    void publishLanes(std::vector<LaneLabel> labels, std::size_t hidden);
    [[nodiscard]] bool readoutsDue(std::uint64_t end) const;
    void storeReadouts(const Readouts& values, std::uint64_t end);
    // Puts the latest readouts in `header`, and notes what they were measured under.
    void applyReadouts(FrameHeaderValues& header);
    // Keeps `stats`, the levels of `view` of `image` per block, in step with the samples placed
    // since the last frame; all of them when `fresh`. False if the image could not be read.
    bool updateStats(BlockStats& stats, const SweepImage& image, protocol::ChannelView view,
                     int channels, bool fresh);
    // Readouts of samples [first, end) of a sweep: their levels from `stats`, and the frequency
    // over the newest second of `view` in `live` (the capture, or what was held on freeze) ending
    // at `liveEnd`. False, with `readouts` untouched, if that audio is gone.
    bool measureSweepReadouts(const BlockStats& stats, std::size_t first, std::size_t end,
                              const SampleSource& live, std::uint64_t liveEnd,
                              protocol::ChannelView view, int channels, Readouts& readouts);
    // Tempo-synced Compare frames, over screen positions of the sweep of `length` samples: lane
    // `index`'s Mid from `from` (silent while the lane is not on that length), and the sum of
    // `parts` from `position`.
    void readLaneImage(std::size_t index, std::size_t length, std::uint64_t from,
                       std::span<float> into);
    void readSum(const std::vector<SumPart>& parts, std::size_t length, std::size_t position,
                 std::span<float> into);
    // Keeps `stats`, the levels of the sum of `parts` per block, in step: where compared tracks
    // changed this frame, widened by `reach`, and when `key` differs from `lastKey`, all of them
    // again over the next frames (see kSumRefreshSamples); all at once for a new length. True if
    // levels were measured again beyond this frame's changes.
    bool updateSumStats(BlockStats& stats, SumRefresh& refresh,
                        const std::vector<std::int64_t>& key, std::vector<std::int64_t>& lastKey,
                        const std::vector<SumPart>& parts, std::size_t reach, std::size_t length,
                        std::size_t block);
    // The energies a summing gain over screen positions `ranges` needs: into `tracks`, each of
    // `parts` (shifted as it is), and into `sums` their sum, whose levels per block `sumStats`
    // keeps; per block inside each range and per piece of a block at its edges, so that the gain
    // covers exactly those positions.
    void collectEnergies(std::span<const Positions> ranges, std::size_t length, std::size_t block,
                         const std::vector<SumPart>& parts, const BlockStats& sumStats,
                         std::vector<std::vector<double>>& tracks,
                         std::vector<std::span<const double>>& spans, std::vector<double>& sums);
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
    std::optional<SweepWindow> imageWindow; // The window `image` was built for.
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
    // Compare view: whether this frame compares tracks; in Seconds mode, each lane's source and
    // where this frame's window starts in it, the sum lane's columns as the window rolls, and
    // the compared lanes' visible samples, as recorded and as fixed, for the gains.
    bool comparing = false;
    bool lastPreviewFix = false;
    std::vector<std::pair<const SampleSource*, std::uint64_t>> laneSources;
    RollColumns sumRoll;
    std::vector<float> sumSamples;
    std::vector<std::vector<float>> laneSamples;
    std::vector<std::vector<float>> fixedLanes;
    std::vector<std::span<const float>> rawTracks;
    std::vector<std::span<const float>> fixedTracks;
    // Each compared track's last match by serial, with the capture end when it was searched, and
    // the lanes and visible part it was searched over.
    struct MatchState
    {
        PairAnalysis analysis;
        std::uint64_t analysedAt = 0;
        std::uint64_t wait = 0; // Audio to pass after analysedAt before the next search.
    };
    std::map<std::uint64_t, MatchState> matchStates;
    std::uint64_t lastMatchSearch = 0; // Capture end of the last search.
    bool matchesPending = false;       // Matches remain to search: frames go on meanwhile.
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
    // Tempo-synced: the sum lane's span, visible part and columns, and its pending recomputation.
    std::vector<std::int64_t> sumGeometry;
    std::vector<std::int64_t> lastSumGeometry;
    SumRefresh sumRefresh;
    std::vector<float> sumScratch;
    // The sums' levels per block, and the parts they were measured for.
    BlockStats rawSumStats;
    BlockStats fixedSumStats;
    std::vector<std::int64_t> rawSumKey;
    std::vector<std::int64_t> lastRawSumKey;
    std::vector<std::int64_t> fixedSumKey;
    std::vector<std::int64_t> lastFixedSumKey;
    SumRefresh rawSumRefresh;
    SumRefresh fixedSumRefresh;
    // Whether a sum is still being recomputed: frames go on meanwhile, also while frozen.
    // A track's shifted levels (Lane::fixedRefresh) need no term of their own: a shift that changes
    // them changes the fixed sum's parts too, and the sum, of at least two parts, is redone at
    // most half as fast, so it is still refreshing until they are done.
    [[nodiscard]] bool sumsRefreshing() const
    {
        return sumRefresh.remaining > 0 || rawSumRefresh.remaining > 0 ||
               fixedSumRefresh.remaining > 0;
    }
    // The match search's copies of the visible part, and the gains' energies.
    std::vector<float> matchOwn;
    std::vector<float> matchTrack;
    std::vector<float> pieceScratch;
    std::vector<float> pieceSumScratch;
    std::vector<std::vector<double>> trackEnergies;
    std::vector<std::span<const double>> energySpans;
    std::vector<std::vector<double>> fixedTrackEnergies;
    std::vector<std::span<const double>> fixedEnergySpans;
    std::vector<SumPart> onePart; // One part of a sum, read alone.
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
