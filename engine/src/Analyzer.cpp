#include "engine/Analyzer.h"

#include "engine/ChannelViews.h"
#include "engine/FrameEncoder.h"
#include "engine/FreeRunning.h"
#include "engine/Readouts.h"
#include "engine/Reducer.h"
#include "engine/Sweep.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace orizon::engine {

using protocol::ChannelView;
using protocol::FrameFlag;
using protocol::SettingId;

namespace {

// In Seconds mode a freeze holds the longest span, which the view can be panned back through. The
// lanes of other tracks hold up to this much more: their captures can run a little ahead of or
// behind this one's, and each must still cover the whole span with the view panned as far back as
// this track's hold allows. What the capture keeps cuts it short above 174.8 kHz with the smallest
// capture, and above 349.5 kHz with the largest: to 0.53 s at 192 and 384 kHz (see
// CaptureRing::capacityFor).
constexpr double kLaneMarginSeconds = 1.0;

// Samples a freeze holds. In Beats mode, the newest audio the capture can spare (a margin for the
// audio thread, which writes on), so that a longer beat span chosen while frozen has its audio;
// in Seconds mode, the longest span, and for another track's lane the margin above.
std::uint64_t frozenLength(const CaptureRing& ring, double sampleRate, bool synced,
                           bool lane = false)
{
    const std::uint64_t readable = CaptureRing::readableLength(ring.capacity());
    if (synced) {
        return readable;
    }
    // Never more than the capture can spare while the audio thread writes on. That always covers
    // the hold up to the highest sample rate (see CaptureRing::capacityFor); a lane's margin may
    // be cut short.
    const double seconds = HoldBuffer::kHoldSeconds + (lane ? kLaneMarginSeconds : 0.0);
    return std::min<std::uint64_t>(static_cast<std::uint64_t>(std::llround(seconds * sampleRate)),
                                   readable);
}

// The narrowest view accepted: as far as the UI zooms. The UI computes its views in floating
// point, so one at the limit can come out a few ulps narrower; those are accepted too.
constexpr double kMinimumViewWidth = protocol::kMinimumViewWidth * (1.0 - 1e-9);

constexpr std::uint8_t flag(FrameFlag value)
{
    return static_cast<std::uint8_t>(value);
}

// The part of a window of `count` samples that the screen shows, as sample indices from its first
// sample, with one extra point beyond each edge so a raw trace reaches the screen edges. The
// window starts `fraction` of a sample after its first sample. A window of no samples (a failed
// read, or a free-running span before its first column of audio) shows none.
struct Crop
{
    std::size_t first = 0;
    std::size_t end = 0;
    double startFraction = 0.0;
    double visibleSamples = 0.0;
};

Crop cropOf(double count, double fraction, double cropStart, double cropEnd)
{
    if (count < 1.0) {
        return {};
    }
    const double visibleFrom = fraction + cropStart * count;
    const double visibleTo = fraction + cropEnd * count;
    const auto first = static_cast<std::size_t>(
        std::clamp(std::floor(visibleFrom), 0.0, std::max(count - 1.0, 0.0)));
    const auto end = static_cast<std::size_t>(
        std::clamp(std::ceil(visibleTo) + 1.0, static_cast<double>(first) + 1.0, count));
    return {.first = first,
            .end = end,
            .startFraction = visibleFrom - static_cast<double>(first),
            .visibleSamples = (cropEnd - cropStart) * count};
}

bool contains(const std::vector<std::uint64_t>& ids, std::uint64_t id)
{
    return std::ranges::find(ids, id) != ids.end();
}

TriggerParameters triggerParameters(const Settings& settings, double sampleRate)
{
    return {.level = settings.value(SettingId::kTriggerLevel),
            .hysteresis = settings.value(SettingId::kTriggerHysteresis),
            .slope = settings.enumValue<protocol::TriggerSlope>(SettingId::kTriggerSlope),
            .holdOffSamples = settings.value(SettingId::kTriggerHoldOff) * sampleRate};
}

// Samples per block of levels: kStatsBlockSeconds of audio.
std::size_t statsBlock(double sampleRate)
{
    return std::max<std::size_t>(
        1, static_cast<std::size_t>(std::lround(kStatsBlockSeconds * sampleRate)));
}

// Peak and RMS over samples [first, end) of a span, from its levels per block: those of the
// blocks holding them.
Readouts levelsOf(const BlockStats& stats, std::size_t first, std::size_t end)
{
    Readouts readouts;
    const std::size_t block = stats.blockSamples();
    const std::span<const BlockStats::Block> blocks = stats.blocks();
    const std::size_t firstBlock = std::min(first / block, blocks.size());
    const std::size_t endBlock = std::min((end + block - 1) / block, blocks.size());
    float peak = 0.0f;
    double sumSquares = 0.0;
    std::size_t invalid = 0;
    for (std::size_t index = firstBlock; index < endBlock; ++index) {
        peak = std::max(peak, blocks[index].peak);
        sumSquares += blocks[index].sumSquares;
        invalid += blocks[index].invalid;
    }
    readouts.invalid = static_cast<std::uint32_t>(invalid);
    const std::size_t samples = std::min(endBlock * block, stats.sampleCount()) -
                                std::min(firstBlock * block, stats.sampleCount()) - invalid;
    if (samples > 0) {
        readouts.peak = peak;
        readouts.rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(samples)));
    }
    return readouts;
}

// Goes on with a sum's pending recomputation (an Analyzer::SumRefresh): up to `step` positions of
// a span of `length`, from where it stopped, wrapping at the end.
template <typename Refresh, typename Update>
void continueRefresh(Refresh& refresh, std::size_t length, std::size_t step, const Update& update)
{
    if (length == 0) {
        refresh = {};
        return;
    }
    std::size_t budget = std::min(std::max<std::size_t>(step, 1), refresh.remaining);
    refresh.remaining -= budget;
    while (budget > 0) {
        refresh.cursor %= length;
        const std::size_t run = std::min(budget, length - refresh.cursor);
        update(refresh.cursor, refresh.cursor + run);
        refresh.cursor += run;
        budget -= run;
    }
}

// Merges `ranges` that overlap or touch, in place, in position order: the positions where several
// tracks' sweeps changed (all of them alike, as tracks play together) are then summed once.
void mergeRanges(std::vector<SweepImage::Range>& ranges)
{
    std::ranges::sort(ranges, {}, &SweepImage::Range::first);
    std::size_t kept = 0;
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        if (kept > 0 && ranges[index].first <= ranges[kept - 1].end) {
            ranges[kept - 1].end = std::max(ranges[kept - 1].end, ranges[index].end);
        } else {
            ranges[kept++] = ranges[index];
        }
    }
    ranges.resize(kept);
}

// Lane `index`'s bit in a frame's `invalidLanes`: the lanes, then the sum after them.
static_assert(kMaxLanes < 32, "every lane and the sum have a bit");
std::uint32_t laneBit(std::size_t index)
{
    return std::uint32_t{1} << index;
}

// Whether any of `samples` is not finite (NaN or infinite, from a broken signal upstream).
bool holdsInvalid(std::span<const float> samples)
{
    return !std::ranges::all_of(samples, [](float sample) { return std::isfinite(sample); });
}

// Whether the blocks of `stats` holding samples [first, end) have any that is not finite.
bool holdsInvalid(const BlockStats& stats, std::size_t first, std::size_t end)
{
    const std::size_t block = stats.blockSamples();
    const std::span<const BlockStats::Block> blocks = stats.blocks();
    const std::size_t firstBlock = std::min(first / block, blocks.size());
    const std::size_t endBlock = std::min((end + block - 1) / block, blocks.size());
    return std::any_of(blocks.begin() + static_cast<std::ptrdiff_t>(firstBlock),
                       blocks.begin() + static_cast<std::ptrdiff_t>(endBlock),
                       [](const BlockStats::Block& level) { return level.invalid > 0; });
}

// `lanes` matches that are none: no correlation or offset (NaN), and not related (polarity 0).
void assignNoMatches(std::vector<float>& values, std::size_t lanes)
{
    values.assign(lanes * protocol::kMatchValues, std::numeric_limits<float>::quiet_NaN());
    for (std::size_t lane = 0; lane < lanes; ++lane) {
        values[(lane * protocol::kMatchValues) + 2] = 0.0f;
    }
}

// How many positions of a sum of `parts` tracks to recompute per frame, as one of `shares`
// recomputations under way together, which share the frame's kSumRefreshSamples.
std::size_t refreshStep(std::size_t parts, std::size_t shares)
{
    return kSumRefreshSamples / std::max<std::size_t>(shares, 1) / std::max<std::size_t>(parts, 1);
}

// Where the fixed sum is measured, of positions [first, end) of a span of `count`: where every
// compared track, shifted by its fix (offsets from `earliest`, at most 0, to `latest`, at least 0),
// has samples of its own. At the span's edges a shift reaches past them (into audio not captured
// yet, at the right edge live), and a track summed with that silence would read as a weaker fix
// than it is. Empty (first == end) when no position has them all.
std::pair<std::size_t, std::size_t> fixedPart(std::size_t first, std::size_t end, std::size_t count,
                                              std::ptrdiff_t earliest, std::ptrdiff_t latest)
{
    const auto from = std::max<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(first), -earliest);
    const auto to =
        std::max(from, std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(end),
                                                static_cast<std::ptrdiff_t>(count) - latest));
    return {static_cast<std::size_t>(from), static_cast<std::size_t>(to)};
}

// Frozen, positions [first, last) of `image` cropped for a shorter span: when the image is still
// filling in (rebuilt for a span longer than the hold, or begun partway through a cycle), that
// span rebuilt from the audio `held` up to `heldEnd`, which may cover more of it, is shown there
// instead, beside the image, which stays as it is for when the crop ends; otherwise nothing is.
// True if what is shown changed.
bool showHeldPart(SweepImage& image, const SampleSource& held, const MusicalStamp& clock,
                  std::uint64_t heldEnd, double sampleRate, protocol::BeatSpan span,
                  int inputChannels, std::uint64_t first, std::uint64_t last)
{
    const bool cropped = last > first && (first > 0 || last < image.length());
    if (!cropped || image.complete()) {
        const bool had = image.hasPart();
        image.clearPart();
        return had;
    }
    if (image.showsPart(first, last - first)) {
        return false;
    }
    const auto window = sweepWindow(clock, heldEnd, sampleRate, span, kMaxSweepSamples);
    SweepImage part(image.channels());
    if (!window || !part.update(held, *window, true, inputChannels, last - first)) {
        const bool had = image.hasPart();
        image.clearPart();
        return had;
    }
    image.showPart(part, first);
    return true;
}

} // namespace

Analyzer::Analyzer(Engine& engine, const InstanceRegistry& registry)
    : engine(engine), registry(registry), hold(engine.captureRing().capacity())
{}

LaneSet Analyzer::laneSet() const
{
    const std::scoped_lock lock(laneMutex);
    return published;
}

void Analyzer::publishLanes(std::vector<LaneLabel> labels, std::size_t hidden)
{
    const std::scoped_lock lock(laneMutex);
    if (published.id != 0 && published.lanes == labels && published.hidden == hidden) {
        return;
    }
    published = {.id = published.id + 1, .lanes = std::move(labels), .hidden = hidden};
}

void Analyzer::setColumns(std::size_t columns)
{
    const std::scoped_lock lock(commandMutex);
    commands.columns = columns;
}

void Analyzer::setFrozen(bool frozen)
{
    const std::scoped_lock lock(commandMutex);
    commands.frozen = frozen;
    if (!frozen) {
        commands.panSeconds = 0.0;
    }
}

void Analyzer::setPan(double seconds)
{
    const std::scoped_lock lock(commandMutex);
    commands.panSeconds = std::max(seconds, 0.0);
}

void Analyzer::setView(double start, double end)
{
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0.0 || end > 1.0 ||
        end - start < kMinimumViewWidth) {
        return;
    }
    const std::scoped_lock lock(commandMutex);
    commands.viewStart = start;
    commands.viewEnd = end;
}

// The checks are kept with the instance, so that they outlive this analyzer (the editor closed and
// opened again); they are not saved. A track checked before that is not drawn now (in another
// group, or beyond the tracks drawn) stays checked.
void Analyzer::setCompared(const std::vector<std::uint64_t>& ids)
{
    const LaneSet drawn = laneSet();
    std::vector<std::uint64_t> next;
    for (const std::uint64_t id : ids) {
        const auto lane = std::ranges::find(drawn.lanes, id, &LaneLabel::id);
        if ((lane == drawn.lanes.end() || !lane->own) && !contains(next, id)) {
            next.push_back(id);
        }
    }
    if (const auto previous = engine.compared()) {
        for (const std::uint64_t id : *previous) {
            const bool isDrawn = std::ranges::any_of(
                drawn.lanes, [&](const LaneLabel& lane) { return lane.id == id; });
            if (!isDrawn && !contains(next, id)) {
                next.push_back(id);
            }
        }
    }
    engine.setCompared(std::move(next));
}

void Analyzer::setPreviewFix(bool on)
{
    const std::scoped_lock lock(commandMutex);
    commands.previewFix = on;
}

void Analyzer::redraw()
{
    const std::scoped_lock lock(commandMutex);
    ++commands.redraws;
}

void Analyzer::arm()
{
    const std::scoped_lock lock(commandMutex);
    commands.armRequested = true;
}

// Readouts are shown at most every 100 ms, so live they are measured no more often than every
// 100 ms of audio, unless the settings changed. Frozen frames come only when something changed
// (such as a pan), so they always measure. `end` is this track's capture end.
bool Analyzer::readoutsDue(std::uint64_t end) const
{
    const auto interval =
        static_cast<std::uint64_t>(protocol::kReadoutIntervalSeconds * engine.sampleRate());
    return isFrozen || !readoutCache.valid || readoutCache.settings != frameSettings ||
           readoutCache.frozen != isFrozen || end < readoutCache.end ||
           end - readoutCache.end >= interval;
}

void Analyzer::storeReadouts(const Readouts& values, std::uint64_t end)
{
    readoutCache.values = values;
    readoutCache.end = end;
    readoutCache.valid = true;
}

void Analyzer::applyReadouts(FrameHeaderValues& header)
{
    if (!readoutCache.valid) {
        return;
    }
    readoutCache.settings = frameSettings;
    readoutCache.frozen = isFrozen;
    header.invalidSamples = readoutCache.values.invalid;
    header.peak = readoutCache.values.peak;
    header.rms = readoutCache.values.rms;
    header.frequency = static_cast<float>(
        readoutCache.values.frequencyHz.value_or(std::numeric_limits<double>::quiet_NaN()));
}

Analyzer::Commands Analyzer::takeCommands()
{
    const std::scoped_lock lock(commandMutex);
    Commands taken = commands;
    commands.armRequested = false;
    return taken;
}

