#pragma once

#include "engine/BlockClock.h"
#include "engine/BlockStats.h"
#include "engine/Engine.h"
#include "engine/FrameEncoder.h"
#include "engine/HoldBuffer.h"
#include "engine/InstanceRegistry.h"
#include "engine/LaneHold.h"
#include "engine/Meter.h"
#include "engine/PairAnalysis.h"
#include "engine/Reconstruction.h"
#include "engine/Reducer.h"
#include "engine/Settings.h"
#include "engine/SweepImage.h"
#include "engine/Trigger.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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
// A match search is spread over frames, at most about this many multiply-adds of it in each
// (passes over its window, and the transforms of its cross-correlation, each counted as the
// multiply-adds it takes as long as), so that a long window at a high rate (8 s at 96 kHz takes
// some 30 million) does not stall one frame: a millisecond or two's work on a fast machine, so a
// slower one still keeps its frames.
inline constexpr std::size_t kMatchSearchWork = std::size_t{1} << 23U;
// Trigger or Free's Compare gains are measured over frames too, reading or passing over at most
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
// frame in all, shared by the sums and shifted tracks being redone: a long sweep of many tracks,
// or many matches moved at once, is redone over a few frames instead of stalling one.
inline constexpr std::size_t kSumRefreshSamples = std::size_t{1} << 21U;

// At most this many lanes are drawn in the multi-track view: tracks, and in the Compare view
// their sum; and at most this many tracks, so that the sum always has a lane.
inline constexpr std::size_t kMaxLanes = protocol::kMaxLanes;
inline constexpr std::size_t kMaxShownTracks = protocol::kMaxShownTracks;

// One track of the multi-track view, as the UI labels it.
struct LaneLabel
{
    std::uint64_t id = 0; // Stable while the track exists; the UI names tracks by it.
    std::string name;
    std::optional<std::uint32_t> colour;
    bool own = false; // The instance whose editor shows the lanes.
    // Its number among the instances on its track, shown or not; 0 for the only one.
    std::uint32_t instance = 0;

    bool operator==(const LaneLabel&) const = default;
};

// The tracks behind the lanes of frames whose laneSet is `id`, in lane order.
struct LaneSet
{
    std::uint32_t id = 0;
    std::vector<LaneLabel> lanes;
};

// One instance in the process, as the editor lists it to choose the tracks shown.
struct TrackEntry
{
    LaneLabel label;
    bool shown = false; // A lane of this instance's multi-track view; always for its own.

    bool operator==(const TrackEntry&) const = default;
};

// Every instance in the process, in lane order (the host's track order, then creation); `id`
// changes whenever the list does, from 1.
struct TrackList
{
    std::uint32_t id = 0;
    std::vector<TrackEntry> tracks;
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
    // Live Beats sweeps updated to take new audio, one per sweep or lane each time: as audio
    // arrives and for each frame, not on every analysis step.
    std::uint64_t sweepUpdates = 0;
};

// The part of a window that the screen shows, as indices of a block that starts `offset` samples
// before the window's first sample (index i is window position i - offset), with one extra point
// beyond each edge so a raw trace reaches the screen edges, and `lead` and `trail` more each side
// read only as neighbours for the wave between the samples (the raw view's reach for raw frames,
// the columns' for min/max columns); the frame drops them. With an offset, those neighbours may
// lie past the window's ends, read from the audio around it, NaN where none is held.
// `startFraction` is where the visible part starts, in samples from `first`.
struct Crop
{
    std::size_t first = 0;
    std::size_t end = 0;
    double startFraction = 0.0;
    double visibleSamples = 0.0;
    std::size_t lead = 0;
    std::size_t trail = 0;
    std::size_t offset = 0;
};

