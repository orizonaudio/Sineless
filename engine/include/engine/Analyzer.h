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
// is searched again at most every kMatchRefreshSeconds of audio, one track at a time and at most
// one every kMatchSpacingSeconds, so that many tracks share the analysis thread. A long search is
// spread over frames (kMatchSearchWork), so at high rates the refreshes come further apart.
inline constexpr double kMatchWindowSeconds = protocol::kMatchWindowSeconds;
inline constexpr double kMatchRefreshSeconds = 0.5;
inline constexpr double kMatchSpacingSeconds = 0.1;
// A match search is spread over frames, at most about this many multiply-adds of it in each, so
// that a long window at a high rate (8 s at 96 kHz takes some 36 million) does not stall one
// frame: a few milliseconds' work on a fast machine, so a slower one still keeps its frames.
inline constexpr std::size_t kMatchSearchWork = std::size_t{1} << 23U;
// Seconds mode's Compare gains are measured over frames too, reading or passing over at most
// about this many samples of the compared tracks in each: fifteen tracks over a 4 s view at
// 96 kHz, as recorded and as fixed, are some 11 million read and twice as many passed over.
inline constexpr std::size_t kGainReadWork = std::size_t{1} << 22U;
// A match search that could not read all its audio is tried again this soon. Frozen or stopped,
// searches are not spaced, and this keeps a search that keeps failing from running every frame.
inline constexpr double kMatchRetrySeconds = 0.05;
// Frozen, a match whose audio could not be read is tried again on the frames that follow (a held
// copy caught short is captured again on the next one), at most this many times in all: one that
// can never be read, such as a track holding too little of the view, then stops being tried, so
// that it does not keep the frozen view drawing.
inline constexpr int kFrozenReadTries = 3;

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

// The analysis's work since it was created, counted rather than timed, so that tests and
// benchmarks can check that a frame's cost follows what changed, not the size of what is shown.
// Copying audio is not counted: placing it into sweeps (all of a sweep when one is rebuilt, as
// when its length changes) and holding it on freeze. A rebuilt sweep's columns are then all
// reduced again, which is.
struct AnalysisWork
{
    // Samples read back from the capture, held audio or sweep images to reduce, measure, sum or
    // search them: one per channel view read, so a Mid read from a stereo capture counts once.
    std::uint64_t samplesRead = 0;
    // Of those, the samples reduced into screen columns: views, lanes and the Compare sum lane.
    std::uint64_t samplesReduced = 0;
    // Samples of compared tracks added into a Compare sum, for its columns or its levels.
    std::uint64_t samplesSummed = 0;
    // Of those read, the samples read for match searches: of both tracks of each pair searched,
    // over the newest kMatchWindowSeconds (or less, of a shorter sweep or view).
    std::uint64_t samplesSearched = 0;
    // Searches for a compared track's match, and their multiply-adds (each spread over frames).
    std::uint64_t matchSearches = 0;
    std::uint64_t searchWork = 0;
    // Samples passed over measuring Compare's summing gains (each measurement spread over frames).
    std::uint64_t gainWork = 0;
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
    // previews the fix. The selection is kept with the instance (Engine::compared), so that it
    // outlives this analyzer, but is not saved; until first set, the group's first other track is
    // compared.
    void setCompared(const std::vector<std::uint64_t>& ids);
    void setPreviewFix(bool on);
    // Draws the frame again at the next request, whatever has changed: for a page that has just
    // loaded (or loaded again after its process ended) and has no frame yet.
    void redraw();

    // Writes the next frame into `frame`. Returns false when there is nothing new to show.
    bool produce(std::vector<std::byte>& frame);
    // Places the audio captured since the last frame into the tempo-synced sweeps without
    // producing a frame: for when no frame is wanted, so that nothing is missed meanwhile however
    // short the capture. Runs on the analysis worker, like produce().
    void advance();

    // The lanes of the latest multi-track frames; the id changes whenever they do. Any thread.
    [[nodiscard]] LaneSet laneSet() const;