void Analyzer::followTransport(const Settings& settings, Commands& command)
{
    const bool playing = engine.timeline().latest().playing;
    const bool enabled =
        settings.enumValue<protocol::Toggle>(SettingId::kAutoFreeze) == protocol::Toggle::kOn;
    if (!enabled || !command.frozen) {
        autoFrozen = false;
    }
    if (enabled && lastPlaying.has_value()) {
        if (*lastPlaying && !playing && !command.frozen) {
            setFrozen(true);
            command.frozen = true;
            autoFrozen = true;
        } else if (!*lastPlaying && playing && autoFrozen) {
            setFrozen(false);
            command.frozen = false;
            command.panSeconds = 0.0;
            autoFrozen = false;
        }
    }
    lastPlaying = playing;
}

bool Analyzer::freeze(double sampleRate, std::uint64_t minimumSamples)
{
    hold.resize(engine.captureRing().capacity()); // The rate may have changed since it opened.
    frozenStamp = engine.timeline().latest();
    frozenEnd = engine.captureRing().endIndex();
    frozenRate = sampleRate;
    const auto length = std::max<std::uint64_t>(hold.holdLength(sampleRate), minimumSamples);
    if (!hold.capture(engine.captureRing(), static_cast<std::size_t>(length))) {
        return false; // The writer outran the copy; try again on the next step.
    }
    isFrozen = true;
    const std::scoped_lock lock(commandMutex);
    commands.frozen = true; // Single mode freezes on its own; keep the command in step.
    return true;
}

bool Analyzer::produce(std::vector<std::byte>& frame)
{
    if (!produceFrame(frame)) {
        return false;
    }
    // Only Compare draws frames for its results alone; other frames are not kept to compare.
    if (!comparing) {
        lastSent.clear();
        return true;
    }
    if (resultsOnly && frame == lastSent) {
        return false; // The same frame again: a search or gains ended with the same values.
    }
    lastSent.assign(frame.begin(), frame.end());
    return true;
}

bool Analyzer::produceFrame(std::vector<std::byte>& frame)
{
    spreadStepped = false;
    searchEnded = false;
    resultsOnly = false;
    Commands command = takeCommands();
    const Settings settings = engine.settings().get();
    frameSettings = settings;
    followTransport(settings, command);
    const double liveRate = engine.sampleRate();
    if (command.armRequested) {
        trigger.arm();
    }
    const bool synced =
        settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase) == protocol::TimeBase::kBeats;
    if (command.frozen && !isFrozen) {
        if (!freeze(liveRate, frozenLength(engine.captureRing(), liveRate, synced))) {
            return false;
        }
        if (synced) {
            catchUpImageToHold();
        }
    }
    if (!command.frozen) {
        isFrozen = false;
    }
    // Frozen, what is shown is the audio held, placed and timed at the rate it was recorded at.
    const double sampleRate = isFrozen ? frozenRate : liveRate;
    if (command.columns == 0) {
        return false;
    }
    // Multi and Compare both draw lanes of the group's tracks.
    const bool multi = settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode) !=
                       protocol::DisplayMode::kSingle;
    // A sweep keeps the stamp of the audio it placed last only while it is placed live: frozen,
    // in Seconds or in the other display mode, the transport may jump meanwhile.
    forgetPlacing(!synced || isFrozen || multi, !synced || isFrozen || !multi);
    // A Beats sweep is placed by the host's tempo. Until the host has reported one (an instance
    // it has not played through), the clock's tempo is only a default: no sweep is drawn, and the
    // frame says why, again whenever the inputs change or the page asks for a redraw.
    if (synced && !(isFrozen ? frozenStamp : engine.timeline().latest()).tempoKnown) {
        forgetPlacing(true, true); // Nothing is placed: positions are not known.
        // Multi and Compare still list the group's tracks, so the page names their lanes.
        std::uint32_t laneSetId = 0;
        if (multi) {
            selectLanes(settings);
            laneSetId = laneSet().id;
        }
        if (sentNoTempo == isFrozen && !inputsChanged(settings, command, laneSetId)) {
            return false;
        }
        sentNoTempo = isFrozen;
        rememberInputs(settings, command, laneSetId);
        // Shown in Beats: switched to Seconds, the newest span is shown at once, as after a sweep.
        lastShown = Window{.synced = true};
        const FrameHeaderValues header{
            .flags = static_cast<std::uint8_t>(static_cast<unsigned>(flag(FrameFlag::kSynced)) |
                                               flag(FrameFlag::kNoTempo) |
                                               (isFrozen ? flag(FrameFlag::kFrozen) : 0U)),
            .laneSet = laneSetId,
            .sampleRate = static_cast<float>(sampleRate)};
        encodeFrame(header, {}, frame);
        return true;
    }
    sentNoTempo.reset();
    if (multi && synced) {
        return produceLanes(settings, command, frame);
    }
    if (multi) {
        selectLanes(settings);
    } else {
        lanes.clear(); // Frees the lanes' sweeps and holds outside the multi-track view.
        comparing = false;
    }

    const double spanSeconds = settings.value(SettingId::kTimeSpan);
    const bool freeRunning = isFreeRunning(
        spanSeconds, settings.enumValue<protocol::TriggerMode>(SettingId::kTriggerMode));
    // The capture is sized for the longest span at its rate, but a host that changes the rate
    // while processing leaves it as it was, and above the highest rate it grows no more: a longer
    // span shows what is kept, and says so. Live, that is all the capture can be read of. Frozen,
    // all that is held of what the capture could be read of then (the host may have prepared it
    // again since). Other tracks' lanes hold no more, placed by their own clocks, so a margin is
    // kept for the offset between tracks: what the capture spares beyond the longest span, up to
    // a lane's margin, or an eighth of what it keeps when it cannot hold that span.
    const auto readable = static_cast<double>(CaptureRing::readableLength(
        isFrozen ? hold.heldCapacity() : engine.captureRing().capacity()));
    // In whole samples, as holds are: the longest span held whole is then never short of a margin
    // by a fraction of a sample.
    const double spare =
        readable - static_cast<double>(std::llround(HoldBuffer::kHoldSeconds * sampleRate));
    const double laneMargin = !multi        ? 0.0
                              : spare > 0.0 ? std::min(spare, kLaneMarginSeconds * sampleRate)
                                            : readable / 8.0;
    const double kept = isFrozen
                            ? std::min(static_cast<double>(hold.endIndex() - hold.beginIndex()),
                                       readable - laneMargin)
                            : readable;
    // Holds are whole samples: at a rate that is not, a held span a fraction of a sample short
    // of the span's length holds all of it.
    const bool spanLimited =
        !synced && static_cast<double>(std::llround(spanSeconds * sampleRate)) > kept;
    const double span = spanLimited ? kept : spanSeconds * sampleRate;
    // Live in Seconds mode, a frame shows the newest audio: with no new audio on any track shown
    // and nothing else changed, it would be the last frame again, so none is drawn. The page asks
    // at display rate, also when the transport is stopped or the host has stopped processing.
    const bool liveSeconds = !synced && !isFrozen;
    // Where a free-running window, or Auto's untriggered fallback, ends: the paced edge, which
    // moves evenly between blocks, or the newest sample where pacing does not apply (short spans,
    // whose whole trace is new every frame anyway; before the block clock locks; offline).
    const std::uint64_t newest = engine.captureRing().endIndex();
    pacedEdge.observe(engine.blockStamp().latest(), liveRate);
    const bool paceable = liveSeconds && span >= protocol::kPacedSpanMinimumSeconds * sampleRate;
    const std::uint64_t edge =
        paceable ? pacedEdge.edge(clockNanos(), newest, protocol::kPacingDelayCapSeconds) : newest;
    // A free-running window, or Auto drawing untriggered, is new when its edge moved; a triggered
    // window only with new audio, so no work is done between blocks.
    const bool followsEdge =
        freeRunning || (settings.enumValue<protocol::TriggerMode>(SettingId::kTriggerMode) ==
                            protocol::TriggerMode::kAuto &&
                        lastShown && !lastShown->triggered && !lastShown->synced);
    if (liveSeconds) {
        liveEnds.clear();
        liveEnds.push_back(followsEdge ? edge : newest);
        if (multi) {
            for (const std::uint64_t serial : laneOrder) {
                liveEnds.push_back(lanes.at(serial)->engine->captureRing().endIndex());
            }
        }
    }
    const bool newAudio = !liveSeconds || liveEnds != lastLiveEnds;
    const bool newFormat = !isFrozen && formatsChanged(multi);
    // Other lanes, or other tracks compared, change the frame too. A match still to search does
    // only when its search can run: live with the transport playing, searches are spaced by audio,
    // so without new audio none would, and the frame drawn would be the same; and not while
    // another runs. Gains not measured for what is shown do, to begin measuring them.
    const std::uint32_t laneSetId = multi ? laneSet().id : 0;
    const bool inputs = inputsChanged(settings, command, laneSetId) || newFormat;
    resultsOnly = !inputs && (isFrozen || !newAudio);
    bool changed = inputs || (comparing &&
                              ((matchesPending && !searchingSerial && (newAudio || searchAtOnce)) ||
                               (!comparisonCache.valid && !gainJob.active)));
    // A search or gains running over frames go on whatever arrives, but change what is shown only
    // when one ends: until then they are stepped without drawing the same frame again.
    if (!changed && comparing && (searchingSerial || gainJob.active)) {
        changed = stepSpreadWork(liveRate);
    }
    if (liveSeconds && !changed && !newAudio && lastShown && !lastShown->synced) {
        return false;
    }

    Window window{.span = span, .freeRunning = freeRunning, .limited = spanLimited};
    auto sampleCount = static_cast<std::uint64_t>(std::llround(span));
    bool freezeAfterRender = false;
    const SampleSource* source =
        isFrozen ? static_cast<const SampleSource*>(&hold) : &engine.captureRing();

    if (synced) {
        // Tempo-synced sweep. Live, the capture's new samples are placed. Frozen, the sweep stays
        // as it is: a shorter span is cropped from it, and a longer one rebuilt from the held
        // audio, stepping down if it does not fit.
        const CaptureRing& ring = engine.captureRing();
        // Live, the sweep and its playhead reach the paced edge, so they move evenly between
        // blocks and no audio newer than the playhead shows in the dimmed previous cycle.
        const std::uint64_t end =
            isFrozen ? frozenEnd
                     : ownSweepEdge(settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan));
        if (!changed && end == lastEnd) {
            return false;
        }
        const auto beatSpan = settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan);
        const int channels = engine.inputChannels();
        SweepWindow sweep;
        if (isFrozen) {
            const auto frozen = frozenSweep(frozenStamp, frozenEnd, sampleRate, beatSpan,
                                            imageWindow, image.length());
            if (!frozen) {
                return false;
            }
            if (frozen->rebuild) {
                if (!image.update(hold, frozen->window, true, channels)) {
                    return false;
                }
                imageWindow = frozen->window;
            } else {
                const auto count = static_cast<double>(image.length());
                showHeldPart(image, hold, frozenStamp, frozenEnd, sampleRate, beatSpan, channels,
                             static_cast<std::uint64_t>(std::llround(frozen->start * count)),
                             static_cast<std::uint64_t>(std::llround(frozen->end * count)));
            }
            sweep = frozen->window;
            window.subStart = frozen->start;
            window.subEnd = frozen->end;
        } else {
            const auto placeBefore = [&](const MusicalStamp& previous, std::uint64_t until) {
                if (const auto before =
                        sweepWindow(previous, until, sampleRate, beatSpan, kMaxSweepSamples)) {
                    ++workDone.sweepUpdates;
                    (void)image.update(ring, *before, false, channels);
                }
            };
            const auto live = sweepWindow(
                placingStamp(engine.timeline().latest(), end, sampleRate, ownPlacing, placeBefore),
                end, sampleRate, beatSpan, kMaxSweepSamples);
            ++workDone.sweepUpdates;
            if (!live || !image.update(ring, *live, false, channels)) {
                return false;
            }
            imageWindow = live;
            placedSpan = beatSpan;
            sweep = *live;
        }
        source = &image;
        window.start = 0.0;
        window.span = static_cast<double>(image.length());
        window.freeRunning = false;
        window.synced = true;
        window.limited = sweep.limited;
        window.partial = !(image.complete() || image.partComplete());
        window.cursor = sweep.cursor;
        window.cycleStart = sweep.cycleStart;
        window.spanQuarterNotes = sweep.spanQuarterNotes;
        window.barQuarterNotes = sweep.barQuarterNotes;
        window.cycleSeconds = sweep.cycleSeconds;
        sampleCount = image.length();
        lastEnd = end;

    } else if (isFrozen) {
        if (!changed) {
            return false;
        }
        // Keep the trigger point (triggered) or the right edge (free-running) of the last live
        // window where it was, so changing the scale while frozen zooms around it. A Beats sweep's
        // window is a place in the sweep, not in the capture: frozen from Beats, the view ends at
        // the newest audio held, as when frozen before any live frame.
        const auto held = static_cast<double>(hold.endIndex());
        const std::optional<Window> live =
            lastLive && !lastLive->synced ? lastLive : std::optional<Window>{};
        const double rightEdge = live && live->freeRunning ? live->edge
                                 : live                    ? live->start + live->span
                                                           : held;
        const double anchor =
            !live || freeRunning ? rightEdge : live->start + kTriggerPosition * live->span;
        const double anchorOffset = freeRunning || !live ? span : kTriggerPosition * span;
        const double latestStart = held - span;
        // A view pans back through the newest frozenHoldSeconds held. The hold reaches a little
        // further (kSecondsFreezeSeconds), so the unpanned window, which may end a paced delay
        // before the newest audio, is never moved forward to fit.
        const auto heldStart = static_cast<double>(hold.beginIndex());
        const double earliestStart = std::max(
            heldStart,
            held - static_cast<double>(std::llround(protocol::kFrozenHoldSeconds * sampleRate)));
        const double unpanned = anchor - anchorOffset;
        const double lowest = std::max(heldStart, std::min(earliestStart, unpanned));
        window.start = std::clamp(unpanned - command.panSeconds * sampleRate, lowest,
                                  std::max(lowest, latestStart));
        // The UI keeps its pan within this, so it never asks for more than is held.
        window.panReach = std::max(0.0, (unpanned - earliestStart) / sampleRate);
        window.triggered = live && live->triggered;
    } else if (freeRunning) {
        const std::uint64_t end = edge;
        const auto spanSamples = static_cast<std::uint64_t>(std::llround(span));
        const FreeWindow whole = freeWindow(end, spanSamples, command.columns);
        const auto visibleSamples = std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(std::llround((command.viewEnd - command.viewStart) * span)));
        window.edge = static_cast<double>(end);
        window.readoutStart = whole.start;
        window.readoutCount = whole.sampleCount;
        if (visibleSamples <= command.columns) {
            // Raw samples: the whole span, cropped to the view as a triggered window is, so the
            // trace reaches past the screen's edges.
            sampleCount = std::min(spanSamples, end);
            window.start = static_cast<double>(end - sampleCount);
        } else {
            // Columns: the window covers only the visible part, on a grid of its own, so its
            // columns keep fixed sample boundaries and do not shimmer; readouts still cover the
            // whole span.
            const std::uint64_t hidden = std::min<std::uint64_t>(
                end, static_cast<std::uint64_t>(std::llround((1.0 - command.viewEnd) * span)));
            const FreeWindow shown = freeWindow(end - hidden, visibleSamples, command.columns);
            window.start = static_cast<double>(shown.start);
            window.visiblePart = true;
            window.onGrid = shown.onGrid;
            window.grid = {.spanSamples = visibleSamples, .columns = command.columns};
            window.firstColumn = shown.firstColumn;
            window.cropStart = 0.0;
            window.cropEnd = 1.0;
            sampleCount = shown.sampleCount;
        }
    } else {
        const CaptureRing& ring = engine.captureRing();
        const double timeout = kAutoTimeoutSeconds * sampleRate;
        const auto [first, end] =
            TriggerState::searchRegion(ring.beginIndex(), ring.endIndex(), span, timeout);
        triggerSamples.resize(static_cast<std::size_t>(end - first));
        scratch.resize(std::max(scratch.size(), triggerSamples.size()));
        // The multi-track view shows each track's Mid, and triggers on this track's.
        const ChannelView triggerView =
            multi
                ? ChannelView::kMid
                : effectiveTriggerSource(settings.enumValue<ChannelView>(SettingId::kTriggerSource),
                                         engine.inputChannels());
        if (!readSamples(ring, engine.inputChannels(), triggerView, first, triggerSamples,
                         scratch)) {
            return false;
        }
        const TriggerDecision decision = trigger.decide(
            triggerSamples, first, span, timeout, triggerParameters(settings, sampleRate),
            settings.enumValue<protocol::TriggerMode>(SettingId::kTriggerMode));
        if (decision.action == TriggerDecision::Action::kHold && lastShown &&
            (lastShown->synced || lastShown->freeRunning)) {
            // Just switched from Beats, or from a free-running window: the last window was a
            // place in a sweep, or another kind of window. Show the newest span, untriggered,
            // then wait for a trigger from there.
            window.start = static_cast<double>(end) - span;
        } else if (decision.action == TriggerDecision::Action::kHold) {
            if (!changed || !lastShown) {
                return false;
            }
            window.start = lastShown->start;
            window.triggered = lastShown->triggered;
        } else {
            // Auto drawing untriggered shows the newest span, ending at the paced edge.
            window.start =
                decision.triggered ? decision.windowStart : static_cast<double>(edge) - span;
            window.triggered = decision.triggered;
            freezeAfterRender = decision.freeze;
        }
    }

    window.viewStart = command.viewStart;
    window.viewEnd = command.viewEnd;
    if (!window.visiblePart) {
        const double shown = window.subEnd - window.subStart;
        window.cropStart = window.subStart + command.viewStart * shown;
        window.cropEnd = window.subStart + command.viewEnd * shown;
    }
    if (multi
            ? !renderLanes(*source, window, sampleCount, sampleRate, command, frame)
            : !render(*source, settings, sampleRate, window, sampleCount, command.columns, frame)) {
        return false;
    }
    if (!isFrozen) {
        lastLive = window;
        lastLiveEnds = liveEnds;
        lastLiveFormats = liveFormats;
        // For benchmarks: where a moving frame's right edge stood (a sweep's playhead in Beats).
        if (window.freeRunning || window.synced) {
            engine.recordEdge({.nanos = clockNanos(),
                               .edge = window.synced ? lastEnd : edge,
                               .newest = engine.captureRing().endIndex()});
        }
    }
    lastShown = window;
    rememberInputs(settings, command, laneSetId);
    if (freezeAfterRender) {
        freeze(sampleRate);
        lastFrozen = isFrozen; // The frame just sent already shows the held window.
    }
    return true;
}