// Per-editor-session analysis: turns the capture into display frames. It is
// created when the editor opens, so the hold buffer is allocated only then.
//
// In the multi-track view it reads the other instances shown from `registry`.
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
    // The other tracks the multi-track view shows, by lane id: at most kMaxShownTracks with this
    // one, which is always shown. The selection is kept with the instance (Engine::shown), so that
    // it outlives this analyzer, but is not saved; a new instance shows its own track alone.
    void setShown(const std::vector<std::uint64_t>& ids);
    // Compare view: whether the sum previews the fix.
    void setPreviewFix(bool on);
    // Draws the frame again at the next request, whatever has changed: for a page that has just
    // loaded (or loaded again after its process ended) and has no frame yet.
    void redraw();
    // The time free-running windows are paced by: monotonicNanos, or a simulated clock in tests.
    using Clock = std::function<std::uint64_t()>;
    void setClock(Clock clock) { clockNanos = std::move(clock); }

    // Writes the next frame into `frame`. Returns false when there is nothing new to show.
    bool produce(std::vector<std::byte>& frame);
    // Places the audio captured since the last frame into the tempo-synced sweeps without
    // producing a frame: for when no frame is wanted, so that nothing is missed meanwhile however
    // short the capture. Runs on the analysis worker, like produce().
    void advance();

    // The lanes of the latest multi-track frames; the id changes whenever they do. Any thread.
    [[nodiscard]] LaneSet laneSet() const;
    // Every instance in the process as of the latest multi-track frame, and which are shown; none
    // (id 0) before the first. Any thread.
    [[nodiscard]] TrackList trackList() const;

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
        // Trigger: the sample time zero counts from (the trigger point, or where an untriggered
        // window puts it), and the start before any pan, which the frame's trigger position is
        // measured from. NaN in Free and Beats.
        double zero = std::numeric_limits<double>::quiet_NaN();
        double placedStart = 0.0;
        bool triggered = false;
        // Free-running: the window ends at the newest audio (the Free time base).
        bool freeRunning = false;
        // Free-running and live: the right edge of the whole span, which a freeze anchors on.
        double edge = 0.0;
        // Free-running: the window is the visible part alone. In columns, zoomed or not, on a
        // grid of its own (the grid of the part reduced and its first column), not cropped; in
        // raw samples, with a sample beyond each edge, cropped to the view.
        bool visiblePart = false;
        bool onGrid = false;
        ColumnGrid grid{};
        std::uint64_t firstColumn = 0;
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
        // Frozen in Trigger or Free: how far back the view can be panned (seconds), to the start of
        // the held audio.
        double panReach = 0.0;
    };

    // A sum's recomputation spread over frames: `remaining` positions from `cursor`, wrapping.
    struct SumRefresh
    {
        std::size_t cursor = 0;
        std::size_t remaining = 0;
    };

    // What a structure kept from frame to frame was built from, and the lanes frame it last
    // followed. A Beats sum (its levels per block, its columns) is brought up to date only where
    // each frame's audio changed it, so it holds what it is asked for only if it was built from
    // the same parts and has followed every lanes frame since: one that a frame left out (Compare
    // off meanwhile) missed the audio placed then, and is redone. The one test for both.
    struct Kept
    {
        std::vector<std::int64_t> key;
        std::uint64_t frame = 0; // 0: never built.

        // Whether it holds `next` as of the frame before `now`.
        [[nodiscard]] bool holds(const std::vector<std::int64_t>& next, std::uint64_t now) const
        {
            return frame != 0 && frame + 1 >= now && key == next;
        }
        // The same, noting that it follows `next` from frame `now` on: the caller redoes it when
        // it did not hold.
        bool follows(const std::vector<std::int64_t>& next, std::uint64_t now)
        {
            const bool held = holds(next, now);
            key = next;
            frame = now;
            return held;
        }
    };

    // One track of the multi-track view: its own persistent sweep, and its audio while frozen.
    struct Lane
    {
        // Another track's image holds its lane channel alone; this track's, its captured channels
        // (both of a stereo track), which the Meter reads Left and Right from.
        explicit Lane(int imageChannels = 1) : image(imageChannels) {}
        std::shared_ptr<Engine> engine;
        SweepImage image;
        std::optional<SweepWindow> window;
        std::uint64_t end = 0; // Where the last update placed the sweep to.
        // Beats, live: this lane's capture index of the audio played with this track's index i
        // is i + indexOffset (the captures started at different moments), and how many
        // placements in a row a measurement disagreed with it.
        std::optional<std::int64_t> indexOffset;
        int offsetDisagreements = 0;
        LaneHold hold;
        // Shown while the multi-track view was frozen: none of its audio was held at the freeze,
        // so its lane is empty until the display resumes (FR-6).
        bool heldNothing = false;
        // The lane's screen columns, and the part of the sweep they reduce.
        ColumnReduction reduction;
        std::size_t reducedFirst = 0;
        // Trigger or Free, rolling: the lane's columns, kept as they scroll.
        RollColumns roll;
        // The lane's levels per block of its sweep, for summing gains.
        BlockStats stats;
        // This track's lane: the Meter's statistics per block of its sweep.
        MeterBlocks meter;
        // Tempo-synced Compare, while the fix shifts this track: its levels per block as shifted,
        // for the fixed summing gain, the shift they were measured for and their pending
        // recomputation (see updateSumStats).
        BlockStats fixedStats;
        SumRefresh fixedRefresh;
        std::vector<std::int64_t> fixedKey;
        Kept fixedKept;
        // What the lane's columns were last drawn from: nothing for its audio as recorded, or the
        // alignment it was read through (feedKey). Columns drawn from another feed are drawn
        // again: a sweep's over the next frames (feedRefresh), rolled ones afresh.
        std::vector<std::int64_t> reducedFeed;
        SumRefresh feedRefresh;
        std::vector<std::int64_t> rolledFeed;
        // Tempo-synced, lined up between samples: the lane's audio as its alignment's filter
        // reads it (before its gain), over the positions this frame's new audio reaches. Its own
        // columns, its levels, and the difference's levels and columns each read that stretch
        // every frame: filtered once here, in the lanes frame `frame` (0: none).
        struct Lined
        {
            std::uint64_t frame = 0;
            std::ptrdiff_t offset = 0;
            float fraction = 0.0f;
            std::size_t length = 0; // The sweep's.
            std::size_t first = 0;
            std::vector<float> values;
        };
        Lined lined;
    };
    struct SumPart;
    // Screen or window positions [first, end).
    struct Positions
    {
        std::size_t first = 0;
        std::size_t end = 0;
    };
    // How far a change of a track's audio reaches in what is drawn from it shifted: positions
    // before it and after it.
    struct Reach
    {
        std::size_t before = 0;
        std::size_t after = 0;
    };
    // What a change of `parts`' audio reaches (Reach).
    static Reach reachOf(const std::vector<SumPart>& parts);

    Commands takeCommands();
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
    // The view the trigger reads: in Single the trigger source when drawn, else the first view
    // drawn; this track's lane channel in Multi. The one place the trigger search and the
    // frames that name it decide it.
    [[nodiscard]] protocol::ChannelView triggerViewFor(const Settings& settings, bool multi) const;
    // The channel every lane shows in Multi: the one `settings` choose. Frozen, another track's
    // lane held in another channel is drawn empty.
    [[nodiscard]] protocol::ChannelView laneChannel(const Settings& settings) const;
    // What this track's lane reads, which the frame names: its lane channel, or L on a mono
    // track, whose one channel every view reads.
    [[nodiscard]] protocol::ChannelView ownLaneView(const Settings& settings) const;
    // The view `lane`'s image holds of `channel`: L on a mono track, whose one channel every view
    // reads, so that its image is kept when the channel changes.
    [[nodiscard]] static protocol::ChannelView laneView(const Lane& lane,
                                                        protocol::ChannelView channel);
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
    // Trigger or Free: the Compare frame of `window` (`sampleCount` samples from `start` of this
    // track, shown over `crop`, at `factor` points per sample, `rolling` on fixed column
    // boundaries), from the lanes' sources in `laneSources`, whose audio is at `sampleRate`.
    void renderCompare(FrameHeaderValues header, const Window& window, std::uint64_t start,
                       std::uint64_t sampleCount, const Crop& crop, std::size_t factor,
                       bool rolling, double sampleRate, const Commands& command,
                       std::vector<std::byte>& frame);
    // The sum lane, drawn over `crop`, and the matches and gains, measured over its visible part.
    // `playhead` is the sweep's playhead as a screen position, where the newest audio is; the
    // lanes' audio is at `sampleRate`.
    void appendComparison(FrameHeaderValues& header, const Crop& crop, std::uint64_t count,
                          std::size_t columns, bool previewFix, std::size_t playhead,
                          double sampleRate);
    // The compared track whose match to search this frame, as a lane index, if any.
    std::optional<std::size_t> matchToSearch(std::uint32_t laneSet, std::size_t first,
                                             std::size_t end, std::size_t length, std::uint64_t now,
                                             std::uint64_t shownFrom, double viewStart,
                                             double viewEnd);
    void publishLanes(std::vector<LaneLabel> labels, std::vector<TrackEntry> tracks);
    // What the Meter's readouts were measured over: the zoom, which changes the visible part
    // without new audio (a pan does so only while frozen, when they are always measured).
    struct ReadoutView
    {
        double viewStart = 0.0;
        double viewEnd = 1.0;
        bool operator==(const ReadoutView&) const = default;
    };
    [[nodiscard]] bool readoutsDue(std::uint64_t end, protocol::ChannelView view,
                                   const ReadoutView& shown) const;
    // Keeps `values`, measured on `view` up to the capture end `end`, for this and later frames.
    // Nothing where a read failed: the readouts stay as they were, and the next frame measures
    // again.
    void storeReadouts(const std::optional<Readouts>& values, std::uint64_t end,
                       protocol::ChannelView view, const ReadoutView& shown);
    // The Meter's readouts over samples [first, end) of `source` (a window's visible part, of this
    // track's capture or its hold, by capture index): the readout channel `view`, and Left and
    // Right, of `channels` captured channels, at `sampleRate`. With the Meter on, from statistics
    // per block that follow the audio; off, the levels alone, from the samples. Nothing if the
    // source does not hold the samples or a read fails.
    [[nodiscard]] std::optional<Readouts> measureWindow(const SampleSource& source, int channels,
                                                        protocol::ChannelView view,
                                                        std::uint64_t first, std::uint64_t end,
                                                        double sampleRate);
    [[nodiscard]] bool metered() const;
    // Reads `view` of `source` for a span's levels per block.
    BlockStats::Reader statsReader(const SampleSource& source, int channels,
                                   protocol::ChannelView view);
    [[nodiscard]] static std::optional<Readouts>
    levelsOf(BlockStats& stats, std::size_t first, std::size_t end, const BlockStats::Reader& read);
    // Keeps `meter`, the Meter's statistics of `image` per block, in step with the samples placed
    // since the last frame; all of them when `fresh`. False if the image could not be read.
    bool updateMeter(MeterBlocks& meter, const SweepImage& image, protocol::ChannelView view,
                     int channels, bool fresh, std::size_t block, double sampleRate);
    // The Meter's reader of `image` (or a window's `source` from `origin`), as MeterBlocks reads.
    MeterBlocks::Reader meterReader(const SampleSource& source, int channels,
                                    protocol::ChannelView view, std::int64_t origin);
    // Puts the latest readouts, and the view they measured, in `header`, and notes what they were
    // measured under.
    void applyReadouts(FrameHeaderValues& header);
    // Keeps `stats`, the levels of `view` of `image` per block, in step with the samples placed
    // since the last frame; all of them, in blocks of `block` samples, when `fresh`. False if the
    // image could not be read.
    bool updateStats(BlockStats& stats, const SweepImage& image, protocol::ChannelView view,
                     int channels, bool fresh, std::size_t block);
    // Tempo-synced Compare frames, over screen positions of the sweep of `length` samples: lane
    // `index`'s lane channel from `from` (silent while the lane is not on that length), and the sum
    // of `parts` from `position`.
    void readLaneImage(std::size_t index, std::size_t length, std::uint64_t from,
                       std::span<float> into);
    void readSum(const std::vector<SumPart>& parts, std::size_t length, std::size_t position,
                 std::span<float> into);
    // Keeps `stats`, the levels of the sum of `parts` per block, in step: where compared tracks
    // changed this frame, widened by `reach` (positions before and after), and when they do not
    // hold `key` as of the frame before (`kept`: other parts, or a frame that left them out), all
    // of them again over the next frames, as one of `shares` recomputations under way (see
    // kSumRefreshSamples); all at once for a new length. True if levels were measured again beyond
    // this frame's changes.
    bool updateSumStats(BlockStats& stats, SumRefresh& refresh,
                        const std::vector<std::int64_t>& key, Kept& kept,
                        const std::vector<SumPart>& parts, Reach reach, std::size_t length,
                        std::size_t block, std::size_t shares);
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
    // Ends a freeze Auto-freeze made, as playing again does.
    void unfreezeAfterStop(Commands& command);
    // Waiting to resume (resumeAt): moves the paced edge on and unfreezes once it gets there.
    void resume(const Settings& settings, Commands& command);
    bool freeze(double sampleRate, std::uint64_t minimumSamples = 0,
                std::optional<std::uint64_t> holdEnd = std::nullopt);
    // Replaces each of the first `blocks` blocks of a raw frame's points, read with `crop`'s
    // neighbours, by the wave through its samples at `factor` points per sample, with no curve
    // across any of `seams(block)`, the joins in that block (a view's, a lane's, the sum's), as
    // indices into it. Data after the blocks is kept.
    void reconstruct(FrameHeaderValues& header, std::size_t blocks, const Crop& crop,
                     std::size_t factor,
                     const std::function<std::span<const std::size_t>(std::size_t)>& seams);
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
    // A Beats frame saying the host has not reported a tempo was the last sent: whether it was
    // frozen.
    std::optional<bool> sentNoTempo;
    std::optional<Window> lastLive;
    std::optional<Window> lastShown;
    // Trigger or Free, live: the capture end of this track and of each lane shown, now and when the
    // last frame was drawn. A frame with the same ends and the same inputs would be the same.
    std::vector<std::uint64_t> liveEnds;
    std::vector<std::uint64_t> lastLiveEnds;
    // This track's paced edge: free-running windows and Auto's untriggered fallback end there.
    PacedEdge pacedEdge;
    Clock clockNanos = monotonicNanos;
    // Where a live sweep is placed to: this track's paced edge, and its stamp to place by.
    [[nodiscard]] std::uint64_t ownSweepEdge(protocol::BeatSpan span);
    [[nodiscard]] std::uint64_t laneSweepEdge(Lane& lane, const Lane& own);
    // The position to place a sweep's audio up to `to` by. Its audio from `from` (where it was
    // placed last) on is placed first part by part, by `placeBefore` up to each recorded change,
    // by the position it was played at (Timeline::positionsSince); `fallback` before any record.
    MusicalStamp
    placeThroughChanges(const Timeline& timeline, std::uint64_t from, std::uint64_t to,
                        const MusicalStamp& fallback,
                        const std::function<void(const MusicalStamp&, std::uint64_t)>& placeBefore);
    std::array<MusicalStamp, Timeline::kPositionChanges> positions; // placeThroughChanges' copy.
    // The position to place a hold ending at `end` by: the one in effect there when a change was
    // recorded after it (a jump still ahead of the playhead), `fallback` (the newest) otherwise.
    MusicalStamp clockAt(const Timeline& timeline, std::uint64_t end, const MusicalStamp& fallback);
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
    // Auto-freeze in Beats, playing again: the capture index the paced playhead must reach before
    // the view goes live (the newest audio when play was seen), so no frame shows audio from the
    // stop placed on the clock that ran on meanwhile.
    std::optional<std::uint64_t> resumeAt;
    // Tempo sync: the clock and capture end when frozen, and the capture end of the last frame.
    MusicalStamp frozenStamp;
    std::uint64_t frozenEnd = 0;
    // The rate the audio held on freeze was recorded at, which places it and gives its times: the
    // host may change the track's while frozen.
    double frozenRate = 0.0;
    // The lane channel the lanes' rolled columns, and the sum's, were reduced from.
    protocol::ChannelView rolledLaneChannel = protocol::ChannelView::kMid;
    std::uint64_t lastEnd = 0;
    // Multi-track view: lanes by member serial, and their order on screen.
    std::map<std::uint64_t, std::unique_ptr<Lane>> lanes;
    std::vector<std::uint64_t> laneOrder;
    // The beat span the sweeps (Single mode's, or the lanes') were last drawn with, live.
    protocol::BeatSpan placedSpan = protocol::BeatSpan::kOneBar;
    std::uint64_t ownLane = 0; // Serial of this instance's lane.
    // New audio placed in the lanes between frames (advance), which the next frame draws.
    bool lanesPlaced = false;
    // The captured audio, summed over the tracks placed, when advance last placed the sweeps.
    std::uint64_t advancedCaptured = 0;
    // Compare view: whether this frame compares tracks; in Trigger or Free, each lane's source and
    // where this frame's window starts in it, the sum lane's columns as the window rolls, and
    // the compared lanes' visible samples, as recorded and as fixed, for the gains.
    bool comparing = false;
    bool lastPreviewFix = false;
    std::uint64_t lastRedraws = 0;
    // Where a lane's part of the window is: its source, where the part starts there, and how many
    // channels to read it with (one for audio held as the lane channel alone).
    struct LaneSource
    {
        const SampleSource* source = nullptr;
        std::uint64_t start = 0;
        int channels = 1;
        // Frozen, held at another rate or in another channel: it has none of this window's audio,
        // at any position.
        bool none = false;
    };
    std::vector<LaneSource> laneSources;
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
        // Searched to align the tracks (the difference's kind, over Align's reach), not for a
        // fix: which of the analysis's fields it filled.
        bool aligned = false;
        std::uint64_t analysedAt = 0;
        std::uint64_t wait = 0; // Audio to pass after analysedAt before the next search.
        int failedReads = 0;    // Tries since its audio was last read, in this view.
    };
    std::map<std::uint64_t, MatchState> matchStates;
    // The search in progress, spread over frames (kMatchSearchWork): the track it measures, and the
    // capture end when it started, which its match is dated by.
    PairSearch matchSearch;
    bool searchAligning = false; // The kind of the search in progress.
    bool matchAligning = false;  // The kind the matches on show were made due for.
    std::optional<std::uint64_t> searchingSerial;
    std::uint64_t searchStartedAt = 0;
    // Trigger or Free, Compare: the summing gains being measured over frames (kGainReadWork), of
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
        // The positions of the visible part where every aligned part read so far had its audio:
        // the difference is measured there alone.
        Positions held;
        // The last lane is the difference: `raw` holds its parts, whose sum is measured against
        // part `reference` (this track) for the null depth, and nothing is measured as fixed.
        bool difference = false;
        std::size_t reference = 0;
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
    void adoptAlignment(std::uint64_t serial, const PairAnalysis& match);
    // The compared tracks as parts of the sum, in rawParts and fixedParts: as recorded, and
    // shifted and flipped as matched; and as parts of the difference. Both time bases build them
    // here, so they agree.
    void buildParts();
    // Whether the frame being produced shows the difference in Compare's last lane.
    [[nodiscard]] bool differenceShown() const;
    // The last lane's parts: the difference's, or the sum's as recorded or as the fix previews.
    [[nodiscard]] const std::vector<SumPart>& shownParts(bool previewFix) const;
    // Which of `parts` is this track.
    [[nodiscard]] std::size_t ownPart(const std::vector<SumPart>& parts) const;
    // Starts the gains afresh when the last lane is not the one they were measured for.
    void followLane();
    // The matches' values, one set per lane, into comparisonCache.matches.
    void packMatches();
    // The earliest (at most 0) and latest (at least 0) sample `parts` read, from the position
    // each is shown at: their shifts, and around an aligned part's the reach of its filter.
    static std::pair<std::ptrdiff_t, std::ptrdiff_t> fixedReach(const std::vector<SumPart>& parts);
    // The first and last sample `part` reads, from the position it is shown at.
    static std::pair<std::ptrdiff_t, std::ptrdiff_t> readsOf(const SumPart& part);
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
        // What the track is multiplied by: 1 as recorded, -1 flipped by a fix or by Align, and
        // -1 for the other tracks of a difference, which is the sum of this track and the
        // others negated (so 1 again for one Align flips).
        float gain = 1.0f;
        // Lined up by Align: shifted a fraction of a sample further than `offset` (0 for none),
        // through the filter `taps` (none for a whole shift), and read beyond the window shown
        // as far as its audio is held, since an effect's latency can exceed a short window.
        float fraction = 0.0f;
        std::shared_ptr<const std::vector<float>> taps = nullptr;
        bool aligned = false;
    };
    // How Align lines a compared track up with this one: the shift in whole samples and the
    // fraction beyond, the filter that reads the wave there, and whether it is flipped. Its
    // level is left as it is: a change of level is part of what an effect did, and belongs in
    // the difference.
    struct Alignment
    {
        std::ptrdiff_t offset = 0;
        float fraction = 0.0f;
        bool flipped = false;
        std::shared_ptr<const std::vector<float>> taps = nullptr;
    };
    // The alignment in use of each compared track, by serial: the last one a search found, kept
    // until one finds another (a steady routing redoes nothing), and dropped with the matches.
    std::map<std::uint64_t, Alignment> alignments;
    // The part a compared track is in the difference, or in its own lane when Align lines it up:
    // as recorded, or as its alignment has it while Align is on.
    [[nodiscard]] SumPart alignedPart(std::size_t lane, float sign) const;
    // Samples either side of a position that an aligned part's filter reads.
    static constexpr std::size_t kAlignReach = kReconstructionReach;
    Reconstruction alignFilter{kAlignReach};
    // What lane `lane`'s own columns are drawn from in the frame being produced: its audio as
    // recorded, or, compared in the difference with Align on, as its alignment has it.
    [[nodiscard]] SumPart laneFeed(std::size_t lane) const;
    // A feed as a key (Lane::reducedFeed): empty for audio as recorded.
    static void feedKey(const SumPart& feed, std::vector<std::int64_t>& key);
    std::vector<std::int64_t> feedScratch;
    // Tempo-synced: empties `into`, read from `position` of a sweep of `length`, where the
    // aligned `part` has no audio of the moments shown there: where it would read past the
    // sweep's edges, or across the playhead, beyond which the sweep holds the previous pass.
    void blankUnheld(const SumPart& part, std::size_t length, std::size_t position,
                     std::span<float> into) const;
    // The sweep's playhead as a screen position, in the Beats frame being produced.
    std::size_t sweepPlayhead = 0;
    // Where this track's sweep changed in that frame, and where a lane's columns are drawn
    // again for it.
    std::vector<SweepImage::Range> ownChanges;
    std::vector<SweepImage::Range> feedChanges;
    // Trigger or Free: adds the aligned `part` to `into` over window positions from `position`
    // (which may lie before the window, as a column's neighbours do), read from `lane`, whose
    // part of the window starts at `laneStart`, as far beyond the window as it holds audio.
    // Returns the positions of `into` where it holds every sample read; nothing is added
    // elsewhere.
    Positions addAlignedPart(const LaneSource& lane, std::uint64_t laneStart, const SumPart& part,
                             std::int64_t position, std::span<float> into);
    std::vector<float> alignScratch;
    // Fills lane `lane`'s Lined for this frame: where its sweep or this track's changed, widened
    // by `reach` (what such a change reaches in anything lined up) and `slack` (the whole blocks
    // and columns around it that are read with it). None for a stretch longer than kLinedSamples
    // (a frame after a long pause, or a sweep that wrapped), which is then filtered as it is read.
    void lineUpChanges(std::size_t lane, std::size_t length, Reach reach, std::size_t slack);
    static constexpr std::size_t kLinedSamples = std::size_t{1} << 17U;
    // Recomputations of aligned lanes under way in the Beats frame being produced, and of the
    // sums in the one before: they share a frame's kSumRefreshSamples.
    std::size_t feedShares = 0;
    std::size_t sumShares = 0;
    struct ComparisonCache
    {
        std::vector<float> matches;
        float sumGain = 0.0f;
        float fixedGain = 0.0f;
        // The difference's null depth, and which the last lane was when all three were measured:
        // the sum has the gains and no depth, the difference the depth and no gains.
        float nullDepth = std::numeric_limits<float>::quiet_NaN();
        protocol::CompareLane lane = protocol::CompareLane::kSum;
        bool aligned = false;              // Whether Align lined the difference's tracks up then.
        std::vector<std::uint64_t> tracks; // The tracks the gains were measured for.
        std::uint64_t at = 0;
        bool valid = false;
    };
    std::vector<SweepImage::Range> frameChanges;
    ColumnReduction sumReduction;
    std::vector<SumPart> rawParts;   // The compared tracks as recorded...
    std::vector<SumPart> fixedParts; // ...and shifted and flipped as matched.
    // The difference: this track, and the other compared tracks negated.
    std::vector<SumPart> differenceParts;
    std::vector<std::int64_t> sumKey;
    Kept sumKept;
    // Lanes frames drawn in Beats, from 1: what the kept sums follow (Kept).
    std::uint64_t lanesFrame = 0;
    // Tempo-synced: the sum lane's span, visible part and columns, and its pending recomputation.
    std::vector<std::int64_t> sumGeometry;
    std::vector<std::int64_t> lastSumGeometry;
    SumRefresh sumRefresh;
    std::vector<float> sumScratch;
    // A compared lane's or a sum part's neighbours past a window's ends, and their read.
    std::vector<float> marginSamples;
    std::vector<float> marginScratch;
    // The sums' levels per block, and the parts they were measured for.
    BlockStats rawSumStats;
    BlockStats fixedSumStats;
    std::vector<std::int64_t> rawSumKey;
    Kept rawSumKept;
    std::vector<std::int64_t> fixedSumKey;
    Kept fixedSumKept;
    SumRefresh rawSumRefresh;
    SumRefresh fixedSumRefresh;
    // The difference's levels per block, kept only while it is the lane on show (as the sums'
    // are only while the sum is), for its null depth.
    BlockStats differenceStats;
    std::vector<std::int64_t> differenceKey;
    Kept differenceKept;
    SumRefresh differenceRefresh;
    // Whether a sum is still being recomputed: frames go on meanwhile, also while frozen.
    // A track's shifted levels (Lane::fixedRefresh) need no term of their own: a shift that changes
    // them changes the fixed sum's parts too, and the sum, of at least two parts, is redone at
    // most half as fast, so it is still refreshing until they are done.
    // The same for a lane drawn from another alignment than the one now in use: a search that
    // ended in a frame, after its lanes were drawn, is shown by the next.
    [[nodiscard]] bool sumsRefreshing() const;
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
    std::uint64_t matchShownFrom =
        0;                       // Frozen in Trigger or Free: where the window starts, as panned.
    double matchViewStart = 0.0; // The view zoomed or panned, as the page asked.
    double matchViewEnd = 1.0;
    bool panning = false;     // Frozen and panned this frame: its searches and gains wait a frame.
    bool matchFrozen = false; // Frozen or live: the audio the matches were searched over.
    protocol::ChannelView matchLaneChannel = protocol::ChannelView::kMid; // The channel matched.
    bool searchAtOnce = false; // Matches made due with no audio to space their searches by.
    std::uint32_t lastLaneSet = 0;
    mutable std::mutex laneMutex;
    LaneSet published;
    TrackList publishedTracks;
    // The last readouts of the multi-track view and the capture end they were measured at.
    struct ReadoutCache
    {
        Readouts values;
        std::uint64_t end = 0;
        protocol::ChannelView view = protocol::ChannelView::kMid;
        bool valid = false;
        ReadoutView shown;
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
    // Single mode: the levels per block of the tempo-synced sweep, and the channel count and
    // view they were measured for; with the Meter on, its statistics per block too.
    BlockStats sweepStats;
    int sweepStatsChannels = 0;
    protocol::ChannelView sweepStatsView = protocol::ChannelView::kMid;
    MeterBlocks sweepMeter;
    // Trigger and Free, with the Meter on: its statistics per block of this track's audio, which
    // follow it (position 0 is capture index `streamOrigin`), and the readout view and channel
    // count they were measured for; and those of a trace held from before what they keep,
    // measured afresh on the readout schedule.
    MeterBlocks streamMeter;
    std::uint64_t streamOrigin = 0;
    protocol::ChannelView streamView = protocol::ChannelView::kMid;
    int streamChannels = 0;
    MeterBlocks spotMeter;
    std::vector<float> meterView;
    std::vector<float> meterLeft;
    std::vector<float> meterRight;
    std::vector<std::size_t> meterJoins;
    // Reading a lane's image of two channels as its lane channel, which may be Mid or Side.
    std::vector<float> laneScratch;
    Settings frameSettings; // The settings of the frame being produced.
    std::vector<float> readScratch;
    std::vector<float> triggerSamples;
    std::vector<float> viewSamples;
    std::vector<float> scratch;
    std::vector<float> reduced;
    std::vector<float> frameData;
    // Raw frames: the wave between the samples.
    Reconstruction reconstruction;
    // Min/max columns of windows reduced afresh: the wave around their extreme samples.
    ColumnPeaks columnPeaks;
    // The joins in a sweep's columns: the views', a lane's, and the Compare sum's (the playhead's
    // and every lane's).
    std::vector<std::size_t> viewSeams;
    std::vector<std::size_t> laneSeams;
    std::vector<std::vector<std::size_t>> lanesSeams; // Each Multi lane's, in lane order.
    std::vector<std::size_t> sumSeams;
    std::vector<float> upsampled;
    std::vector<float> reconstructed;
};

} // namespace orizon::engine