    // The work done so far. On the analysis worker, like produce().
    [[nodiscard]] const AnalysisWork& work() const { return workDone; }

private:
    struct Commands
    {
        std::size_t columns = 0;
        bool frozen = false;
        double panSeconds = 0.0;
        bool armRequested = false;
        double viewStart = 0.0;
        double viewEnd = 1.0;
        bool previewFix = false;
        std::uint64_t redraws = 0;
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
        // The lane's screen columns, and the part of the sweep they reduce.
        ColumnReduction reduction;
        std::size_t reducedFirst = 0;
        // Seconds mode, rolling: the lane's columns, kept as they scroll.
        RollColumns roll;
        // The lane's levels per block of its sweep, for readouts and summing gains.
        BlockStats stats;
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
    // readView and readViewHeld (ChannelViews.h), counted in AnalysisWork::samplesRead.
    [[nodiscard]] bool readSamples(const SampleSource& source, int inputChannels,
                                   protocol::ChannelView view, std::uint64_t start,
                                   std::span<float> destination, std::span<float> working);
    bool readSamplesHeld(const SampleSource& source, int inputChannels, protocol::ChannelView view,
                         std::uint64_t start, std::span<float> destination,
                         std::span<float> working);
    std::uint64_t selectLanes(const Settings& settings);
    // Live: places each lane's new audio into its sweep. True if any lane changed.
    [[nodiscard]] bool inputsChanged(const Settings& settings, const Commands& command,
                                     std::uint32_t laneSetId) const;
    void rememberInputs(const Settings& settings, const Commands& command, std::uint32_t laneSetId);
    [[nodiscard]] bool formatsChanged(bool multi);
    bool placeLanes(const Settings& settings, protocol::BeatSpan span);
    [[nodiscard]] std::uint64_t sharedLength(std::uint64_t serial, const SweepWindow& sweep) const;
    void catchUpToHold(Lane& lane);
    void catchUpImageToHold();
    bool showFrozenParts(protocol::BeatSpan span, double start, double end);
    [[nodiscard]] std::uint32_t sumLaneBit(std::uint32_t laneBits) const;
    // What produce draws, before it leaves out a frame that would repeat the last sent.
    bool produceFrame(std::vector<std::byte>& frame);
    // The frame was drawn only for analysis results (a search or gains that ended, with no new
    // audio, input or format): if it came out as the last frame sent, it is not sent again.
    bool resultsOnly = false;
    std::vector<std::byte> lastSent;
    bool produceLanes(const Settings& settings, const Commands& command,
                      std::vector<std::byte>& frame);
    // `sampleRate` is the rate of the audio shown: frozen, the one it was held at.
    bool renderLanes(const SampleSource& ownSource, const Window& window, std::uint64_t sampleCount,
                     double sampleRate, const Commands& command, std::vector<std::byte>& frame);
    // Seconds mode: the Compare frame of `window` (`sampleCount` samples from `start` of this
    // track, visible over [first, end), `rolling` on fixed column boundaries), from the lanes'
    // sources in `laneSources`, whose audio is at `sampleRate`.
    void renderCompare(FrameHeaderValues header, const Window& window, std::uint64_t start,
                       std::uint64_t sampleCount, std::size_t first, std::size_t end, bool rolling,
                       double sampleRate, const Commands& command, std::vector<std::byte>& frame);
    // `playhead` is the sweep's playhead as a screen position, where the newest audio is; the
    // lanes' audio is at `sampleRate`.
    void appendComparison(FrameHeaderValues& header, std::size_t first, std::size_t end,
                          std::uint64_t count, std::size_t columns, bool previewFix,
                          std::size_t playhead, double sampleRate);
    // The compared track whose match to search this frame, as a lane index, if any.
    std::optional<std::size_t> matchToSearch(std::uint32_t laneSet, std::size_t first,
                                             std::size_t end, std::size_t length, std::uint64_t now,
                                             std::uint64_t shownFrom, double viewStart,
                                             double viewEnd);
    void publishLanes(std::vector<LaneLabel> labels, std::size_t hidden);
    [[nodiscard]] bool readoutsDue(std::uint64_t end) const;
    void storeReadouts(const Readouts& values, std::uint64_t end);
    // Puts the latest readouts in `header`, and notes what they were measured under.
    void applyReadouts(FrameHeaderValues& header);
    // Keeps `stats`, the levels of `view` of `image` per block, in step with the samples placed
    // since the last frame; all of them, in blocks of `block` samples, when `fresh`. False if the
    // image could not be read.
    bool updateStats(BlockStats& stats, const SweepImage& image, protocol::ChannelView view,
                     int channels, bool fresh, std::size_t block);
    // Readouts of samples [first, end) of a sweep: their levels from `stats`, and the frequency
    // over the newest second of `view` in `live` (the capture, or what was held on freeze) ending
    // at `liveEnd`, recorded at `sampleRate`. False, with `readouts` untouched, if that audio is
    // gone.
    bool measureSweepReadouts(const BlockStats& stats, std::size_t first, std::size_t end,
                              const SampleSource& live, std::uint64_t liveEnd, double sampleRate,
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
                const Window& window, std::uint64_t sampleCount, std::size_t columns,
                std::vector<std::byte>& frame);