bool Analyzer::render(const SampleSource& source, const Settings& settings, double sampleRate,
                      const Window& window, std::uint64_t sampleCount, std::size_t columns,
                      std::vector<std::byte>& frame)
{
    const int inputChannels = engine.inputChannels();
    const double first = std::floor(window.start);
    const auto start = static_cast<std::uint64_t>(std::max(first, 0.0));

    const auto views = effectiveViews(
        static_cast<std::uint8_t>(settings.value(SettingId::kChannelViews)), inputChannels);
    const ChannelView triggerView = effectiveTriggerSource(
        settings.enumValue<ChannelView>(SettingId::kTriggerSource), inputChannels);

    const Crop crop = cropOf(static_cast<double>(sampleCount), window.start - first,
                             window.cropStart, window.cropEnd);
    const double visibleSamples = crop.visibleSamples;

    FrameHeaderValues header{
        .flags =
            static_cast<std::uint8_t>((window.triggered ? flag(FrameFlag::kTriggered) : 0U) |
                                      (isFrozen ? flag(FrameFlag::kFrozen) : 0U) |
                                      (window.freeRunning ? flag(FrameFlag::kFreeRunning) : 0U) |
                                      (window.synced ? flag(FrameFlag::kSynced) : 0U) |
                                      (window.limited ? flag(FrameFlag::kLimited) : 0U) |
                                      (window.partial ? flag(FrameFlag::kPartial) : 0U)),
        .views = views,
        .startFraction = static_cast<float>(crop.startFraction),
        .cursor = static_cast<float>(window.cursor),
        .cycleStart = static_cast<float>(window.cycleStart),
        .spanQuarterNotes = static_cast<float>(window.spanQuarterNotes),
        .barQuarterNotes = static_cast<float>(window.barQuarterNotes),
        .cycleSeconds = static_cast<float>(window.cycleSeconds),
        .viewStart = static_cast<float>(window.viewStart),
        .viewEnd = static_cast<float>(window.viewEnd),
        .visibleSamples = static_cast<float>(visibleSamples),
        .panReach = static_cast<float>(window.panReach),
        .sampleRate = static_cast<float>(sampleRate),
        .spanSeconds = window.synced ? 0.0f : static_cast<float>(window.span / sampleRate)};
    frameData.clear();
    std::size_t pointCount = 0;
    // Each view keeps its columns from frame to frame where it can: a tempo-synced sweep reduces
    // again only the columns its newest samples landed in, a free-running window only the columns
    // that scrolled in. A triggered window is new every time.
    const bool synced = window.synced;
    // Readouts measure the trigger source, whose control is there in Seconds mode; in Beats mode,
    // where nothing triggers, the Mid, as the multi-track lanes do.
    const ChannelView readoutView = synced ? ChannelView::kMid : triggerView;
    const bool rolling = window.freeRunning && window.onGrid && !window.synced &&
                         window.cropStart == 0.0 && window.cropEnd == 1.0;
    const std::size_t visibleCount = crop.end - crop.first;
    const bool sameReduction = viewReduction.first == crop.first &&
                               viewReduction.channels == inputChannels &&
                               viewReduction.views == views;
    if (!rolling || viewReduction.channels != inputChannels) {
        for (RollColumns& roll : viewRolls) {
            roll.clear();
        }
    }
    for (std::size_t index = 0; index < protocol::kChannelViewCount; ++index) {
        if ((views & (1U << index)) == 0) {
            continue;
        }
        const auto view = static_cast<ChannelView>(index);
        // Reads for the view's columns: a sweep's or a free-running window's.
        const auto read = [&](std::uint64_t first, std::span<float> destination) {
            workDone.samplesReduced += destination.size();
            readScratch.resize(std::max(readScratch.size(), destination.size()));
            return readSamples(source, inputChannels, view, first, destination, readScratch);
        };
        if (synced) {
            ColumnReduction& reduction = viewReductions.at(index);
            const auto readVisible = [&](std::size_t first, std::span<float> destination) {
                return read(crop.first + first, destination);
            };
            bool updated = true;
            if (!sameReduction || !reduction.fits(visibleCount, columns)) {
                reduction.reset(visibleCount, columns);
                updated = reduction.update(0, visibleCount, readVisible);
            } else {
                for (const SweepImage::Range& range : image.changes()) {
                    const std::uint64_t from = std::max<std::uint64_t>(range.first, crop.first);
                    const std::uint64_t to = std::min<std::uint64_t>(range.end, crop.end);
                    if (from < to) {
                        updated = reduction.update(static_cast<std::size_t>(from) - crop.first,
                                                   static_cast<std::size_t>(to) - crop.first,
                                                   readVisible) &&
                                  updated;
                    }
                }
            }
            if (!updated) {
                return false;
            }
            header.mode = reduction.mode();
            pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visibleCount;
            frameData.insert(frameData.end(), reduction.values().begin(), reduction.values().end());
            continue;
        }
        if (rolling) {
            const auto values =
                viewRolls.at(index).update(0, window.firstColumn, window.grid, read);
            header.invalidLanes |= viewRolls.at(index).holdsInvalid() ? 1U : 0U;
            header.mode = protocol::FrameMode::kMinMax;
            pointCount = columns;
            frameData.insert(frameData.end(), values.begin(), values.end());
            continue;
        }
        // A window (at most a few seconds) is read whole; a sweep never is.
        viewSamples.resize(static_cast<std::size_t>(sampleCount));
        scratch.resize(std::max(scratch.size(), viewSamples.size()));
        if (!readSamples(source, inputChannels, view, start, viewSamples, scratch)) {
            return false;
        }
        const std::span<const float> visible =
            std::span(viewSamples).subspan(crop.first, crop.end - crop.first);
        header.invalidLanes |= holdsInvalid(visible) ? 1U : 0U;
        workDone.samplesReduced += visible.size();
        header.mode = reduceToColumns(visible, columns, reduced);
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visible.size();
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    }
    if (synced) {
        // The Mid's levels per block, for readouts, kept in step with the sweep.
        const bool fresh =
            !sweepStats.fits(static_cast<std::size_t>(sampleCount), statsBlock(sampleRate)) ||
            sweepStatsChannels != inputChannels;
        sweepStatsChannels = inputChannels;
        if (!updateStats(sweepStats, image, readoutView, inputChannels, fresh,
                         statsBlock(sampleRate))) {
            return false;
        }
        // Single has no lanes: bit 0 says what is shown holds samples that are not finite. The
        // Mid is not finite wherever a channel is, so its levels say so for every trace.
        header.invalidLanes |= holdsInvalid(sweepStats, crop.first, crop.end) ? 1U : 0U;
        image.clearChanges();
        viewReduction = {.first = crop.first, .channels = inputChannels, .views = views};
    } else {
        viewReduction = {};
    }
    if (rolling) {
        viewReduction.channels = inputChannels;
    }
    header.pointCount = static_cast<std::uint32_t>(pointCount);

    // Readouts cover the whole span, whatever part is on screen, at most every 100 ms of audio:
    // a sweep's from its levels per block, a window's from its samples.
    const std::uint64_t end = engine.captureRing().endIndex();
    if (readoutsDue(end)) {
        if (synced) {
            const auto shown = static_cast<double>(sampleCount);
            Readouts readouts;
            if (measureSweepReadouts(
                    sweepStats, static_cast<std::size_t>(std::llround(window.subStart * shown)),
                    static_cast<std::size_t>(std::llround(window.subEnd * shown)),
                    isFrozen ? static_cast<const SampleSource&>(hold) : engine.captureRing(),
                    isFrozen ? hold.endIndex() : end, sampleRate, readoutView, inputChannels,
                    readouts)) {
                storeReadouts(readouts, end);
            }
        } else {
            const std::uint64_t readoutStart =
                window.readoutCount > 0 ? window.readoutStart : start;
            const std::uint64_t readoutCount =
                window.readoutCount > 0 ? window.readoutCount : sampleCount;
            viewSamples.resize(static_cast<std::size_t>(readoutCount));
            scratch.resize(std::max(scratch.size(), viewSamples.size()));
            if (!readSamples(source, inputChannels, readoutView, readoutStart, viewSamples,
                             scratch)) {
                return false;
            }
            storeReadouts(computeReadouts(viewSamples, sampleRate), end);
        }
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
    return true;
}

bool Analyzer::updateStats(BlockStats& stats, const SweepImage& sweep, ChannelView view,
                           int channels, bool fresh, std::size_t block)
{
    const auto read = [&](std::size_t first, std::span<float> destination) {
        readScratch.resize(std::max(readScratch.size(), destination.size()));
        return readSamples(sweep, channels, view, first, destination, readScratch);
    };
    const auto length = static_cast<std::size_t>(sweep.length());
    if (fresh) {
        stats.reset(length, block);
        return stats.update(0, length, read);
    }
    bool measured = true;
    for (const SweepImage::Range& range : sweep.changes()) {
        measured = stats.update(static_cast<std::size_t>(range.first),
                                static_cast<std::size_t>(range.end), read) &&
                   measured;
    }
    return measured;
}

bool Analyzer::measureSweepReadouts(const BlockStats& stats, std::size_t first, std::size_t end,
                                    const SampleSource& live, std::uint64_t liveEnd,
                                    double sampleRate, ChannelView view, int channels,
                                    Readouts& readouts)
{
    const std::uint64_t begin = live.beginIndex();
    const std::uint64_t available = liveEnd > begin ? liveEnd - begin : 0;
    const std::size_t window = frequencyWindow(
        static_cast<std::size_t>(std::min<std::uint64_t>(available, end - first)), sampleRate);
    viewSamples.resize(window);
    scratch.resize(std::max(scratch.size(), viewSamples.size()));
    if (!readSamples(live, channels, view, liveEnd - window, viewSamples, scratch)) {
        return false;
    }
    readouts = levelsOf(stats, first, end);
    readouts.frequencyHz = estimateFrequency(viewSamples, sampleRate);
    return true;
}

// The tracks of the multi-track view: the group in order, at most kMaxLanes (one fewer in the
// Compare view, which adds their sum), always including this instance's own. Publishes their
// labels, keeps each lane's state across frames, and holds each lane's audio while frozen.
// Returns the own lane's serial.
std::uint64_t Analyzer::selectLanes(const Settings& settings)
{
    const bool synced =
        settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase) == protocol::TimeBase::kBeats;
    std::vector<GroupMember> members =
        registry.members(settings.enumValue<protocol::Group>(SettingId::kGroup));
    auto own = std::ranges::find_if(
        members, [&](const GroupMember& member) { return member.engine.get() == &engine; });
    if (own == members.end()) {
        // Not registered (a host without the registry entry): show this track alone. The
        // analyzer never outlives its engine, so the reference needs no ownership.
        members.insert(members.begin(),
                       {.engine = std::shared_ptr<Engine>(std::shared_ptr<Engine>(), &engine),
                        .track = engine.trackInfo(),
                        .serial = 0});
        own = members.begin();
    }
    const std::uint64_t ownSerial = own->serial;
    const bool compare = settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode) ==
                         protocol::DisplayMode::kCompare;
    const std::size_t maxTracks = compare ? kMaxLanes - 1 : kMaxLanes;
    std::size_t hidden = 0;
    if (members.size() > maxTracks) {
        hidden = members.size() - maxTracks;
        if (std::cmp_greater_equal(std::distance(members.begin(), own), maxTracks)) {
            std::rotate(members.begin() + static_cast<std::ptrdiff_t>(maxTracks - 1), own, own + 1);
        }
        members.resize(maxTracks);
    }

    // Compare view: this track, with the others asked for that are drawn (until asked, the first
    // other track). With no other track checked the sum is this track alone; with no other track
    // in the group there is nothing to compare.
    std::vector<std::uint64_t> compared;
    if (compare) {
        const auto selection = engine.compared();
        for (const GroupMember& member : members) {
            if (member.serial == ownSerial) {
                continue;
            }
            const bool asked = selection ? contains(*selection, member.serial) : compared.empty();
            if (asked) {
                compared.push_back(member.serial);
            }
        }
    }
    comparing = compare && members.size() > 1;
    const auto isCompared = [&](std::uint64_t serial) {
        return comparing && (serial == ownSerial || contains(compared, serial));
    };

    std::vector<LaneLabel> labels;
    labels.reserve(members.size());
    for (const GroupMember& member : members) {
        labels.push_back({.id = member.serial,
                          .name = member.track.name,
                          .colour = member.track.colour,
                          .own = member.serial == ownSerial,
                          .compared = isCompared(member.serial)});
    }
    publishLanes(std::move(labels), hidden);

    std::map<std::uint64_t, std::unique_ptr<Lane>> next;
    laneOrder.clear();
    for (GroupMember& member : members) {
        const auto found = lanes.find(member.serial);
        auto lane = found != lanes.end() ? std::move(found->second) : std::make_unique<Lane>();
        lane->engine = std::move(member.engine);
        const CaptureRing& ring = lane->engine->captureRing();
        if (isFrozen && !lane->hold.held()) {
            if (lane->hold.capture(*lane->engine,
                                   frozenLength(ring, lane->engine->sampleRate(), synced, true),
                                   scratch) &&
                synced) {
                catchUpToHold(*lane);
            }
        } else if (!isFrozen && lane->hold.held()) {
            lane->hold.release();
        }
        lane->compared = isCompared(member.serial);
        laneOrder.push_back(member.serial);
        next.insert_or_assign(member.serial, std::move(lane));
    }
    lanes = std::move(next);
    ownLane = ownSerial;
    return ownSerial;
}

// Live, the channel count and sample rate of this track and of each lane shown, against those
// of the last frame drawn live: a track turning mono, or a new sample rate with the same capture
// size, changes what a frame shows though no audio is new and no setting changed.
bool Analyzer::formatsChanged(bool multi)
{
    liveFormats.clear();
    const auto add = [&](const Engine& source) {
        liveFormats.push_back(static_cast<std::uint64_t>(source.inputChannels()));
        liveFormats.push_back(std::bit_cast<std::uint64_t>(source.sampleRate()));
    };
    add(engine);
    if (multi) {
        for (const std::uint64_t serial : laneOrder) {
            add(*lanes.at(serial)->engine);
        }
    }
    return liveFormats != lastLiveFormats;
}

// Whether anything that decides a frame, other than new audio, differs from the last frame's, and
// recording them once a frame is made: one list for every mode and time base, read and written in
// one place, so that a new input is never missed in one of them.
void Analyzer::rememberInputs(const Settings& settings, const Commands& command,
                              std::uint32_t laneSetId)
{
    lastSettings = settings;
    lastColumns = command.columns;
    lastPan = command.panSeconds;
    lastViewStart = command.viewStart;
    lastViewEnd = command.viewEnd;
    lastFrozen = isFrozen;
    lastLaneSet = laneSetId;
    lastPreviewFix = command.previewFix;
    lastRedraws = command.redraws;
}

bool Analyzer::inputsChanged(const Settings& settings, const Commands& command,
                             std::uint32_t laneSetId) const
{
    return settings != lastSettings || command.columns != lastColumns ||
           command.panSeconds != lastPan || isFrozen != lastFrozen ||
           command.viewStart != lastViewStart || command.viewEnd != lastViewEnd ||
           laneSetId != lastLaneSet || command.previewFix != lastPreviewFix ||
           command.redraws != lastRedraws;
}

bool Analyzer::placeLanes(const Settings& settings, protocol::BeatSpan span)
{
    placedSpan = span;
    bool fresh = false;
    // The own lane first: the others take its length.
    std::vector<std::uint64_t> order{ownLane};
    std::ranges::copy_if(laneOrder, std::back_inserter(order),
                         [&](std::uint64_t serial) { return serial != ownLane; });
    for (const std::uint64_t serial : order) {
        Lane& lane = *lanes.at(serial);
        const CaptureRing& ring = lane.engine->captureRing();
        const std::uint64_t end =
            serial == ownLane ? ownSweepEdge(span) : laneSweepEdge(lane, *lanes.at(ownLane));
        if (end != lane.end || !lane.window || settings != lastSettings) {
            const double rate = lane.engine->sampleRate();
            const auto placeBefore = [&](const MusicalStamp& previous, std::uint64_t until) {
                if (const auto before =
                        sweepWindow(previous, until, rate, span, kMaxSweepSamples)) {
                    ++workDone.sweepUpdates;
                    (void)lane.image.update(ring, *before, false, lane.engine->inputChannels(),
                                            sharedLength(serial, *before));
                }
            };
            const auto sweep = sweepWindow(placingStamp(lane.engine->timeline().latest(), end, rate,
                                                        lane.placing, placeBefore),
                                           end, rate, span, kMaxSweepSamples);
            ++workDone.sweepUpdates;
            const std::uint64_t length = sweep ? sharedLength(serial, *sweep) : 0;
            // Rebuilt only to take the shared length; otherwise the image keeps its own through
            // small changes.
            const bool adopt =
                sweep && length != sweep->sampleCount && lane.image.length() != length;
            if (sweep &&
                lane.image.update(ring, *sweep, adopt, lane.engine->inputChannels(), length)) {
                lane.window = sweep;
                fresh = true;
            }
            lane.end = end;
        }
    }
    return fresh;
}

// Where this track's live sweep is placed to: its paced edge, which moves evenly between blocks a
// delay behind the newest audio (the newest sample while pacing does not apply).
void Analyzer::forgetPlacing(bool own, bool lanesToo)
{
    if (own) {
        ownPlacing.reset();
    }
    if (lanesToo) {
        for (auto& [serial, lane] : lanes) {
            lane->placing.reset();
        }
    }
}

std::uint64_t Analyzer::ownSweepEdge(protocol::BeatSpan span)
{
    pacedEdge.observe(engine.blockStamp().latest(), engine.sampleRate());
    const std::uint64_t newest = engine.captureRing().endIndex();
    // A sweep shorter than pacedSpanMinimumSeconds is new across its width every frame: shown at
    // the newest audio, as a short Seconds span is.
    const MusicalStamp stamp = engine.timeline().latest();
    const double seconds =
        beatSpanQuarterNotes(span, Timeline::barQuarterNotes(stamp)) * 60.0 / stamp.tempo;
    if (seconds < protocol::kPacedSpanMinimumSeconds) {
        ownSweepPaced = false;
        return newest;
    }
    const std::uint64_t edge =
        pacedEdge.edge(clockNanos(), newest, protocol::kPacingDelayCapSeconds);
    ownSweepPaced = pacedEdge.paced();
    return edge;
}

// Where another track's lane is placed to: where this track's playhead stands, in the lane's
// capture, so the lanes' playheads move together though each track's blocks arrive at their own
// moments; never past the lane's audio, nor back. The two captures started at different moments,
// so the same audio has indices a constant apart, measured from the tracks' musical stamps
// (AD-15). A measurement more than a millisecond off means one track has seen a transport jump
// the other has not yet: it is taken only when it lasts (the offset itself changed, as when a
// track's processing restarted). The newest sample while this track is not paced, or the lane is
// at another rate.
std::uint64_t Analyzer::laneSweepEdge(Lane& lane, const Lane& own)
{
    constexpr double kOffsetToleranceSeconds = 0.001;
    // Placements, each as audio arrives and for each frame: about 0.6 s with 512-sample blocks at
    // 48 kHz, longer with longer blocks.
    constexpr int kOffsetPersistence = 100;
    const std::uint64_t newest = lane.engine->captureRing().endIndex();
    const double rate = lane.engine->sampleRate();
    if (!ownSweepPaced || rate != own.engine->sampleRate()) {
        lane.indexOffset.reset();
        return newest;
    }
    const MusicalStamp ownLatest = own.engine->timeline().latest();
    const MusicalStamp laneLatest = lane.engine->timeline().latest();
    const std::int64_t measured =
        static_cast<std::int64_t>(laneLatest.sampleIndex) -
        std::llround(Timeline::sampleIndexAt(ownLatest, laneLatest.quarterNotes, rate));
    if (!lane.indexOffset ||
        std::llabs(measured - *lane.indexOffset) <= std::llround(kOffsetToleranceSeconds * rate) ||
        ++lane.offsetDisagreements > kOffsetPersistence) {
        lane.indexOffset = measured;
        lane.offsetDisagreements = 0;
    }
    const auto mapped = static_cast<std::int64_t>(own.end) + *lane.indexOffset;
    // A lane whose audio has not arrived as far as this track's playhead (its blocks come later)
    // stops at its newest sample; the delay rises to cover it, so the lanes stay together.
    if (std::cmp_greater(mapped, newest)) {
        pacedEdge.coverShortfall(static_cast<double>(mapped - static_cast<std::int64_t>(newest)));
    }
    const auto edge = static_cast<std::uint64_t>(std::max<std::int64_t>(mapped, 0));
    return std::clamp(edge, std::min(lane.end, newest), newest);
}

// The musical stamp to place audio up to `end` by. The host gives each block its position; a
// sweep placed a delay behind the newest audio has not reached the newest blocks yet, and the
// timeline keeps only the newest position. So each position seen that does not continue the one
// before (a transport jump: a loop, a relocation, stop and start; or a tempo or metre change) is
// kept until the playhead reaches it. Audio before it is placed first, by `placeBefore` with the
// position it was played at, so a jump moves the sweep only when the playhead reaches it and
// never moves audio played before it. Positions that only continue one another place audio at
// the same columns, so the newest of them is used. Seen every 2 ms step that audio arrived, so a
// change is placed late only when a shorter block followed it within the step.
MusicalStamp
Analyzer::placingStamp(const MusicalStamp& latest, std::uint64_t end, double sampleRate,
                       SweepPlacing& placing,
                       const std::function<void(const MusicalStamp&, std::uint64_t)>& placeBefore)
{
    constexpr std::size_t kMostChanges = 64; // Far more than a sweep's delay can hold.
    const auto continues = [&](const MusicalStamp& previous) {
        return previous.tempo == latest.tempo && previous.numerator == latest.numerator &&
               previous.denominator == latest.denominator &&
               std::abs(Timeline::sampleIndexAt(previous, latest.quarterNotes, sampleRate) -
                        static_cast<double>(latest.sampleIndex)) <= 1.0;
    };
    if (placing.seen && latest.sampleIndex < placing.seen->sampleIndex) {
        placing.reset(); // The capture started over.
    }
    if (placing.seen && latest.sampleIndex > placing.seen->sampleIndex &&
        !continues(*placing.seen)) {
        if (placing.changes.size() == kMostChanges) {
            placing.changes.erase(placing.changes.begin());
        }
        placing.changes.push_back(latest);
    }
    placing.seen = latest;
    if (!placing.placing) {
        placing.placing = latest;
        placing.changes.clear();
        return latest;
    }
    while (!placing.changes.empty() && placing.changes.front().sampleIndex <= end) {
        const MusicalStamp change = placing.changes.front();
        placing.changes.erase(placing.changes.begin());
        if (change.sampleIndex > placing.placing->sampleIndex) {
            placeBefore(*placing.placing, change.sampleIndex);
        }
        placing.placing = change;
    }
    if (placing.changes.empty()) {
        placing.placing = latest; // Continues the position placed last.
    }
    return *placing.placing;
}

// Lanes are drawn on the own lane's grid, so their sweeps must hold as many samples. A tempo
// change reaches each track's clock at a slightly different moment, and each image keeps its
// length through small changes (see sweepLengthsSimilar), so on their own the lengths would part
// and stay apart. A lane's image takes the own image's length when its sweep is within that
// tolerance of it, and the sweep's own length otherwise.
std::uint64_t Analyzer::sharedLength(std::uint64_t serial, const SweepWindow& sweep) const
{
    const auto found = lanes.find(ownLane);
    if (serial == ownLane || found == lanes.end()) {
        return sweep.sampleCount;
    }
    // Within a sample too: a short sweep's image keeps no drift of its own, but its lane would
    // stay blank for being one sample off this track's.
    const std::uint64_t ownLength = found->second->image.length();
    const std::uint64_t apart =
        std::max(sweep.sampleCount, ownLength) - std::min(sweep.sampleCount, ownLength);
    return ownLength != 0 && (apart <= 1 || sweepLengthsSimilar(sweep.sampleCount, ownLength))
               ? ownLength
               : sweep.sampleCount;
}

void Analyzer::advance()
{
    // The block clock sees every block's stamp it can, also between frames.
    pacedEdge.observe(engine.blockStamp().latest(), engine.sampleRate());
    const Settings settings = engine.settings().get();
    const bool synced =
        settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase) == protocol::TimeBase::kBeats;
    if (isFrozen || !synced) {
        forgetPlacing(true, true);
        return; // Frozen sweeps stay as they are; windows are placed when a frame is made.
    }
    const auto beatSpan = settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan);
    const bool multi = settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode) !=
                       protocol::DisplayMode::kSingle;
    if (multi) {
        selectLanes(settings);
    }
    // Between frames, the sweeps are placed only as audio arrives: so none leaves the capture
    // unplaced, and each block's musical position is seen (placingStamp). The paced edge moves on
    // every step, and each frame places them up to it.
    std::uint64_t captured = engine.captureRing().endIndex();
    if (multi) {
        captured = 0;
        for (const auto& [serial, lane] : lanes) {
            captured += lane->engine->captureRing().endIndex();
        }
    }
    if (std::exchange(advancedCaptured, captured) == captured) {
        return;
    }
    if (multi) {
        lanesPlaced = placeLanes(settings, beatSpan) || lanesPlaced;
        return;
    }
    const CaptureRing& ring = engine.captureRing();
    const std::uint64_t end = ownSweepEdge(beatSpan);
    const auto placeBefore = [&](const MusicalStamp& previous, std::uint64_t until) {
        if (const auto before =
                sweepWindow(previous, until, engine.sampleRate(), beatSpan, kMaxSweepSamples)) {
            ++workDone.sweepUpdates;
            (void)image.update(ring, *before, false, engine.inputChannels());
        }
    };
    const auto sweep = sweepWindow(
        placingStamp(engine.timeline().latest(), end, engine.sampleRate(), ownPlacing, placeBefore),
        end, engine.sampleRate(), beatSpan, kMaxSweepSamples);
    ++workDone.sweepUpdates;
    if (sweep && image.update(ring, *sweep, false, engine.inputChannels())) {
        imageWindow = sweep;
        placedSpan = beatSpan;
    }
}