    Engine& engine;
    const InstanceRegistry& registry;
    AnalysisWork workDone;
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
    // Seconds mode, live: the capture end of this track and of each lane shown, now and when the
    // last frame was drawn. A frame with the same ends and the same inputs would be the same.
    std::vector<std::uint64_t> liveEnds;
    std::vector<std::uint64_t> lastLiveEnds;
    // Live, in either time base: the channel counts and sample rates of this track and of each lane
    // shown, now and when the last frame was drawn, which are inputs too (formatsChanged).
    std::vector<std::uint64_t> liveFormats;
    std::vector<std::uint64_t> lastLiveFormats;
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
    // Tempo sync: the clock and capture end when frozen, and the capture end of the last frame.
    MusicalStamp frozenStamp;
    std::uint64_t frozenEnd = 0;
    // The rate the audio held on freeze was recorded at, which places it and gives its times: the
    // host may change the track's while frozen.
    double frozenRate = 0.0;
    std::uint64_t lastEnd = 0;
    // Multi-track view: lanes by member serial, and their order on screen.
    std::map<std::uint64_t, std::unique_ptr<Lane>> lanes;
    std::vector<std::uint64_t> laneOrder;
    // The beat span the sweeps (Single mode's, or the lanes') were last drawn with, live.
    protocol::BeatSpan placedSpan = protocol::BeatSpan::kOneBar;
    std::uint64_t ownLane = 0; // Serial of this instance's lane.
    // Compare view: whether this frame compares tracks; in Seconds mode, each lane's source and
    // where this frame's window starts in it, the sum lane's columns as the window rolls, and
    // the compared lanes' visible samples, as recorded and as fixed, for the gains.
    bool comparing = false;
    bool lastPreviewFix = false;
    std::uint64_t lastRedraws = 0;
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
        int failedReads = 0;    // Tries since its audio was last read, in this view.
    };
    std::map<std::uint64_t, MatchState> matchStates;
    // The search in progress, spread over frames (kMatchSearchWork): the track it measures, and the
    // capture end when it started, which its match is dated by.
    PairSearch matchSearch;
    std::optional<std::uint64_t> searchingSerial;
    std::uint64_t searchStartedAt = 0;
    // Seconds mode, Compare: the summing gains being measured over frames (kGainReadWork), of
    // the window where it was when they began (each lane's start then), with the parts then; the
    // parts read so far, and then the gains measured so far (the sum as recorded, then fixed).
    struct GainJob
    {
        bool active = false;
        std::uint64_t at = 0; // The capture end when it began.
        std::size_t first = 0;
        std::size_t visibleCount = 0;
        std::size_t fixedFirst = 0;
        std::size_t fixedEnd = 0;
        std::vector<std::uint64_t> laneStarts;
        // Per lane, the oldest sample its parts read that its source held when they began.
        std::vector<std::uint64_t> needFrom;
        std::vector<std::uint64_t> tracks; // The tracks compared, in lane order.
        std::vector<SumPart> raw;
        std::vector<SumPart> fixed;
        std::size_t next = 0;
        int measured = 0;       // Gains measured: as recorded, then as fixed.
        bool measuring = false; // The next gain's measurement has begun.
        GainMeasure measure;
        float sumGain = 0.0f;
        double sampleRate = 0.0; // Of the audio read: positions mean nothing at another.
        std::uint64_t count = 0; // The window's length when they began, and its levels' block.
        std::size_t block = 0;
        // A match ended while it ran: the parts it read are not the newest, so the gains are
        // measured again once it ends.
        bool stale = false;
    };
    GainJob gainJob;
    // Steps the search in progress; true when it ended this frame and its match is stored.
    bool continueMatchSearch(double liveRate);
    // Steps the gains in progress by a frame's budget; true when they were published.
    bool stepGainJob();
    // The gains are due again for the live audio that arrived since they were measured.
    [[nodiscard]] bool gainsDueByTime(std::uint64_t now, std::uint64_t interval) const;
    // A track's capture has moved past audio the gains in progress need.
    [[nodiscard]] bool gainAudioReplaced() const;
    // Between frames that show something new: steps the search and gains in progress without
    // drawing. True when one ended, so that a frame shows it.
    bool stepSpreadWork(double liveRate);
    bool spreadStepped = false; // This produce stepped them already.
    bool searchEnded = false;   // ...and a search ended, which the frame drawn shows.
    // Adds `part` to `into` over positions from `position` of a window of `count` samples: each
    // shows the part's sample at position + offset, flipped if it is, and silence beyond the
    // window, read by `read`. False if the part's capture does not hold its samples yet.
    template <typename Read>
    bool addPartFrom(const Read& read, const SumPart& part, std::size_t position,
                     std::uint64_t count, std::span<float> into);
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
        std::vector<std::uint64_t> tracks; // The tracks the gains were measured for.
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
    std::uint64_t matchShownFrom = 0; // Frozen in Seconds: where the window starts, as panned.
    double matchViewStart = 0.0;      // The view zoomed or panned, as the page asked.
    double matchViewEnd = 1.0;
    bool panning = false;      // Frozen and panned this frame: its searches and gains wait a frame.
    bool matchFrozen = false;  // Frozen or live: the audio the matches were searched over.
    bool searchAtOnce = false; // Matches made due with no audio to space their searches by.
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
    // Single mode: the Mid's levels per block of the tempo-synced sweep, for readouts, and the
    // channel count they were measured for.
    BlockStats sweepStats;
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