// The multi-track view in Beats mode: one lane per track of the group, each the track's Mid as a
// persistent sweep on its own musical clock, so that lanes line up on bar lines. While frozen
// the lanes are not updated, so all of them hold the same moment.
bool Analyzer::produceLanes(const Settings& settings, const Commands& command,
                            std::vector<std::byte>& frame)
{
    const std::uint64_t ownSerial = selectLanes(settings);
    const std::uint32_t laneSetId = laneSet().id;

    if (image.length() > 0) {
        image.clear(); // Single mode's sweep is not needed here; free its memory.
        imageWindow.reset();
    }
    const auto beatSpan = settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan);
    const Lane& own = *lanes.at(ownSerial);

    // Live, each lane follows its own capture and clock, and only new audio is placed. Frozen,
    // the lanes stay as they are; the own lane decides, as in Single mode, whether a span is
    // cropped from them or rebuilt from each lane's held audio.
    bool fresh = false;
    std::optional<SweepWindow> shown;
    double subStart = 0.0;
    double subEnd = 1.0;
    const bool placedBefore = std::exchange(lanesPlaced, false);
    if (!isFrozen) {
        fresh = placeLanes(settings, beatSpan) || placedBefore;
        shown = own.window;
    } else if (own.hold.held()) {
        const auto frozen =
            frozenSweep(own.hold.clock(), own.hold.endIndex(), own.hold.sampleRate(), beatSpan,
                        own.window, own.image.length());
        if (frozen && frozen->rebuild) {
            std::vector<std::uint64_t> order{ownSerial};
            std::ranges::copy_if(laneOrder, std::back_inserter(order),
                                 [&](std::uint64_t serial) { return serial != ownSerial; });
            for (const std::uint64_t serial : order) {
                Lane& lane = *lanes.at(serial);
                const LaneHold& laneHold = lane.hold;
                const auto held =
                    laneHold.held() ? sweepWindow(laneHold.clock(), laneHold.endIndex(),
                                                  laneHold.sampleRate(), beatSpan, kMaxSweepSamples)
                                    : std::nullopt;
                if (held && lane.image.update(laneHold, *held, true, lane.engine->inputChannels(),
                                              sharedLength(serial, *held))) {
                    lane.window = held;
                    fresh = true;
                }
            }
            shown = own.window;
        } else if (frozen) {
            shown = frozen->window;
            subStart = frozen->start;
            subEnd = frozen->end;
            fresh = showFrozenParts(beatSpan, subStart, subEnd);
        }
    } else {
        shown = own.window;
    }
    if (!shown) {
        return false;
    }

    // Checked whatever else changed, so that the formats drawn are always the ones remembered.
    const bool newFormat = !isFrozen && formatsChanged(true);
    const bool inputs = fresh || newFormat || inputsChanged(settings, command, laneSetId);
    resultsOnly = !inputs;
    bool changed = inputs || (comparing && matchesPending && !searchingSerial);
    // A search running over frames is stepped without drawing until it ends (see produce); sums
    // still being recomputed go on in the frames drawn between searches.
    if (!changed && comparing) {
        changed = searchingSerial ? stepSpreadWork(engine.sampleRate()) : sumsRefreshing();
    }
    if (!changed) {
        return false;
    }

    // Lanes share the own track's window and length (see sharedLength). A lane whose sweep is
    // further off (its own tempo or span differs, or frozen, it was held at another rate) is drawn
    // silent until it matches.
    const SweepWindow& window = *shown;
    const std::uint64_t count = own.image.length();
    const double shownPart = subEnd - subStart;
    const Crop crop =
        cropOf(static_cast<double>(count), 0.0, subStart + command.viewStart * shownPart,
               subStart + command.viewEnd * shownPart);
    const std::size_t columns = command.columns;
    // The rate of the own lane's audio: frozen, the one it was held at.
    const double sampleRate =
        isFrozen && own.hold.held() ? own.hold.sampleRate() : engine.sampleRate();
    const std::size_t block = statsBlock(sampleRate);
    frameData.clear();
    FrameHeaderValues header{
        .flags = static_cast<std::uint8_t>(flag(FrameFlag::kSynced) |
                                           (isFrozen ? flag(FrameFlag::kFrozen) : 0U) |
                                           (window.limited ? flag(FrameFlag::kLimited) : 0U)),
        .laneCount = static_cast<std::uint8_t>(laneOrder.size()),
        .startFraction = static_cast<float>(crop.startFraction),
        .cursor = static_cast<float>(window.cursor),
        .cycleStart = static_cast<float>(window.cycleStart),
        .spanQuarterNotes = static_cast<float>(window.spanQuarterNotes),
        .barQuarterNotes = static_cast<float>(window.barQuarterNotes),
        .cycleSeconds = static_cast<float>(window.cycleSeconds),
        .viewStart = static_cast<float>(command.viewStart),
        .viewEnd = static_cast<float>(command.viewEnd),
        .visibleSamples = static_cast<float>(crop.visibleSamples),
        .laneSet = laneSetId,
        .sampleRate = static_cast<float>(sampleRate)};
    // Each lane keeps its columns from frame to frame; only those holding samples placed since
    // the last frame are reduced again.
    const std::size_t visibleCount = crop.end - crop.first;
    std::size_t pointCount = 0;
    bool partial = false;
    frameChanges.clear();
    for (std::size_t laneIndex = 0; laneIndex < laneOrder.size(); ++laneIndex) {
        const std::uint64_t serial = laneOrder[laneIndex];
        Lane& lane = *lanes.at(serial);
        ColumnReduction& reduction = lane.reduction;
        // The image holds the Mid alone: read as is, with no working space.
        const auto read = [&](std::size_t first, std::span<float> destination) {
            workDone.samplesReduced += destination.size();
            return readSamples(lane.image, lane.image.channels(), ChannelView::kMid,
                               crop.first + first, destination, {});
        };
        bool updated = false;
        if (lane.image.length() == count) {
            if (!reduction.fits(visibleCount, columns) || lane.reducedFirst != crop.first) {
                reduction.reset(visibleCount, columns);
                lane.reducedFirst = crop.first;
                updated = reduction.update(0, visibleCount, read);
            } else {
                updated = true;
                for (const SweepImage::Range& range : lane.image.changes()) {
                    const std::uint64_t first = std::max<std::uint64_t>(range.first, crop.first);
                    const std::uint64_t end = std::min<std::uint64_t>(range.end, crop.end);
                    if (first < end) {
                        updated =
                            reduction.update(static_cast<std::size_t>(first) - crop.first,
                                             static_cast<std::size_t>(end) - crop.first, read) &&
                            updated;
                    }
                }
            }
            // The lane's levels per block, for readouts and summing gains.
            updateStats(lane.stats, lane.image, ChannelView::kMid, lane.image.channels(),
                        !lane.stats.fits(static_cast<std::size_t>(count), block), block);
            header.invalidLanes |=
                holdsInvalid(lane.stats, crop.first, crop.end) ? laneBit(laneIndex) : 0U;
            partial = partial || !(lane.image.complete() || lane.image.partComplete());
            if (comparing && lane.compared) {
                frameChanges.insert(frameChanges.end(), lane.image.changes().begin(),
                                    lane.image.changes().end());
            }
            lane.image.clearChanges();
        }
        if (!updated) {
            // Not on this sweep's length (yet): silent, and updated afresh once it is.
            reduction.reset(visibleCount, columns);
            reduction.invalidate();
        }
        header.mode = reduction.mode();
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visibleCount;
        frameData.insert(frameData.end(), reduction.values().begin(), reduction.values().end());
    }
    header.pointCount = static_cast<std::uint32_t>(pointCount);
    mergeRanges(frameChanges);

    if (partial) {
        header.flags = static_cast<std::uint8_t>(header.flags | flag(FrameFlag::kPartial));
    }
    if (comparing) {
        const auto playhead = static_cast<std::size_t>(
            std::llround((subStart + window.cursor * shownPart) * static_cast<double>(count)));
        header.invalidLanes |= sumLaneBit(header.invalidLanes);
        appendComparison(header, crop.first, crop.end, count, columns, command.previewFix, playhead,
                         sampleRate);
    }

    // Readouts measure the own track's Mid: peak and RMS over the whole span from its levels per
    // block, the frequency over the newest second, at most every 100 ms of audio.
    const std::uint64_t ownEnd = own.engine->captureRing().endIndex();
    if (readoutsDue(ownEnd) && own.stats.fits(static_cast<std::size_t>(count), block)) {
        Readouts readouts;
        if (measureSweepReadouts(
                own.stats,
                static_cast<std::size_t>(std::llround(subStart * static_cast<double>(count))),
                static_cast<std::size_t>(std::llround(subEnd * static_cast<double>(count))),
                isFrozen ? static_cast<const SampleSource&>(own.hold) : own.engine->captureRing(),
                isFrozen ? own.hold.endIndex() : ownEnd, sampleRate, ChannelView::kMid,
                engine.inputChannels(), readouts)) {
            storeReadouts(readouts, ownEnd);
        }
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
    // A Beats sweep was shown: a later Seconds frame has no window of its own to go on, and a
    // later freeze no live one. A freeze made in Seconds keeps its window, as in Single mode.
    lastShown = Window{.synced = true};
    if (!isFrozen) {
        lastLive = lastShown;
    }
    if (!isFrozen) {
        lastLiveFormats = liveFormats;
        // For benchmarks: where this track's playhead stood.
        engine.recordEdge(
            {.nanos = clockNanos(), .edge = own.end, .newest = engine.captureRing().endIndex()});
    }
    rememberInputs(settings, command, laneSetId);
    return true;
}

// Single mode's sweep, as the lanes' (see catchUpToHold): drawn up to where the freeze holds the
// audio, on the span it was drawn with.
void Analyzer::catchUpImageToHold()
{
    const auto held = sweepWindow(frozenStamp, frozenEnd, frozenRate, placedSpan, kMaxSweepSamples);
    if (!held || image.length() == 0 || !imageWindow) {
        return;
    }
    if (image.update(hold, *held, false, engine.inputChannels(), image.length())) {
        imageWindow = held;
    }
}

// The lanes were last placed a frame before the freeze, which holds the audio up to its moment:
// what the hold has since is placed too, so that a frozen sweep ends, and its playhead stands,
// where the hold does. On the span the lanes were drawn with, also when another is chosen at the
// same moment: it is then cropped or rebuilt from them as drawn.
void Analyzer::catchUpToHold(Lane& lane)
{
    const auto held = sweepWindow(lane.hold.clock(), lane.hold.endIndex(), lane.hold.sampleRate(),
                                  placedSpan, kMaxSweepSamples);
    const std::uint64_t length = lane.image.length();
    if (!held || length == 0 || !lane.window) {
        return;
    }
    if (lane.image.update(lane.hold, *held, false, lane.engine->inputChannels(), length)) {
        lane.window = held;
    }
}

// Frozen, a span cropped from the lanes, over [start, end) of the own lane's image: each lane
// still filling in shows that span rebuilt from its held audio (see showHeldPart); a whole lane
// keeps the crop, with all it drew. True if any lane's part changed.
bool Analyzer::showFrozenParts(protocol::BeatSpan span, double start, double end)
{
    const auto count = static_cast<double>(lanes.at(ownLane)->image.length());
    const auto first = static_cast<std::uint64_t>(std::llround(start * count));
    const auto last = static_cast<std::uint64_t>(std::llround(end * count));
    bool changed = false;
    for (const auto& [serial, lane] : lanes) {
        if (!lane->hold.held() || static_cast<double>(lane->image.length()) != count) {
            changed = changed || lane->image.hasPart();
            lane->image.clearPart();
            continue;
        }
        changed = showHeldPart(lane->image, lane->hold, lane->hold.clock(), lane->hold.endIndex(),
                               lane->hold.sampleRate(), span, lane->engine->inputChannels(), first,
                               last) ||
                  changed;
    }
    return changed;
}

// The sum lane's bit in a frame's `invalidLanes`: set when a compared lane's is, since the sum
// of a sample that is not finite is not finite either.
std::uint32_t Analyzer::sumLaneBit(std::uint32_t laneBits) const
{
    for (std::size_t index = 0; index < laneOrder.size(); ++index) {
        if ((laneBits & laneBit(index)) != 0 && lanes.at(laneOrder[index])->compared) {
            return laneBit(laneOrder.size());
        }
    }
    return 0U;
}

// The multi-track view with the seconds time base: every lane shows this track's window (its
// trigger, free-running or frozen window), placed in each other track's capture through the musical
// clock that all instances share with the host.
bool Analyzer::renderLanes(const SampleSource& ownSource, const Window& window,
                           std::uint64_t sampleCount, double sampleRate, const Commands& command,
                           std::vector<std::byte>& frame)
{
    const std::size_t columns = command.columns;
    const double first = std::floor(window.start);
    const auto start = static_cast<std::uint64_t>(std::max(first, 0.0));
    const MusicalStamp ownClock = isFrozen ? frozenStamp : engine.timeline().latest();
    const double startQuarterNotes = Timeline::quarterNotesAt(ownClock, start, sampleRate);
    const Crop crop = cropOf(static_cast<double>(sampleCount), window.start - first,
                             window.cropStart, window.cropEnd);
    viewSamples.resize(static_cast<std::size_t>(sampleCount));
    scratch.resize(std::max(scratch.size(), viewSamples.size()));

    FrameHeaderValues header{
        .flags =
            static_cast<std::uint8_t>((window.triggered ? flag(FrameFlag::kTriggered) : 0U) |
                                      (isFrozen ? flag(FrameFlag::kFrozen) : 0U) |
                                      (window.freeRunning ? flag(FrameFlag::kFreeRunning) : 0U) |
                                      (window.limited ? flag(FrameFlag::kLimited) : 0U)),
        .laneCount = static_cast<std::uint8_t>(laneOrder.size()),
        .startFraction = static_cast<float>(crop.startFraction),
        .viewStart = static_cast<float>(window.viewStart),
        .viewEnd = static_cast<float>(window.viewEnd),
        .visibleSamples = static_cast<float>(crop.visibleSamples),
        .laneSet = laneSet().id,
        .panReach = static_cast<float>(window.panReach),
        .sampleRate = static_cast<float>(sampleRate),
        .spanSeconds = window.synced ? 0.0f : static_cast<float>(window.span / sampleRate)};
    // Where each lane's part of the window starts in its own capture (or hold).
    const auto sourceOf =
        [&](std::uint64_t serial) -> std::pair<const SampleSource*, std::uint64_t> {
        const Lane& lane = *lanes.at(serial);
        if (serial == ownLane) {
            return {&ownSource, start};
        }
        const MusicalStamp clock = isFrozen ? lane.hold.clock() : lane.engine->timeline().latest();
        const SampleSource* source =
            isFrozen ? static_cast<const SampleSource*>(&lane.hold) : &lane.engine->captureRing();
        // Frozen, a lane held at another rate than this track's audio (first held after the host
        // changed it) holds none of this window's samples: it is drawn silent from the end of its
        // hold, as a Beats lane on another sweep length is.
        if (isFrozen && lane.hold.sampleRate() != sampleRate) {
            return {source, lane.hold.endIndex()};
        }
        return {
            source,
            static_cast<std::uint64_t>(std::max(
                std::round(Timeline::sampleIndexAt(clock, startQuarterNotes, sampleRate)), 0.0))};
    };
    // A free-running window falls into columns on a grid fixed to sample indices: each lane keeps
    // its columns, on the same grid from where its part of the window starts, and reduces only
    // the new ones.
    const bool rolling =
        window.freeRunning && window.onGrid && window.cropStart == 0.0 && window.cropEnd == 1.0;
    // Where a lane's grid starts, from where its part of the window starts (wrapping arithmetic:
    // the lane's columns are read from this plus the grid's boundaries).
    const auto gridOrigin = [&](std::uint64_t laneStart) {
        return laneStart - window.grid.boundary(window.firstColumn);
    };
    if (comparing) {
        laneSources.clear();
        for (const std::uint64_t serial : laneOrder) {
            laneSources.push_back(sourceOf(serial));
        }
        renderCompare(header, window, start, sampleCount, crop.first, crop.end, rolling, sampleRate,
                      command, frame);
        return true;
    }
    frameData.clear();
    std::size_t pointCount = 0;
    for (std::size_t index = 0; index < laneOrder.size(); ++index) {
        const std::uint64_t serial = laneOrder[index];
        Lane& lane = *lanes.at(serial);
        const auto [source, laneStart] = sourceOf(serial);
        const int channels = lane.engine->inputChannels();
        if (rolling) {
            const auto values = lane.roll.update(
                gridOrigin(laneStart), window.firstColumn, window.grid,
                [&](std::uint64_t first, std::span<float> destination) {
                    workDone.samplesReduced += destination.size();
                    readScratch.resize(std::max(readScratch.size(), destination.size()));
                    return readSamples(*source, channels, ChannelView::kMid, first, destination,
                                       readScratch);
                });
            header.mode = protocol::FrameMode::kMinMax;
            pointCount = columns;
            frameData.insert(frameData.end(), values.begin(), values.end());
            header.invalidLanes |= lane.roll.holdsInvalid() ? laneBit(index) : 0U;
            continue;
        }
        lane.roll.clear();
        // What the lane holds of the window; silence where it does not (yet, or any more).
        (void)readSamplesHeld(*source, channels, ChannelView::kMid, laneStart, viewSamples,
                              scratch);
        const std::span<const float> visible =
            std::span(viewSamples).subspan(crop.first, crop.end - crop.first);
        header.invalidLanes |= holdsInvalid(visible) ? laneBit(index) : 0U;
        workDone.samplesReduced += visible.size();
        header.mode = reduceToColumns(visible, columns, reduced);
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visible.size();
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    }
    header.pointCount = static_cast<std::uint32_t>(pointCount);

    // Readouts measure this track's Mid over the whole span, at most every 100 ms of audio.
    const std::uint64_t ownEnd = engine.captureRing().endIndex();
    if (readoutsDue(ownEnd)) {
        const std::uint64_t readoutStart = window.readoutCount > 0 ? window.readoutStart : start;
        const std::uint64_t readoutCount =
            window.readoutCount > 0 ? window.readoutCount : sampleCount;
        viewSamples.resize(static_cast<std::size_t>(readoutCount));
        scratch.resize(std::max(scratch.size(), viewSamples.size()));
        if (readSamples(ownSource, engine.inputChannels(), ChannelView::kMid, readoutStart,
                        viewSamples, scratch)) {
            storeReadouts(computeReadouts(viewSamples, sampleRate), ownEnd);
        }
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
    return true;
}

// The compared track whose match to search next: a track without a match first, else the
// one with the oldest match that is due, provided kMatchSpacingSeconds of audio passed since the
// last search. A change of the tracks compared, the view or the span, or a freeze or its end,
// makes every match due, but keeps it on show until its search; so does panning a frozen view
// (`shownFrom`, where its window starts). Frozen, or with the transport stopped, no audio may
// arrive to space those searches by: they come one after another instead, and the later
// refreshes are spaced again; frozen, the audio is held, so there are none. None while a search
// runs. Notes whether matches remain to search.
std::optional<std::size_t> Analyzer::matchToSearch(std::uint32_t laneSet, std::size_t first,
                                                   std::size_t end, std::size_t length,
                                                   std::uint64_t now, std::uint64_t shownFrom,
                                                   double viewStart, double viewEnd)
{
    // A zoomed live free-running window draws only its visible part, so a pan of the same width
    // changes no crop or length: the view itself says where it is.
    const bool sameView = laneSet == matchLaneSet && first == matchFirst && end == matchEnd &&
                          length == matchCount && isFrozen == matchFrozen &&
                          viewStart == matchViewStart && viewEnd == matchViewEnd;
    // Panned and nothing else: a drag moves the view a little each frame.
    const bool panned = sameView && isFrozen && shownFrom != matchShownFrom;
    panning = false;
    if (!sameView || panned) {
        for (auto& [serial, state] : matchStates) {
            state.wait = 0;
            state.failedReads = 0;
        }
        comparisonCache.valid = false;
        matchLaneSet = laneSet;
        matchFirst = first;
        matchEnd = end;
        matchCount = length;
        matchFrozen = isFrozen;
        matchShownFrom = shownFrom;
        matchViewStart = viewStart;
        matchViewEnd = viewEnd;
        searchAtOnce = isFrozen || !engine.timeline().latest().playing;
        // A search of the audio shown before measures what is no longer shown, and so do gains:
        // forgotten, it is not stepped again, and the next search starts afresh.
        searchingSerial.reset();
        gainJob.active = false;
        if (panned) {
            // Nothing begins until the view stays where it is for a frame: while it is dragged,
            // each search and measurement would be ended by the next frame's move.
            panning = true;
            matchesPending = true;
            return std::nullopt;
        }
    }
    std::erase_if(matchStates, [&](const auto& entry) {
        return std::ranges::find(laneOrder, entry.first) == laneOrder.end();
    });
    // One search at a time: frames go on meanwhile, as they do while matches remain to search.
    if (searchingSerial) {
        matchesPending = true;
        return std::nullopt;
    }
    const double sampleRate = engine.sampleRate();
    std::optional<std::size_t> chosen;
    std::uint64_t chosenAt = 0;
    bool missing = false;
    std::size_t candidates = 0;
    for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
        const std::uint64_t serial = laneOrder[lane];
        if (serial == ownLane || !lanes.at(serial)->compared) {
            continue;
        }
        const auto found = matchStates.find(serial);
        if (found == matchStates.end()) {
            ++candidates;
            if (!missing) {
                chosen = lane;
                missing = true;
            }
        } else if (found->second.wait == 0 ||
                   (!isFrozen && (now < found->second.analysedAt ||
                                  now - found->second.analysedAt >= found->second.wait))) {
            // Made due by a change of view, or by the audio played since its search. Frozen,
            // the audio shown is held and does not change: a match found there stays.
            ++candidates;
            if (!missing && (!chosen || found->second.analysedAt < chosenAt)) {
                chosen = lane;
                chosenAt = found->second.analysedAt;
            }
        }
    }
    const auto spacing = static_cast<std::uint64_t>(std::lround(kMatchSpacingSeconds * sampleRate));
    if (!chosen || (!missing && !searchAtOnce && now - lastMatchSearch < spacing)) {
        matchesPending = chosen.has_value();
        searchAtOnce = searchAtOnce && chosen.has_value();
        return std::nullopt;
    }
    matchesPending = candidates > 1;
    searchAtOnce = searchAtOnce && matchesPending;
    lastMatchSearch = now;
    return chosen;
}

void Analyzer::buildParts()
{
    rawParts.clear();
    fixedParts.clear();
    for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
        const std::uint64_t serial = laneOrder[lane];
        if (!lanes.at(serial)->compared) {
            continue;
        }
        SumPart fixed{.lane = lane};
        if (const auto found = matchStates.find(serial); found != matchStates.end()) {
            fixed.offset = found->second.analysis.offset;
            fixed.sign = found->second.analysis.inverted ? -1.0f : 1.0f;
        }
        rawParts.push_back({.lane = lane});
        fixedParts.push_back(fixed);
    }
}

void Analyzer::packMatches()
{
    std::vector<float>& matchValues = comparisonCache.matches;
    assignNoMatches(matchValues, laneOrder.size());
    for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
        const std::uint64_t serial = laneOrder[lane];
        const auto found = matchStates.find(serial);
        if (!lanes.at(serial)->compared || found == matchStates.end()) {
            continue; // This track, and tracks not compared, have none.
        }
        const PairAnalysis& match = found->second.analysis;
        float* const values = matchValues.data() + lane * protocol::kMatchValues;
        values[0] = match.measured ? static_cast<float>(match.correlation)
                                   : std::numeric_limits<float>::quiet_NaN();
        values[1] = match.related ? static_cast<float>(match.offset)
                                  : std::numeric_limits<float>::quiet_NaN();
        values[2] = !match.related ? 0.0f : match.inverted ? -1.0f : 1.0f;
    }
}

std::pair<std::ptrdiff_t, std::ptrdiff_t> Analyzer::fixedReach(const std::vector<SumPart>& parts)
{
    std::ptrdiff_t earliest = 0;
    std::ptrdiff_t latest = 0;
    for (const SumPart& part : parts) {
        earliest = std::min(earliest, part.offset);
        latest = std::max(latest, part.offset);
    }
    return {earliest, latest};
}

bool Analyzer::continueMatchSearch(double liveRate)
{
    if (!searchingSerial) {
        return false;
    }
    const std::uint64_t before = matchSearch.work();
    const bool ended = matchSearch.step(kMatchSearchWork);
    workDone.searchWork += matchSearch.work() - before;
    if (!ended) {
        matchesPending = true; // Frames go on until it ends, frozen too.
        return false;
    }
    matchStates[*searchingSerial] = {
        .analysis = matchSearch.result(),
        .analysedAt = searchStartedAt,
        .wait = static_cast<std::uint64_t>(std::lround(kMatchRefreshSeconds * liveRate))};
    searchingSerial.reset();
    return true;
}

bool Analyzer::readSamples(const SampleSource& source, int inputChannels, ChannelView view,
                           std::uint64_t start, std::span<float> destination,
                           std::span<float> working)
{
    workDone.samplesRead += destination.size();
    return readView(source, inputChannels, view, start, destination, working);
}

bool Analyzer::readSamplesHeld(const SampleSource& source, int inputChannels, ChannelView view,
                               std::uint64_t start, std::span<float> destination,
                               std::span<float> working)
{
    workDone.samplesRead += destination.size();
    return readViewHeld(source, inputChannels, view, start, destination, working);
}

void Analyzer::readLaneImage(std::size_t index, std::size_t length, std::uint64_t from,
                             std::span<float> into)
{
    const Lane& lane = *lanes.at(laneOrder[index]);
    // The image holds the Mid alone: read as is, with no working space.
    if (lane.image.length() != length ||
        !readSamples(lane.image, lane.image.channels(), ChannelView::kMid, from, into, {})) {
        std::ranges::fill(into, 0.0f);
    }
}

void Analyzer::readSum(const std::vector<SumPart>& parts, std::size_t length, std::size_t position,
                       std::span<float> into)
{
    std::ranges::fill(into, 0.0f);
    for (const SumPart& part : parts) {
        // Positions position + i show this track's sample at position + i + offset.
        const std::ptrdiff_t source = static_cast<std::ptrdiff_t>(position) + part.offset;
        const std::ptrdiff_t begin = std::max<std::ptrdiff_t>(0, -source);
        const std::ptrdiff_t stop = std::min<std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(into.size()), static_cast<std::ptrdiff_t>(length) - source);
        if (begin >= stop) {
            continue;
        }
        sumScratch.resize(static_cast<std::size_t>(stop - begin));
        workDone.samplesSummed += sumScratch.size();
        const std::ptrdiff_t from = source + begin;
        readLaneImage(part.lane, length, static_cast<std::uint64_t>(from), sumScratch);
        for (std::ptrdiff_t i = begin; i < stop; ++i) {
            into[static_cast<std::size_t>(i)] +=
                part.sign * sumScratch[static_cast<std::size_t>(i - begin)];
        }
    }
}

bool Analyzer::updateSumStats(BlockStats& stats, SumRefresh& refresh,
                              const std::vector<std::int64_t>& key,
                              std::vector<std::int64_t>& lastKey, const std::vector<SumPart>& parts,
                              std::size_t reach, std::size_t length, std::size_t block,
                              std::size_t shares)
{
    const auto read = [&](std::size_t from, std::span<float> into) {
        readSum(parts, length, from, into);
        return true;
    };
    if (!stats.fits(length, block)) {
        stats.reset(length, block);
        stats.update(0, length, read);
        lastKey = key;
        refresh = {};
        return true;
    }
    if (key != lastKey) {
        // Other parts: every block again, over the next frames; meanwhile the blocks not redone
        // yet still hold the previous parts' levels.
        refresh.remaining = length;
        lastKey = key;
    }
    for (const SweepImage::Range& range : frameChanges) {
        const auto from = static_cast<std::size_t>(range.first > reach ? range.first - reach : 0);
        const auto to =
            static_cast<std::size_t>(std::min<std::uint64_t>(range.end + reach, length));
        if (from < to) {
            stats.update(from, to, read);
        }
    }
    const bool refreshing = refresh.remaining > 0;
    continueRefresh(refresh, length, refreshStep(parts.size(), shares),
                    [&](std::size_t from, std::size_t to) { stats.update(from, to, read); });
    return refreshing;
}

void Analyzer::collectEnergies(std::span<const Positions> ranges, std::size_t length,
                               std::size_t block, const std::vector<SumPart>& parts,
                               const BlockStats& sumStats, std::vector<std::vector<double>>& tracks,
                               std::vector<std::span<const double>>& spans,
                               std::vector<double>& sums)
{
    tracks.resize(parts.size());
    for (std::vector<double>& energies : tracks) {
        energies.clear();
    }
    sums.clear();
    const bool summed = sumStats.fits(length, block);
    // Positions [from, to) measured from their samples. A position where the sum is not finite,
    // as it is wherever a compared track's sample is not (a broken signal upstream), is left out
    // of the sum and of every part alike, as in Seconds mode.
    const auto measureExactly = [&](Positions positions) {
        const std::size_t size = positions.end - positions.first;
        pieceSumScratch.resize(size);
        readSum(parts, length, positions.first, pieceSumScratch);
        double sum = 0.0;
        for (const float value : pieceSumScratch) {
            if (std::isfinite(value)) {
                sum += static_cast<double>(value) * value;
            }
        }
        sums.push_back(sum);
        for (std::size_t part = 0; part < parts.size(); ++part) {
            pieceScratch.resize(size);
            onePart.assign(1, parts[part]);
            readSum(onePart, length, positions.first, pieceScratch);
            double energy = 0.0;
            for (std::size_t i = 0; i < size; ++i) {
                if (std::isfinite(pieceSumScratch[i])) {
                    energy += static_cast<double>(pieceScratch[i]) * pieceScratch[i];
                }
            }
            tracks[part].push_back(energy);
        }
    };
    for (const Positions range : ranges) {
        if (range.first >= range.end) {
            continue;
        }
        const std::size_t firstBlock = (range.first + block - 1) / block;
        const std::size_t endBlock = range.end / block;
        const bool wholeBlocks = endBlock > firstBlock;
        if (!wholeBlocks) {
            measureExactly(range);
            continue;
        }
        // The pieces of blocks at the edges of the range, measured from their samples; whole
        // blocks from the levels kept per block, unless a sample in them is not finite.
        if (range.first < firstBlock * block) {
            measureExactly({.first = range.first, .end = firstBlock * block});
        }
        for (std::size_t index = firstBlock; index < endBlock; ++index) {
            if (summed && sumStats.blocks()[index].invalid > 0) {
                measureExactly({.first = index * block, .end = (index + 1) * block});
                continue;
            }
            sums.push_back(summed ? sumStats.blocks()[index].sumSquares : 0.0);
            for (std::size_t part = 0; part < parts.size(); ++part) {
                // A part as recorded has the lane's own levels; one the fix shifts, its levels as
                // shifted.
                const Lane& lane = *lanes.at(laneOrder[parts[part].lane]);
                const BlockStats& stats = parts[part].offset == 0 ? lane.stats : lane.fixedStats;
                tracks[part].push_back(stats.fits(length, block) ? stats.blocks()[index].sumSquares
                                                                 : 0.0);
            }
        }
        if (endBlock * block < range.end) {
            measureExactly({.first = endBlock * block, .end = range.end});
        }
    }
    spans.clear();
    for (const std::vector<double>& energies : tracks) {
        spans.emplace_back(energies);
    }
}

// The Compare part of a tempo-synced frame whose track lanes are already in `frameData`: the sum
// of the compared tracks as one more lane, then each track's match. Like the track lanes, the sum
// lane keeps its columns, and only those where compared tracks changed (moved by the largest
// correction while the fix is previewed) are summed again; all of them when a correction
// changes, spread over frames (see kSumRefreshSamples). The sums' levels per block are kept the
// same way, so the summing gains, measured with the matches or at most every 100 ms of audio,
// cost a pass over blocks. Matches are searched only when due (see kMatchRefreshSeconds).
void Analyzer::appendComparison(FrameHeaderValues& header, std::size_t first, std::size_t end,
                                std::uint64_t count, std::size_t columns, bool previewFix,
                                std::size_t playhead, double sampleRate)
{
    // Waits are measured in the capture's new audio, at its rate now.
    const double liveRate = engine.sampleRate();
    const auto maxOffset =
        static_cast<std::size_t>(std::lround(protocol::kMatchReachSeconds * sampleRate));
    const std::size_t block = statsBlock(sampleRate);
    const std::uint64_t now = engine.captureRing().endIndex();
    const auto length = static_cast<std::size_t>(count);
    const auto ownIndex =
        static_cast<std::size_t>(std::ranges::find(laneOrder, ownLane) - laneOrder.begin());

    // The match search: one track at a time, over the audio just before the playhead, continuing
    // at the right edge into the end of the previous pass when the playhead is near the left one
    // (the sweep's positions there are the moments just before), or over the newest part visible
    // when the playhead is elsewhere.
    const std::size_t windowSamples =
        std::min(static_cast<std::size_t>(std::lround(kMatchWindowSeconds * sampleRate)), length);
    const bool playheadShown = playhead > first && playhead < end;
    const std::size_t searchEnd = playheadShown ? playhead : end;
    const std::size_t searchCount =
        playheadShown ? windowSamples : std::min(windowSamples, end - first);
    // Positions [searchEnd - searchCount, searchEnd), the part before 0 from the right edge.
    const auto readSearch = [&](std::size_t index, std::span<float> into) {
        workDone.samplesSearched += into.size();
        const std::size_t wrapped = searchCount > searchEnd ? searchCount - searchEnd : 0;
        readLaneImage(index, length, length - wrapped, into.first(wrapped));
        readLaneImage(index, length, searchEnd + wrapped - searchCount, into.subspan(wrapped));
    };
    if (const auto lane = matchToSearch(header.laneSet, first, end, length, now, 0, 0.0, 1.0)) {
        matchOwn.resize(searchCount);
        matchTrack.resize(searchCount);
        readSearch(ownIndex, matchOwn);
        readSearch(*lane, matchTrack);
        ++workDone.matchSearches;
        matchSearch.start(matchOwn, matchTrack, maxOffset, block);
        searchingSerial = laneOrder[*lane];
        searchStartedAt = now;
    }
    const bool searched =
        spreadStepped ? std::exchange(searchEnded, false) : continueMatchSearch(liveRate);

    buildParts();
    // What the incremental sums were built from: a change of tracks, shifts or signs starts them
    // afresh.
    rawSumKey.assign({static_cast<std::int64_t>(length)});
    fixedSumKey.assign({static_cast<std::int64_t>(length)});
    for (const SumPart& fixed : fixedParts) {
        const auto serial = static_cast<std::int64_t>(laneOrder[fixed.lane]);
        rawSumKey.push_back(serial);
        fixedSumKey.insert(fixedSumKey.end(), {serial, fixed.offset, fixed.sign < 0.0f ? 1 : 0});
    }
    // Each track the fix shifts: its levels per block as shifted, which the fixed gain weighs the
    // fixed sum against. A track it does not shift (or no longer compared) keeps none pending, and
    // measures them all again once shifted: starting from its levels as recorded, which a shift of
    // at most kMatchReachSeconds barely changes, and redone over the next frames, as when its match
    // moves.
    const auto shiftedPart = [&](std::uint64_t serial) {
        const auto part = std::ranges::find_if(fixedParts, [&](const SumPart& candidate) {
            return laneOrder[candidate.lane] == serial;
        });
        return part == fixedParts.end() || part->offset == 0 ? nullptr : &*part;
    };
    for (const std::uint64_t serial : laneOrder) {
        Lane& lane = *lanes.at(serial);
        if (const SumPart* const part = shiftedPart(serial)) {
            lane.fixedKey.assign({static_cast<std::int64_t>(length), part->offset});
            if (!lane.fixedStats.fits(length, block) && lane.stats.fits(length, block)) {
                lane.fixedStats = lane.stats;
            }
        } else {
            lane.fixedRefresh = {};
            lane.fixedKey.clear();
            lane.lastFixedKey.clear();
        }
    }

    // The sum lane, with its parts as previewed. Its geometry: the span and visible part, and the
    // columns; its key: each part as summed.
    const std::vector<SumPart>& parts = previewFix ? fixedParts : rawParts;
    sumGeometry.assign({static_cast<std::int64_t>(length), static_cast<std::int64_t>(first),
                        static_cast<std::int64_t>(end), static_cast<std::int64_t>(columns)});
    sumKey.clear();
    for (const SumPart& part : parts) {
        sumKey.insert(sumKey.end(), {static_cast<std::int64_t>(laneOrder[part.lane]), part.offset,
                                     part.sign < 0.0f ? 1 : 0});
    }
    const std::size_t visibleCount = end - first;
    const bool sumRebuilt =
        sumGeometry != lastSumGeometry || !sumReduction.fits(visibleCount, columns);

    // The recomputations under way this frame, which share its kSumRefreshSamples, so that many
    // at once (every match moved) take no longer per frame than one.
    const auto pending = [&](const BlockStats& stats, const SumRefresh& refresh,
                             const std::vector<std::int64_t>& key,
                             const std::vector<std::int64_t>& lastKey) {
        return stats.fits(length, block) && (refresh.remaining > 0 || key != lastKey) ? 1U : 0U;
    };
    std::size_t shares = pending(rawSumStats, rawSumRefresh, rawSumKey, lastRawSumKey) +
                         pending(fixedSumStats, fixedSumRefresh, fixedSumKey, lastFixedSumKey);
    for (const std::uint64_t serial : laneOrder) {
        const Lane& lane = *lanes.at(serial);
        if (!lane.fixedKey.empty()) {
            shares += pending(lane.fixedStats, lane.fixedRefresh, lane.fixedKey, lane.lastFixedKey);
        }
    }
    if (!sumRebuilt && (sumRefresh.remaining > 0 || sumKey != lastSumKey)) {
        ++shares;
    }

    // Measured again beyond this frame's changes: the gains follow, so they settle as the
    // recomputation ends.
    const bool rawMeasured = updateSumStats(rawSumStats, rawSumRefresh, rawSumKey, lastRawSumKey,
                                            rawParts, 0, length, block, shares);
    bool fixedMeasured =
        updateSumStats(fixedSumStats, fixedSumRefresh, fixedSumKey, lastFixedSumKey, fixedParts,
                       maxOffset, length, block, shares);
    for (const std::uint64_t serial : laneOrder) {
        Lane& lane = *lanes.at(serial);
        if (lane.fixedKey.empty()) {
            continue;
        }
        onePart.assign(1, *shiftedPart(serial));
        fixedMeasured =
            updateSumStats(lane.fixedStats, lane.fixedRefresh, lane.fixedKey, lane.lastFixedKey,
                           onePart, maxOffset, length, block, shares) ||
            fixedMeasured;
    }

    // The matches' values and the gains, when due.
    const auto gainInterval =
        static_cast<std::uint64_t>(protocol::kReadoutIntervalSeconds * liveRate);
    if (searched || rawMeasured || fixedMeasured || !comparisonCache.valid ||
        gainsDueByTime(now, gainInterval)) {
        packMatches();
        // The sum as recorded over the visible part; the fix where every shifted track has
        // samples of the same moments as the others: not past the sweep's edges, nor where a shift
        // reaches across the playhead, beyond which the sweep holds the previous pass.
        const auto [earliest, latest] = fixedReach(fixedParts);
        const auto [fixedFirst, fixedEnd] = fixedPart(first, end, length, earliest, latest);
        std::array<Positions, 2> fixedRanges{Positions{.first = fixedFirst, .end = fixedEnd},
                                             Positions{.first = fixedEnd, .end = fixedEnd}};
        if (playhead <= length) {
            // Positions p whose shifted reads p + offset lie on the other side of the playhead.
            const auto seamFirst = static_cast<std::size_t>(
                std::max<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(playhead) - latest, 0));
            const std::size_t seamEnd =
                std::min(length, playhead + static_cast<std::size_t>(-earliest));
            fixedRanges[0].end = std::clamp(seamFirst, fixedFirst, fixedEnd);
            fixedRanges[1].first = std::clamp(seamEnd, fixedFirst, fixedEnd);
        }
        const std::array<Positions, 1> visible{Positions{.first = first, .end = end}};
        collectEnergies(visible, length, block, rawParts, rawSumStats, trackEnergies, energySpans,
                        sumEnergies);
        collectEnergies(fixedRanges, length, block, fixedParts, fixedSumStats, fixedTrackEnergies,
                        fixedEnergySpans, fixedSumEnergies);
        comparisonCache.sumGain =
            static_cast<float>(overlapGainOfBlocks(energySpans, sumEnergies, block));
        comparisonCache.fixedGain =
            static_cast<float>(overlapGainOfBlocks(fixedEnergySpans, fixedSumEnergies, block));
        comparisonCache.at = now;
        comparisonCache.valid = true;
    }

    // The sum lane's columns.
    const auto readVisibleSum = [&](std::size_t from, std::span<float> into) {
        workDone.samplesReduced += into.size();
        readSum(parts, length, first + from, into);
        return true;
    };
    if (sumRebuilt) {
        sumReduction.reset(visibleCount, columns);
        sumReduction.update(0, visibleCount, readVisibleSum);
        lastSumGeometry = sumGeometry;
        lastSumKey = sumKey;
        sumRefresh = {};
    } else {
        if (sumKey != lastSumKey) {
            // Other parts: every column again, over the next frames, from left to right.
            sumRefresh.remaining = visibleCount;
            sumRefresh.cursor = 0;
            lastSumKey = sumKey;
        }
        const std::size_t reach = previewFix ? maxOffset : 0;
        for (const SweepImage::Range& range : frameChanges) {
            const std::uint64_t from =
                std::max<std::uint64_t>(range.first > reach ? range.first - reach : 0, first);
            const std::uint64_t to = std::min<std::uint64_t>(range.end + reach, end);
            if (from < to) {
                sumReduction.update(static_cast<std::size_t>(from) - first,
                                    static_cast<std::size_t>(to) - first, readVisibleSum);
            }
        }
        continueRefresh(sumRefresh, visibleCount, refreshStep(parts.size(), shares),
                        [&](std::size_t from, std::size_t to) {
                            sumReduction.update(from, to, readVisibleSum);
                        });
    }
    frameData.insert(frameData.end(), sumReduction.values().begin(), sumReduction.values().end());
    frameData.insert(frameData.end(), comparisonCache.matches.begin(),
                     comparisonCache.matches.end());
    header.laneCount = static_cast<std::uint8_t>(laneOrder.size() + 1);
    header.flags = static_cast<std::uint8_t>(header.flags | flag(FrameFlag::kCompared));
    header.sumGain = comparisonCache.sumGain;
    header.fixedGain = comparisonCache.fixedGain;
}

template <typename Read>
bool Analyzer::addPartFrom(const Read& read, const SumPart& part, std::size_t position,
                           std::uint64_t count, std::span<float> into)
{
    const std::ptrdiff_t source = static_cast<std::ptrdiff_t>(position) + part.offset;
    const std::ptrdiff_t begin = std::max<std::ptrdiff_t>(0, -source);
    const std::ptrdiff_t stop = std::min<std::ptrdiff_t>(
        static_cast<std::ptrdiff_t>(into.size()), static_cast<std::ptrdiff_t>(count) - source);
    if (begin >= stop) {
        return true;
    }
    sumScratch.resize(static_cast<std::size_t>(stop - begin));
    workDone.samplesSummed += sumScratch.size();
    const std::ptrdiff_t from = source + begin;
    const bool held = read(part.lane, static_cast<std::uint64_t>(from), sumScratch);
    for (std::ptrdiff_t i = begin; i < stop; ++i) {
        into[static_cast<std::size_t>(i)] +=
            part.sign * sumScratch[static_cast<std::size_t>(i - begin)];
    }
    return held;
}

bool Analyzer::gainsDueByTime(std::uint64_t now, std::uint64_t interval) const
{
    // Frozen, the audio shown is held and does not change: gains measured there stay until the
    // view or the matches change.
    return !isFrozen && (now < comparisonCache.at || now - comparisonCache.at >= interval);
}

bool Analyzer::gainAudioReplaced() const
{
    // Resumed after the frames stopped for a while (a hidden page asks for none): a track's
    // capture may have moved past audio the gains need since they began.
    for (std::size_t index = 0; index < laneOrder.size(); ++index) {
        if (laneSources[index].first->beginIndex() > gainJob.needFrom[index]) {
            return true;
        }
    }
    return false;
}

bool Analyzer::stepGainJob()
{
    if (gainAudioReplaced()) {
        gainJob.active = false; // Measured again once due, rather than from silence.
        return false;
    }
    // Lane `index`'s Mid where the window was when the gains began.
    const auto readJobWindow = [&](std::size_t index, std::uint64_t from, std::span<float> into) {
        readScratch.resize(std::max(readScratch.size(), into.size()));
        return readSamplesHeld(
            *laneSources[index].first, lanes.at(laneOrder[index])->engine->inputChannels(),
            ChannelView::kMid, gainJob.laneStarts[index] + from, into, readScratch);
    };
    laneSamples.resize(laneOrder.size());
    fixedLanes.resize(laneOrder.size());
    // Reads the parts, each lane as recorded and as the fix shifts it, then measures the sum as
    // recorded, then fixed: as much as the frame's budget allows, so a short view is done at once,
    // as it was before it could be spread.
    std::size_t budget = kGainReadWork;
    while (gainJob.active && budget > 0) {
        if (gainJob.next < gainJob.raw.size()) {
            const std::size_t part = gainJob.next++;
            const std::size_t lane = gainJob.raw[part].lane;
            std::vector<float>& samples = laneSamples[lane];
            samples.resize(gainJob.visibleCount);
            readJobWindow(lane, gainJob.first, samples);
            std::vector<float>& fixed = fixedLanes[lane];
            fixed.assign(gainJob.fixedEnd - gainJob.fixedFirst, 0.0f);
            addPartFrom(readJobWindow, gainJob.fixed[part], gainJob.fixedFirst, gainJob.count,
                        fixed);
            budget -= std::min(budget, samples.size() + fixed.size());
            continue;
        }
        if (!gainJob.measuring) {
            std::vector<std::span<const float>>& tracks =
                gainJob.measured == 0 ? rawTracks : fixedTracks;
            tracks.clear();
            for (const SumPart& part : gainJob.raw) {
                tracks.emplace_back(gainJob.measured == 0 ? laneSamples[part.lane]
                                                          : fixedLanes[part.lane]);
            }
            gainJob.measure.start(tracks, gainJob.block);
            gainJob.measuring = true;
        }
        const std::uint64_t before = gainJob.measure.work();
        const bool ended = gainJob.measure.step(budget);
        const std::uint64_t passed = gainJob.measure.work() - before;
        workDone.gainWork += passed;
        budget -= static_cast<std::size_t>(std::min<std::uint64_t>(budget, passed));
        if (!ended) {
            continue;
        }
        gainJob.measuring = false;
        const auto gain = static_cast<float>(gainJob.measure.result());
        if (gainJob.measured == 0) {
            gainJob.sumGain = gain;
            gainJob.measured = 1;
        } else {
            comparisonCache.sumGain = gainJob.sumGain;
            comparisonCache.fixedGain = gain;
            comparisonCache.tracks = gainJob.tracks;
            // A match that ended meanwhile is measured next: due at once, frozen too.
            comparisonCache.valid = !gainJob.stale;
            comparisonCache.at = gainJob.at;
            gainJob.active = false;
            return true;
        }
    }
    return false;
}

bool Analyzer::stepSpreadWork(double liveRate)
{
    spreadStepped = true;
    bool ended = false;
    if (searchingSerial && continueMatchSearch(liveRate)) {
        searchEnded = true;
        ended = true;
        if (gainJob.active) {
            gainJob.stale = true; // As when the search ends in a frame drawn.
        }
    }
    if (gainJob.active && stepGainJob()) {
        ended = true;
    }
    return ended;
}

// A Compare frame with the seconds time base: every track's lane over the window, then the sum of
// the compared tracks, then each track's match against this track. Rolling, each lane and the
// sum keep their columns and reduce only those that scrolled in, the sum through a reader that
// adds the compared lanes (shifted and flipped as previewed); otherwise the visible part
// [first, end) is reduced afresh. Matches are searched one at a time, over the newest part of the
// visible part; the summing gains (over the visible part) and the readouts (over the whole
// window) are measured at most every 100 ms of audio, the gains over frames.
void Analyzer::renderCompare(FrameHeaderValues header, const Window& window, std::uint64_t start,
                             std::uint64_t sampleCount, std::size_t first, std::size_t end,
                             bool rolling, double sampleRate, const Commands& command,
                             std::vector<std::byte>& frame)
{
    // Waits are measured in the capture's new audio, at its rate now.
    const double liveRate = engine.sampleRate();
    const auto maxOffset =
        static_cast<std::size_t>(std::lround(protocol::kMatchReachSeconds * sampleRate));
    const std::size_t block = statsBlock(sampleRate);
    const std::size_t columns = command.columns;
    const bool previewFix = command.previewFix;
    const auto count = static_cast<std::size_t>(sampleCount);
    const std::size_t visibleCount = end - first;
    const auto ownIndex =
        static_cast<std::size_t>(std::ranges::find(laneOrder, ownLane) - laneOrder.begin());
    const std::uint64_t now = engine.captureRing().endIndex();

    // Lane `index`'s Mid over window samples from `from`, silent where its capture does not hold
    // them: false then (not captured yet, or any more).
    const auto readLaneWindow = [&](std::size_t index, std::uint64_t from, std::span<float> into) {
        const auto [source, laneStart] = laneSources[index];
        readScratch.resize(std::max(readScratch.size(), into.size()));
        return readSamplesHeld(*source, lanes.at(laneOrder[index])->engine->inputChannels(),
                               ChannelView::kMid, laneStart + from, into, readScratch);
    };

    const auto addPart = [&](const SumPart& part, std::size_t position, std::span<float> into) {
        return addPartFrom(readLaneWindow, part, position, count, into);
    };

    // The match search: one track at a time, over the newest part of the visible part, which
    // ends at its right edge.
    const auto windowSamples =
        static_cast<std::size_t>(std::lround(kMatchWindowSeconds * sampleRate));
    const std::size_t searchFirst = end - std::min(windowSamples, visibleCount);
    // The view as zoomed: a triggered window starts a different fraction of a sample in each
    // frame, which moves the samples shown by one now and then, and that is not another view.
    const Crop view = cropOf(static_cast<double>(count), 0.0, window.cropStart, window.cropEnd);
    if (const auto lane = matchToSearch(header.laneSet, view.first, view.end, count, now, start,
                                        window.viewStart, window.viewEnd)) {
        matchOwn.resize(end - searchFirst);
        matchTrack.resize(end - searchFirst);
        // A failed read (a capture overwritten during the copy, or not holding this audio yet)
        // keeps the match as it was rather than one from partly silent audio. It counts as this
        // track's turn, so the others are searched first, but is tried again soon rather than at
        // the next refresh.
        const auto readSearch = [&](std::size_t index, std::span<float> into) {
            workDone.samplesSearched += into.size();
            return readLaneWindow(index, searchFirst, into);
        };
        if (readSearch(ownIndex, matchOwn) && readSearch(*lane, matchTrack)) {
            ++workDone.matchSearches;
            matchSearch.start(matchOwn, matchTrack, maxOffset, block);
            searchingSerial = laneOrder[*lane];
            searchStartedAt = now;
        } else {
            MatchState& state = matchStates.try_emplace(laneOrder[*lane]).first->second;
            state.wait = static_cast<std::uint64_t>(std::lround(kMatchRetrySeconds * liveRate));
            state.analysedAt = now;
            // Frozen, no audio comes due again by time: due again at once, a few times.
            if (isFrozen && ++state.failedReads < kFrozenReadTries) {
                state.wait = 0;
                // A frame to try it in, though nothing else is due, and no audio to wait for.
                matchesPending = true;
                searchAtOnce = true;
            }
        }
    }
    const bool searched =
        spreadStepped ? std::exchange(searchEnded, false) : continueMatchSearch(liveRate);

    buildParts();

    // The matches' values when due, and the summing gains over the visible part: measured over
    // the next frames (kGainReadWork samples read in each), of the window as it was when they
    // began, so that no frame reads every compared track's whole view at once.
    const auto gainInterval =
        static_cast<std::uint64_t>(protocol::kReadoutIntervalSeconds * liveRate);
    if (gainJob.active && gainAudioReplaced()) {
        gainJob.active = false; // They begin afresh below.
    }
    if (searched && gainJob.active) {
        gainJob.stale = true;
    }
    if (searched || !comparisonCache.valid || gainsDueByTime(now, gainInterval)) {
        packMatches();
        if (!gainJob.active && !panning) {
            // The fix is measured where every shifted track has samples in the window.
            const auto [earliest, latest] = fixedReach(fixedParts);
            const auto [fixedFirst, fixedEnd] = fixedPart(first, end, count, earliest, latest);
            gainJob.active = true;
            gainJob.at = now;
            gainJob.sampleRate = sampleRate;
            gainJob.count = count;
            gainJob.block = block;
            gainJob.first = first;
            gainJob.visibleCount = visibleCount;
            gainJob.fixedFirst = fixedFirst;
            gainJob.fixedEnd = fixedEnd;
            gainJob.raw = rawParts;
            gainJob.fixed = fixedParts;
            gainJob.next = 0;
            gainJob.measured = 0;
            gainJob.measuring = false;
            // The oldest sample each lane's parts read, as recorded or shifted, or the oldest
            // its source holds now if later: audio it loses before the end is audio since replaced.
            const auto reach = static_cast<std::uint64_t>(std::max(-earliest, latest));
            const std::size_t oldest = std::min(first, fixedFirst);
            gainJob.laneStarts.clear();
            gainJob.needFrom.clear();
            for (const auto& [source, laneStart] : laneSources) {
                gainJob.laneStarts.push_back(laneStart);
                const std::uint64_t from = laneStart + oldest;
                gainJob.needFrom.push_back(
                    std::max(from - std::min(from, reach), source->beginIndex()));
            }
            gainJob.stale = false;
            gainJob.tracks.clear();
            for (const SumPart& part : rawParts) {
                gainJob.tracks.push_back(laneOrder[part.lane]);
            }
            if (!comparisonCache.valid && comparisonCache.tracks != gainJob.tracks) {
                // Nothing measured for these tracks yet: no value until it is. For the same
                // tracks over another view (while panning or zooming), or under other lanes'
                // labels, the last stays shown.
                comparisonCache.sumGain = std::numeric_limits<float>::quiet_NaN();
                comparisonCache.fixedGain = std::numeric_limits<float>::quiet_NaN();
            }
        }
    }
    if (gainJob.active && !spreadStepped) {
        (void)stepGainJob();
    }

    // The lanes' columns: rolling, each lane keeps its columns and reduces only those that
    // scrolled in, as in the multi-track view; otherwise the visible part is reduced afresh.
    frameData.clear();
    std::size_t pointCount = 0;
    const auto addColumns = [&](std::span<const float> values) {
        header.mode = protocol::FrameMode::kMinMax;
        pointCount = columns;
        frameData.insert(frameData.end(), values.begin(), values.end());
    };
    const auto addVisible = [&](std::span<const float> samples) {
        workDone.samplesReduced += samples.size();
        header.mode = reduceToColumns(samples, columns, reduced);
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : samples.size();
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    };
    for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
        RollColumns& roll = lanes.at(laneOrder[lane])->roll;
        if (rolling) {
            const std::uint64_t laneStart = laneSources[lane].second;
            addColumns(roll.update(laneStart - window.grid.boundary(window.firstColumn),
                                   window.firstColumn, window.grid,
                                   [&](std::uint64_t from, std::span<float> destination) {
                                       workDone.samplesReduced += destination.size();
                                       return readLaneWindow(lane, from - laneStart, destination);
                                   }));
            header.invalidLanes |= roll.holdsInvalid() ? laneBit(lane) : 0U;
            continue;
        }
        roll.clear();
        viewSamples.resize(visibleCount);
        readLaneWindow(lane, first, viewSamples);
        header.invalidLanes |= holdsInvalid(viewSamples) ? laneBit(lane) : 0U;
        addVisible(viewSamples);
    }
    header.invalidLanes |= sumLaneBit(header.invalidLanes);

    // The sum lane, its parts as previewed. Rolling, it keeps its columns too, through a reader
    // that adds the parts, for as long as the parts stay the same.
    const std::vector<SumPart>& parts = previewFix ? fixedParts : rawParts;
    sumKey.assign({static_cast<std::int64_t>(count), static_cast<std::int64_t>(columns)});
    for (const SumPart& part : parts) {
        sumKey.insert(sumKey.end(), {static_cast<std::int64_t>(laneOrder[part.lane]), part.offset,
                                     part.sign < 0.0f ? 1 : 0});
    }
    if (sumKey != lastSumKey) {
        sumRoll.clear();
        lastSumKey = sumKey;
    }
    // Positions position + i of the window show a part's sample at position + i + offset, and
    // silence beyond the window. False if a part's capture does not hold its samples yet.
    const auto readSumWindow = [&](std::size_t position, std::span<float> into) {
        std::ranges::fill(into, 0.0f);
        bool complete = true;
        for (const SumPart& part : parts) {
            complete = addPart(part, position, into) && complete;
        }
        return complete;
    };
    if (rolling) {
        addColumns(sumRoll.update(
            start - window.grid.boundary(window.firstColumn), window.firstColumn, window.grid,
            [&](std::uint64_t from, std::span<float> destination) {
                workDone.samplesReduced += destination.size();
                return readSumWindow(static_cast<std::size_t>(from - start), destination);
            }));
    } else {
        sumRoll.clear();
        sumSamples.resize(visibleCount);
        readSumWindow(first, sumSamples);
        addVisible(sumSamples);
    }
    frameData.insert(frameData.end(), comparisonCache.matches.begin(),
                     comparisonCache.matches.end());
    header.laneCount = static_cast<std::uint8_t>(laneOrder.size() + 1);
    header.flags = static_cast<std::uint8_t>(header.flags | flag(FrameFlag::kCompared));
    header.sumGain = comparisonCache.sumGain;
    header.fixedGain = comparisonCache.fixedGain;
    header.pointCount = static_cast<std::uint32_t>(pointCount);

    // Readouts measure this track's Mid over the whole span, at most every 100 ms of audio.
    const std::uint64_t ownEnd = engine.captureRing().endIndex();
    if (readoutsDue(ownEnd)) {
        const std::uint64_t readoutStart = window.readoutCount > 0 ? window.readoutStart : start;
        const std::uint64_t readoutCount =
            window.readoutCount > 0 ? window.readoutCount : sampleCount;
        viewSamples.resize(static_cast<std::size_t>(readoutCount));
        scratch.resize(std::max(scratch.size(), viewSamples.size()));
        if (readSamples(*laneSources[ownIndex].first, engine.inputChannels(), ChannelView::kMid,
                        readoutStart, viewSamples, scratch)) {
            storeReadouts(computeReadouts(viewSamples, sampleRate), ownEnd);
        }
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
}

} // namespace orizon::engine
