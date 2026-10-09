#include "engine/Analyzer.h"

#include "engine/ChannelViews.h"
#include "engine/FrameEncoder.h"
#include "engine/Readouts.h"
#include "engine/Reducer.h"
#include "engine/Roll.h"
#include "engine/Sweep.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <utility>

namespace orizon::engine {

using protocol::ChannelView;
using protocol::FrameFlag;
using protocol::SettingId;

namespace {

// The narrowest view accepted, matching the UI's 256-fold maximum zoom.
constexpr double kMinimumViewWidth = 1.0 / 512;

constexpr std::uint8_t flag(FrameFlag value)
{
    return static_cast<std::uint8_t>(value);
}

// The part of a window of `count` samples that the screen shows, as sample indices from its first
// sample, with one extra point beyond each edge so a raw trace reaches the screen edges. The
// window starts `fraction` of a sample after its first sample.
struct Crop
{
    std::size_t first = 0;
    std::size_t end = 0;
    double startFraction = 0.0;
    double visibleSamples = 0.0;
};

Crop cropOf(double count, double fraction, double cropStart, double cropEnd)
{
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
    double sumSquares = 0.0;
    for (std::size_t index = firstBlock; index < endBlock; ++index) {
        readouts.peak = std::max(readouts.peak, blocks[index].peak);
        sumSquares += blocks[index].sumSquares;
    }
    const std::size_t samples = std::min(endBlock * block, stats.sampleCount()) -
                                std::min(firstBlock * block, stats.sampleCount());
    if (samples > 0) {
        readouts.rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(samples)));
    }
    return readouts;
}

} // namespace

Analyzer::Analyzer(Engine& engine, const InstanceRegistry& registry)
    : engine(engine), registry(registry)
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

void Analyzer::setCompared(std::vector<std::uint64_t> ids)
{
    const std::scoped_lock lock(commandMutex);
    commands.compared = std::move(ids);
}

void Analyzer::setPreviewFix(bool on)
{
    const std::scoped_lock lock(commandMutex);
    commands.previewFix = on;
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
    const auto interval = static_cast<std::uint64_t>(0.1 * engine.sampleRate());
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
    header.peak = readoutCache.values.peak;
    header.rms = readoutCache.values.rms;
    header.frequency = static_cast<float>(readoutCache.values.frequencyHz.value_or(0.0));
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
    frozenStamp = engine.timeline().latest();
    frozenEnd = engine.captureRing().endIndex();
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
    Commands command = takeCommands();
    const Settings settings = engine.settings().get();
    frameSettings = settings;
    followTransport(settings, command);
    const double sampleRate = engine.sampleRate();
    if (command.armRequested) {
        trigger.arm();
    }
    const bool synced =
        settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase) == protocol::TimeBase::kBeats;
    // A freeze holds the newest audio the capture can spare (a margin for the audio thread,
    // which writes on), so that a longer beat span chosen while frozen has its audio.
    const CaptureRing& captureRing = engine.captureRing();
    const auto heldSamples =
        static_cast<std::uint64_t>(captureRing.capacity() - captureRing.capacity() / 8);
    if (command.frozen && !isFrozen && !freeze(sampleRate, synced ? heldSamples : 0)) {
        return false;
    }
    if (!command.frozen) {
        isFrozen = false;
    }
    if (command.columns == 0) {
        return false;
    }
    // Multi and Compare both draw lanes of the group's tracks.
    const bool multi = settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode) !=
                       protocol::DisplayMode::kSingle;
    if (multi && synced) {
        return produceLanes(settings, command, frame);
    }
    if (multi) {
        selectLanes(settings, command);
    } else {
        lanes.clear(); // Frees the lanes' sweeps and holds outside the multi-track view.
    }

    const double spanSeconds = settings.value(SettingId::kTimeSpan);
    const double span = spanSeconds * sampleRate;
    const bool roll = isRollSpan(spanSeconds);
    // Other lanes, or other tracks compared, change the frame too.
    const std::uint32_t laneSetId = multi ? laneSet().id : 0;
    const bool changed = settings != lastSettings || command.columns != lastColumns ||
                         command.panSeconds != lastPan || isFrozen != lastFrozen ||
                         command.viewStart != lastViewStart || command.viewEnd != lastViewEnd ||
                         laneSetId != lastLaneSet || command.previewFix != lastPreviewFix ||
                         (comparing && matchesPending);

    Window window{.span = span, .roll = roll};
    auto sampleCount = static_cast<std::uint64_t>(std::llround(span));
    double samplesPerColumn = span / static_cast<double>(command.columns);
    bool freezeAfterRender = false;
    const SampleSource* source =
        isFrozen ? static_cast<const SampleSource*>(&hold) : &engine.captureRing();

    if (synced) {
        // Tempo-synced sweep. Live, the capture's new samples are placed. Frozen, the sweep stays
        // as it is: a shorter span is cropped from it, and a longer one rebuilt from the held
        // audio, stepping down if it does not fit.
        const CaptureRing& ring = engine.captureRing();
        const std::uint64_t end = isFrozen ? frozenEnd : ring.endIndex();
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
            }
            sweep = frozen->window;
            window.subStart = frozen->start;
            window.subEnd = frozen->end;
        } else {
            const auto live = sweepWindow(engine.timeline().latest(), end, sampleRate, beatSpan,
                                          kMaxSweepSamples);
            if (!live || !image.update(ring, *live, false, channels)) {
                return false;
            }
            imageWindow = live;
            sweep = *live;
        }
        source = &image;
        window.start = 0.0;
        window.span = static_cast<double>(image.length());
        window.roll = false;
        window.synced = true;
        window.limited = sweep.limited;
        window.partial = !image.complete();
        window.cursor = sweep.cursor;
        window.cycleStart = sweep.cycleStart;
        window.spanQuarterNotes = sweep.spanQuarterNotes;
        window.barQuarterNotes = sweep.barQuarterNotes;
        window.cycleSeconds = sweep.cycleSeconds;
        sampleCount = image.length();
        samplesPerColumn = window.span / static_cast<double>(command.columns);
        lastEnd = end;

    } else if (isFrozen) {
        if (!changed) {
            return false;
        }
        // Keep the trigger point (triggered) or the right edge (roll) of the last live window
        // where it was, so changing the scale while frozen zooms around it.
        const auto held = static_cast<double>(hold.endIndex());
        const double anchor = lastLive
                                  ? (roll ? lastLive->start + lastLive->span
                                          : lastLive->start + kTriggerPosition * lastLive->span)
                                  : held;
        const double anchorOffset = roll || !lastLive ? span : kTriggerPosition * span;
        const double latestStart = held - span;
        const auto earliestStart = static_cast<double>(hold.beginIndex());
        window.start = std::clamp(anchor - anchorOffset - command.panSeconds * sampleRate,
                                  earliestStart, std::max(earliestStart, latestStart));
        window.triggered = lastLive && lastLive->triggered;
    } else if (roll) {
        // Zoomed, the roll window covers only the visible part, so its columns keep fixed
        // sample boundaries and do not shimmer; readouts still cover the whole span.
        const std::uint64_t end = engine.captureRing().endIndex();
        const RollWindow whole = rollWindow(end, span, command.columns);
        const auto visibleEnd = static_cast<std::uint64_t>(
            std::llround(static_cast<double>(end) - (1.0 - command.viewEnd) * span));
        const RollWindow rolled =
            rollWindow(std::min(visibleEnd, end), (command.viewEnd - command.viewStart) * span,
                       command.columns);
        window.start = static_cast<double>(rolled.start);
        window.cropStart = 0.0;
        window.cropEnd = 1.0;
        window.readoutStart = whole.start;
        window.readoutCount = whole.sampleCount;
        sampleCount = rolled.sampleCount;
        samplesPerColumn = static_cast<double>(rolled.samplesPerColumn);
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
        if (!readView(ring, engine.inputChannels(), triggerView, first, triggerSamples, scratch)) {
            return false;
        }
        const TriggerDecision decision = trigger.decide(
            triggerSamples, first, span, timeout, triggerParameters(settings, sampleRate),
            settings.enumValue<protocol::TriggerMode>(SettingId::kTriggerMode));
        if (decision.action == TriggerDecision::Action::kHold) {
            if (!changed || !lastShown || lastShown->roll) {
                return false;
            }
            window.start = lastShown->start;
            window.triggered = lastShown->triggered;
        } else {
            window.start = decision.windowStart;
            window.triggered = decision.triggered;
            freezeAfterRender = decision.freeze;
        }
    }

    window.viewStart = command.viewStart;
    window.viewEnd = command.viewEnd;
    if (!roll || window.synced || isFrozen) {
        const double shown = window.subEnd - window.subStart;
        window.cropStart = window.subStart + command.viewStart * shown;
        window.cropEnd = window.subStart + command.viewEnd * shown;
    }
    if (multi ? !renderLanes(*source, window, sampleCount, command, frame)
              : !render(*source, settings, sampleRate, window, sampleCount, samplesPerColumn,
                        command.columns, frame)) {
        return false;
    }
    if (!isFrozen) {
        lastLive = window;
    }
    lastShown = window;
    lastSettings = settings;
    lastColumns = command.columns;
    lastPan = command.panSeconds;
    lastViewStart = command.viewStart;
    lastViewEnd = command.viewEnd;
    lastFrozen = isFrozen;
    lastLaneSet = laneSetId;
    lastPreviewFix = command.previewFix;
    if (freezeAfterRender) {
        freeze(sampleRate);
        lastFrozen = isFrozen; // The frame just sent already shows the held window.
    }
    return true;
}

bool Analyzer::render(const SampleSource& source, const Settings& settings, double sampleRate,
                      const Window& window, std::uint64_t sampleCount, double samplesPerColumn,
                      std::size_t columns, std::vector<std::byte>& frame)
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
        .sequence = sequence,
        .flags = static_cast<std::uint8_t>((window.triggered ? flag(FrameFlag::kTriggered) : 0U) |
                                           (isFrozen ? flag(FrameFlag::kFrozen) : 0U) |
                                           (window.roll ? flag(FrameFlag::kRoll) : 0U) |
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
        .visibleSamples = static_cast<float>(visibleSamples)};
    frameData.clear();
    std::size_t pointCount = 0;
    // Each view keeps its columns from frame to frame where it can: a tempo-synced sweep reduces
    // again only the columns its newest samples landed in, a roll window only the columns that
    // scrolled in. A triggered window is new every time.
    const bool synced = window.synced && &source == &image;
    const bool rolling = window.roll && !window.synced && window.cropStart == 0.0 &&
                         window.cropEnd == 1.0 && columns > 0 && sampleCount % columns == 0 &&
                         sampleCount > columns;
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
        const auto read = [&](std::uint64_t first, std::span<float> destination) {
            readScratch.resize(std::max(readScratch.size(), destination.size()));
            return readView(source, inputChannels, view, first, destination, readScratch);
        };
        if (synced) {
            ColumnReduction& reduction = viewReductions.at(index);
            const auto readVisible = [&](std::size_t first, std::span<float> destination) {
                return read(crop.first + first, destination);
            };
            bool reduced = true;
            if (!sameReduction || !reduction.fits(visibleCount, columns)) {
                reduction.reset(visibleCount, columns);
                reduced = reduction.update(0, visibleCount, readVisible);
            } else {
                for (const SweepImage::Range& range : image.changes()) {
                    const std::uint64_t first = std::max<std::uint64_t>(range.first, crop.first);
                    const std::uint64_t end = std::min<std::uint64_t>(range.end, crop.end);
                    if (first < end) {
                        reduced = reduction.update(static_cast<std::size_t>(first) - crop.first,
                                                   static_cast<std::size_t>(end) - crop.first,
                                                   readVisible) &&
                                  reduced;
                    }
                }
            }
            if (!reduced) {
                reduction.invalidate();
                return false;
            }
            header.mode = reduction.mode();
            pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visibleCount;
            frameData.insert(frameData.end(), reduction.values().begin(), reduction.values().end());
            continue;
        }
        if (rolling) {
            const auto values =
                viewRolls.at(index).update(start, columns, sampleCount / columns, read);
            header.mode = protocol::FrameMode::kMinMax;
            pointCount = columns;
            frameData.insert(frameData.end(), values.begin(), values.end());
            continue;
        }
        // A window (at most a few seconds) is read whole; a sweep never is.
        viewSamples.resize(static_cast<std::size_t>(sampleCount));
        scratch.resize(std::max(scratch.size(), viewSamples.size()));
        if (!readView(source, inputChannels, view, start, viewSamples, scratch)) {
            return false;
        }
        const std::span<const float> visible =
            std::span(viewSamples).subspan(crop.first, crop.end - crop.first);
        header.mode = reduceToColumns(visible, columns, reduced);
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visible.size();
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    }
    if (synced) {
        // The trigger view's levels per block, for readouts, kept in step with the sweep.
        const bool fresh =
            !sweepStats.fits(static_cast<std::size_t>(sampleCount), statsBlock(sampleRate)) ||
            sweepStatsView != triggerView || sweepStatsChannels != inputChannels;
        sweepStatsView = triggerView;
        sweepStatsChannels = inputChannels;
        if (!updateStats(sweepStats, image, triggerView, inputChannels, fresh)) {
            return false;
        }
        image.clearChanges();
        viewReduction = {.first = crop.first, .channels = inputChannels, .views = views};
    } else {
        viewReduction = {};
    }
    if (rolling) {
        viewReduction.channels = inputChannels;
    }
    header.pointCount = static_cast<std::uint32_t>(pointCount);
    const double croppedSamplesPerColumn = window.cropEnd - window.cropStart < 1.0
                                               ? visibleSamples / static_cast<double>(columns)
                                               : samplesPerColumn;
    header.samplesPerPoint = header.mode == protocol::FrameMode::kMinMax
                                 ? static_cast<float>(croppedSamplesPerColumn)
                                 : 1.0f;

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
                    isFrozen ? hold.endIndex() : end, triggerView, inputChannels, readouts)) {
                storeReadouts(readouts, end);
            }
        } else {
            const std::uint64_t readoutStart =
                window.readoutCount > 0 ? window.readoutStart : start;
            const std::uint64_t readoutCount =
                window.readoutCount > 0 ? window.readoutCount : sampleCount;
            viewSamples.resize(static_cast<std::size_t>(readoutCount));
            scratch.resize(std::max(scratch.size(), viewSamples.size()));
            if (!readView(source, inputChannels, triggerView, readoutStart, viewSamples, scratch)) {
                return false;
            }
            storeReadouts(computeReadouts(viewSamples, sampleRate), end);
        }
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
    ++sequence;
    return true;
}

bool Analyzer::updateStats(BlockStats& stats, const SweepImage& image, ChannelView view,
                           int channels, bool fresh)
{
    const auto read = [&](std::size_t first, std::span<float> destination) {
        readScratch.resize(std::max(readScratch.size(), destination.size()));
        return readView(image, channels, view, first, destination, readScratch);
    };
    const auto length = static_cast<std::size_t>(image.length());
    if (fresh) {
        stats.reset(length, statsBlock(engine.sampleRate()));
        return stats.update(0, length, read);
    }
    bool measured = true;
    for (const SweepImage::Range& range : image.changes()) {
        measured = stats.update(static_cast<std::size_t>(range.first),
                                static_cast<std::size_t>(range.end), read) &&
                   measured;
    }
    return measured;
}

bool Analyzer::measureSweepReadouts(const BlockStats& stats, std::size_t first, std::size_t end,
                                    const SampleSource& live, std::uint64_t liveEnd,
                                    ChannelView view, int channels, Readouts& readouts)
{
    const double sampleRate = engine.sampleRate();
    const std::uint64_t begin = live.beginIndex();
    const std::uint64_t available = liveEnd > begin ? liveEnd - begin : 0;
    const std::size_t window = frequencyWindow(
        static_cast<std::size_t>(std::min<std::uint64_t>(available, end - first)), sampleRate);
    viewSamples.resize(window);
    scratch.resize(std::max(scratch.size(), viewSamples.size()));
    if (!readView(live, channels, view, liveEnd - window, viewSamples, scratch)) {
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
std::uint64_t Analyzer::selectLanes(const Settings& settings, const Commands& command)
{
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
        for (const GroupMember& member : members) {
            if (member.serial == ownSerial) {
                continue;
            }
            const bool asked =
                command.compared ? contains(*command.compared, member.serial) : compared.empty();
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
            lane->hold.capture(*lane->engine, ring.capacity() - ring.capacity() / 8, scratch);
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

Analyzer::Commands Analyzer::peekCommands()
{
    const std::scoped_lock lock(commandMutex);
    return commands;
}

bool Analyzer::placeLanes(const Settings& settings, protocol::BeatSpan span)
{
    bool fresh = false;
    for (const std::uint64_t serial : laneOrder) {
        Lane& lane = *lanes.at(serial);
        const CaptureRing& ring = lane.engine->captureRing();
        const std::uint64_t end = ring.endIndex();
        if (end != lane.end || !lane.window || settings != lastSettings) {
            const auto sweep = sweepWindow(lane.engine->timeline().latest(), end,
                                           lane.engine->sampleRate(), span, kMaxSweepSamples);
            if (sweep && lane.image.update(ring, *sweep, false, lane.engine->inputChannels())) {
                lane.window = sweep;
                fresh = true;
            }
            lane.end = end;
        }
    }
    return fresh;
}

void Analyzer::advance()
{
    const Settings settings = engine.settings().get();
    const bool synced =
        settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase) == protocol::TimeBase::kBeats;
    if (isFrozen || !synced) {
        return; // Frozen sweeps stay as they are; windows are placed when a frame is made.
    }
    const auto beatSpan = settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan);
    if (settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode) !=
        protocol::DisplayMode::kSingle) {
        selectLanes(settings, peekCommands());
        placeLanes(settings, beatSpan);
        return;
    }
    const CaptureRing& ring = engine.captureRing();
    const auto sweep = sweepWindow(engine.timeline().latest(), ring.endIndex(), engine.sampleRate(),
                                   beatSpan, kMaxSweepSamples);
    if (sweep && image.update(ring, *sweep, false, engine.inputChannels())) {
        imageWindow = sweep;
    }
}

// The multi-track view in Beats mode: one lane per track of the group, each the track's Mid as a
// persistent sweep on its own musical clock, so that lanes line up on bar lines. While frozen
// the lanes are not updated, so all of them hold the same moment.
bool Analyzer::produceLanes(const Settings& settings, const Commands& command,
                            std::vector<std::byte>& frame)
{
    const std::uint64_t ownSerial = selectLanes(settings, command);
    const std::uint32_t laneSetId = laneSet().id;

    if (image.length() > 0) {
        image.clear(); // Single mode's sweep is not needed here; free its memory.
        imageWindow.reset();
    }
    const auto beatSpan = settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan);
    const Lane& ownLane = *lanes.at(ownSerial);

    // Live, each lane follows its own capture and clock, and only new audio is placed. Frozen,
    // the lanes stay as they are; the own lane decides, as in Single mode, whether a span is
    // cropped from them or rebuilt from each lane's held audio.
    bool fresh = false;
    std::optional<SweepWindow> shown;
    double subStart = 0.0;
    double subEnd = 1.0;
    if (!isFrozen) {
        fresh = placeLanes(settings, beatSpan);
        shown = ownLane.window;
    } else if (ownLane.hold.held()) {
        const auto frozen =
            frozenSweep(ownLane.hold.clock(), ownLane.hold.endIndex(), ownLane.engine->sampleRate(),
                        beatSpan, ownLane.window, ownLane.image.length());
        if (frozen && frozen->rebuild) {
            for (const std::uint64_t serial : laneOrder) {
                Lane& lane = *lanes.at(serial);
                const LaneHold& hold = lane.hold;
                const auto held =
                    hold.held() ? sweepWindow(hold.clock(), hold.endIndex(),
                                              lane.engine->sampleRate(), beatSpan, kMaxSweepSamples)
                                : std::nullopt;
                if (held && lane.image.update(hold, *held, true, lane.engine->inputChannels())) {
                    lane.window = held;
                    fresh = true;
                }
            }
            shown = ownLane.window;
        } else if (frozen) {
            shown = frozen->window;
            subStart = frozen->start;
            subEnd = frozen->end;
        }
    } else {
        shown = ownLane.window;
    }
    if (!shown) {
        return false;
    }

    const bool changed = fresh || settings != lastSettings || command.columns != lastColumns ||
                         isFrozen != lastFrozen || command.viewStart != lastViewStart ||
                         command.viewEnd != lastViewEnd || laneSetId != lastLaneSet ||
                         command.previewFix != lastPreviewFix || (comparing && matchesPending);
    if (!changed) {
        return false;
    }

    // Lanes share the own track's window. A lane whose sweep differs in length for a moment (a
    // tempo change reaching it a block later) is drawn silent until it catches up.
    const SweepWindow& window = *shown;
    const std::uint64_t count = ownLane.image.length();
    const double shownPart = subEnd - subStart;
    const Crop crop =
        cropOf(static_cast<double>(count), 0.0, subStart + command.viewStart * shownPart,
               subStart + command.viewEnd * shownPart);
    const std::size_t columns = command.columns;
    const std::size_t block = statsBlock(engine.sampleRate());
    frameData.clear();
    FrameHeaderValues header{
        .sequence = sequence,
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
        .laneSet = laneSetId};
    // Each lane keeps its columns from frame to frame; only those holding samples placed since
    // the last frame are reduced again.
    const std::size_t visibleCount = crop.end - crop.first;
    std::size_t pointCount = 0;
    bool partial = false;
    frameChanges.clear();
    for (const std::uint64_t serial : laneOrder) {
        Lane& lane = *lanes.at(serial);
        ColumnReduction& reduction = lane.reduction;
        const int channels = lane.engine->inputChannels();
        const auto read = [&](std::size_t first, std::span<float> destination) {
            readScratch.resize(std::max(readScratch.size(), destination.size()));
            return readView(lane.image, lane.image.channels(), ChannelView::kMid,
                            crop.first + first, destination, readScratch);
        };
        bool reduced = false;
        if (lane.image.length() == count) {
            if (!reduction.fits(visibleCount, columns) || lane.reducedFirst != crop.first ||
                lane.reducedChannels != channels) {
                reduction.reset(visibleCount, columns);
                lane.reducedFirst = crop.first;
                lane.reducedChannels = channels;
                reduced = reduction.update(0, visibleCount, read);
            } else {
                reduced = true;
                for (const SweepImage::Range& range : lane.image.changes()) {
                    const std::uint64_t first = std::max<std::uint64_t>(range.first, crop.first);
                    const std::uint64_t end = std::min<std::uint64_t>(range.end, crop.end);
                    if (first < end) {
                        reduced =
                            reduction.update(static_cast<std::size_t>(first) - crop.first,
                                             static_cast<std::size_t>(end) - crop.first, read) &&
                            reduced;
                    }
                }
            }
            // The lane's levels per block, for readouts and summing gains.
            updateStats(lane.stats, lane.image, ChannelView::kMid, lane.image.channels(),
                        !lane.stats.fits(static_cast<std::size_t>(count), block) ||
                            lane.statsChannels != channels);
            lane.statsChannels = channels;
            partial = partial || !lane.image.complete();
            if (comparing && lane.compared) {
                frameChanges.insert(frameChanges.end(), lane.image.changes().begin(),
                                    lane.image.changes().end());
            }
            lane.image.clearChanges();
        }
        if (!reduced) {
            // Not on this sweep's length (yet): silent, and reduced afresh once it is.
            reduction.reset(visibleCount, columns);
            reduction.invalidate();
        }
        header.mode = reduction.mode();
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visibleCount;
        frameData.insert(frameData.end(), reduction.values().begin(), reduction.values().end());
    }
    header.pointCount = static_cast<std::uint32_t>(pointCount);
    header.samplesPerPoint =
        header.mode == protocol::FrameMode::kMinMax
            ? static_cast<float>(crop.visibleSamples / static_cast<double>(columns))
            : 1.0f;

    if (partial) {
        header.flags = static_cast<std::uint8_t>(header.flags | flag(FrameFlag::kPartial));
    }
    if (comparing) {
        const auto playhead = static_cast<std::size_t>(
            std::llround((subStart + window.cursor * shownPart) * static_cast<double>(count)));
        appendComparison(header, crop.first, crop.end, count, columns, command.previewFix,
                         playhead);
    }

    // Readouts measure the own track's Mid: peak and RMS over the whole span from its levels per
    // block, the frequency over the newest second, at most every 100 ms of audio.
    const std::uint64_t ownEnd = ownLane.engine->captureRing().endIndex();
    if (readoutsDue(ownEnd) && ownLane.stats.fits(static_cast<std::size_t>(count), block)) {
        Readouts readouts;
        if (measureSweepReadouts(
                ownLane.stats,
                static_cast<std::size_t>(std::llround(subStart * static_cast<double>(count))),
                static_cast<std::size_t>(std::llround(subEnd * static_cast<double>(count))),
                isFrozen ? static_cast<const SampleSource&>(ownLane.hold)
                         : ownLane.engine->captureRing(),
                isFrozen ? ownLane.hold.endIndex() : ownEnd, ChannelView::kMid,
                engine.inputChannels(), readouts)) {
            storeReadouts(readouts, ownEnd);
        }
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
    ++sequence;
    lastSettings = settings;
    lastColumns = command.columns;
    lastViewStart = command.viewStart;
    lastViewEnd = command.viewEnd;
    lastFrozen = isFrozen;
    lastLaneSet = laneSetId;
    lastPreviewFix = command.previewFix;
    return true;
}

// The multi-track view with the seconds time base: every lane shows this track's window (its
// trigger, roll or frozen window), placed in each other track's capture through the musical
// clock that all instances share with the host.
bool Analyzer::renderLanes(const SampleSource& ownSource, const Window& window,
                           std::uint64_t sampleCount, const Commands& command,
                           std::vector<std::byte>& frame)
{
    const std::size_t columns = command.columns;
    const double sampleRate = engine.sampleRate();
    const double first = std::floor(window.start);
    const auto start = static_cast<std::uint64_t>(std::max(first, 0.0));
    const MusicalStamp ownClock = isFrozen ? frozenStamp : engine.timeline().latest();
    const double startQuarterNotes = Timeline::quarterNotesAt(ownClock, start, sampleRate);
    const Crop crop = cropOf(static_cast<double>(sampleCount), window.start - first,
                             window.cropStart, window.cropEnd);
    viewSamples.resize(static_cast<std::size_t>(sampleCount));
    scratch.resize(std::max(scratch.size(), viewSamples.size()));

    FrameHeaderValues header{
        .sequence = sequence,
        .flags = static_cast<std::uint8_t>((window.triggered ? flag(FrameFlag::kTriggered) : 0U) |
                                           (isFrozen ? flag(FrameFlag::kFrozen) : 0U) |
                                           (window.roll ? flag(FrameFlag::kRoll) : 0U)),
        .laneCount = static_cast<std::uint8_t>(laneOrder.size()),
        .startFraction = static_cast<float>(crop.startFraction),
        .viewStart = static_cast<float>(window.viewStart),
        .viewEnd = static_cast<float>(window.viewEnd),
        .visibleSamples = static_cast<float>(crop.visibleSamples),
        .laneSet = laneSet().id};
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
        return {
            source,
            static_cast<std::uint64_t>(std::max(
                std::round(Timeline::sampleIndexAt(clock, startQuarterNotes, sampleRate)), 0.0))};
    };
    // A whole roll window falls into columns on fixed sample indices: each lane keeps its columns
    // and reduces only the new ones.
    const bool rolling = window.roll && window.cropStart == 0.0 && window.cropEnd == 1.0 &&
                         columns > 0 && sampleCount % columns == 0 && sampleCount > columns;
    if (comparing) {
        laneSources.clear();
        for (const std::uint64_t serial : laneOrder) {
            laneSources.push_back(sourceOf(serial));
        }
        renderCompare(header, window, start, sampleCount, crop.first, crop.end, rolling, command,
                      frame);
        return true;
    }
    frameData.clear();
    std::size_t pointCount = 0;
    for (const std::uint64_t serial : laneOrder) {
        Lane& lane = *lanes.at(serial);
        const auto [source, laneStart] = sourceOf(serial);
        const int channels = lane.engine->inputChannels();
        if (rolling) {
            const auto values = lane.roll.update(
                laneStart, columns, sampleCount / columns,
                [&](std::uint64_t first, std::span<float> destination) {
                    readScratch.resize(std::max(readScratch.size(), destination.size()));
                    return readView(*source, channels, ChannelView::kMid, first, destination,
                                    readScratch);
                });
            header.mode = protocol::FrameMode::kMinMax;
            pointCount = columns;
            frameData.insert(frameData.end(), values.begin(), values.end());
            continue;
        }
        lane.roll.clear();
        if (!readView(*source, channels, ChannelView::kMid, laneStart, viewSamples, scratch)) {
            std::ranges::fill(viewSamples, 0.0f); // Not captured (yet, or any more).
        }
        const std::span<const float> visible =
            std::span(viewSamples).subspan(crop.first, crop.end - crop.first);
        header.mode = reduceToColumns(visible, columns, reduced);
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visible.size();
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    }
    header.pointCount = static_cast<std::uint32_t>(pointCount);
    header.samplesPerPoint =
        header.mode == protocol::FrameMode::kMinMax
            ? static_cast<float>(crop.visibleSamples / static_cast<double>(columns))
            : 1.0f;

    // Readouts measure this track's Mid over the whole span, at most every 100 ms of audio.
    const std::uint64_t ownEnd = engine.captureRing().endIndex();
    if (readoutsDue(ownEnd)) {
        const std::uint64_t readoutStart = window.readoutCount > 0 ? window.readoutStart : start;
        const std::uint64_t readoutCount =
            window.readoutCount > 0 ? window.readoutCount : sampleCount;
        viewSamples.resize(static_cast<std::size_t>(readoutCount));
        scratch.resize(std::max(scratch.size(), viewSamples.size()));
        if (readView(ownSource, engine.inputChannels(), ChannelView::kMid, readoutStart,
                     viewSamples, scratch)) {
            storeReadouts(computeReadouts(viewSamples, sampleRate), ownEnd);
        }
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
    ++sequence;
    return true;
}

// The compared track whose match to search this frame: a track without a match first, else the
// one with the oldest match that is due, provided kMatchSpacingSeconds of audio passed since the
// last search. A change of the tracks compared, the view or the span makes every match due, but
// keeps it on show until its search. Notes whether matches remain to search.
std::optional<std::size_t> Analyzer::matchToSearch(std::uint32_t laneSet, std::size_t first,
                                                   std::size_t end, std::size_t length,
                                                   std::uint64_t now)
{
    if (laneSet != matchLaneSet || first != matchFirst || end != matchEnd || length != matchCount) {
        for (auto& [serial, state] : matchStates) {
            state.analysedAt = 0;
        }
        comparisonCache.valid = false;
        matchLaneSet = laneSet;
        matchFirst = first;
        matchEnd = end;
        matchCount = length;
    }
    std::erase_if(matchStates, [&](const auto& entry) {
        return std::ranges::find(laneOrder, entry.first) == laneOrder.end();
    });
    const double sampleRate = engine.sampleRate();
    const auto refresh = static_cast<std::uint64_t>(std::lround(kMatchRefreshSeconds * sampleRate));
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
        } else if (now - found->second.analysedAt >= refresh) {
            ++candidates;
            if (!missing && (!chosen || found->second.analysedAt < chosenAt)) {
                chosen = lane;
                chosenAt = found->second.analysedAt;
            }
        }
    }
    const auto spacing = static_cast<std::uint64_t>(std::lround(kMatchSpacingSeconds * sampleRate));
    if (!chosen || (!missing && now - lastMatchSearch < spacing)) {
        matchesPending = chosen.has_value();
        return std::nullopt;
    }
    matchesPending = candidates > 1;
    lastMatchSearch = now;
    return chosen;
}

void Analyzer::readLaneImage(std::size_t index, std::size_t length, std::uint64_t from,
                             std::span<float> into)
{
    const Lane& lane = *lanes.at(laneOrder[index]);
    readScratch.resize(std::max(readScratch.size(), into.size()));
    if (lane.image.length() != length ||
        !readView(lane.image, lane.image.channels(), ChannelView::kMid, from, into, readScratch)) {
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
        const std::ptrdiff_t from = source + begin;
        readLaneImage(part.lane, length, static_cast<std::uint64_t>(from), sumScratch);
        for (std::ptrdiff_t i = begin; i < stop; ++i) {
            into[static_cast<std::size_t>(i)] +=
                part.sign * sumScratch[static_cast<std::size_t>(i - begin)];
        }
    }
}

void Analyzer::updateSumStats(BlockStats& stats, const std::vector<std::int64_t>& key,
                              std::vector<std::int64_t>& lastKey, const std::vector<SumPart>& parts,
                              std::size_t reach, std::size_t length, std::size_t block)
{
    const auto read = [&](std::size_t from, std::span<float> into) {
        readSum(parts, length, from, into);
        return true;
    };
    if (key != lastKey || !stats.fits(length, block)) {
        stats.reset(length, block);
        stats.update(0, length, read);
        lastKey = key;
        return;
    }
    for (const SweepImage::Range& range : frameChanges) {
        const auto from = static_cast<std::size_t>(range.first > reach ? range.first - reach : 0);
        const auto to =
            static_cast<std::size_t>(std::min<std::uint64_t>(range.end + reach, length));
        if (from < to) {
            stats.update(from, to, read);
        }
    }
}

void Analyzer::collectEnergies(std::size_t first, std::size_t end, std::size_t length,
                               std::size_t block)
{
    const std::size_t firstBlock = (first + block - 1) / block;
    const std::size_t endBlock = end / block;
    const bool wholeBlocks = endBlock > firstBlock;
    // The pieces of blocks at the edges of the visible part, measured from their samples.
    struct Piece
    {
        std::size_t first = 0;
        std::size_t end = 0;
    };
    std::array<Piece, 2> pieces{};
    std::size_t pieceCount = 0;
    if (!wholeBlocks) {
        pieces[pieceCount++] = {.first = first, .end = end};
    } else {
        if (first < firstBlock * block) {
            pieces[pieceCount++] = {.first = first, .end = firstBlock * block};
        }
        if (endBlock * block < end) {
            pieces[pieceCount++] = {.first = endBlock * block, .end = end};
        }
    }
    const auto appendPieces = [&](std::vector<double>& into, const auto& read) {
        for (std::size_t piece = 0; piece < pieceCount; ++piece) {
            pieceScratch.resize(pieces[piece].end - pieces[piece].first);
            read(pieces[piece].first, std::span<float>(pieceScratch));
            into.push_back(measureBlock(pieceScratch).sumSquares);
        }
    };
    trackEnergies.resize(rawParts.size());
    energySpans.clear();
    for (std::size_t part = 0; part < rawParts.size(); ++part) {
        const std::size_t lane = rawParts[part].lane;
        const Lane& track = *lanes.at(laneOrder[lane]);
        std::vector<double>& energies = trackEnergies[part];
        energies.clear();
        const bool measured = track.stats.fits(length, block);
        if (wholeBlocks) {
            for (std::size_t index = firstBlock; index < endBlock; ++index) {
                energies.push_back(measured ? track.stats.blocks()[index].sumSquares : 0.0);
            }
        }
        appendPieces(energies, [&](std::size_t from, std::span<float> into) {
            readLaneImage(lane, length, from, into);
        });
    }
    for (const std::vector<double>& energies : trackEnergies) {
        energySpans.emplace_back(energies);
    }
    const auto collectSum = [&](const BlockStats& stats, const std::vector<SumPart>& parts,
                                std::vector<double>& into) {
        into.clear();
        if (wholeBlocks) {
            for (std::size_t index = firstBlock; index < endBlock; ++index) {
                into.push_back(stats.blocks()[index].sumSquares);
            }
        }
        appendPieces(into, [&](std::size_t from, std::span<float> piece) {
            readSum(parts, length, from, piece);
        });
    };
    collectSum(rawSumStats, rawParts, sumEnergies);
    collectSum(fixedSumStats, fixedParts, fixedSumEnergies);
}

// The Compare part of a tempo-synced frame whose track lanes are already in `frameData`: the sum
// of the compared tracks as one more lane, then each track's match. Like the track lanes, the sum
// lane keeps its columns, and only those where compared tracks changed (moved by the largest
// correction while the fix is previewed) are summed again; all of them when a correction
// changes. The sums' levels per block are kept the same way, so the summing gains, measured with
// the matches or at most every 100 ms of audio, cost a pass over blocks. Matches are searched
// only when due (see kMatchRefreshSeconds).
void Analyzer::appendComparison(FrameHeaderValues& header, std::size_t first, std::size_t end,
                                std::uint64_t count, std::size_t columns, bool previewFix,
                                std::size_t playhead)
{
    const double sampleRate = engine.sampleRate();
    const auto maxOffset = static_cast<std::size_t>(std::lround(0.01 * sampleRate));
    const std::size_t block = statsBlock(sampleRate);
    const std::uint64_t now = engine.captureRing().endIndex();
    const auto length = static_cast<std::size_t>(count);
    const auto ownIndex =
        static_cast<std::size_t>(std::ranges::find(laneOrder, ownLane) - laneOrder.begin());

    // The match search: one track per frame, over the audio just before the playhead, continuing
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
        const std::size_t wrapped = searchCount > searchEnd ? searchCount - searchEnd : 0;
        readLaneImage(index, length, length - wrapped, into.first(wrapped));
        readLaneImage(index, length, searchEnd + wrapped - searchCount, into.subspan(wrapped));
    };
    bool searched = false;
    if (const auto lane = matchToSearch(header.laneSet, first, end, length, now)) {
        matchOwn.resize(searchCount);
        matchTrack.resize(searchCount);
        readSearch(ownIndex, matchOwn);
        readSearch(*lane, matchTrack);
        matchStates[laneOrder[*lane]] = {
            .analysis = analysePair(matchOwn, matchTrack, maxOffset, block, scratch),
            .analysedAt = now};
        searched = true;
    }

    // The compared tracks as parts of the sum: as recorded, and shifted and flipped as matched.
    rawParts.clear();
    fixedParts.clear();
    rawSumKey.assign({static_cast<std::int64_t>(length)});
    fixedSumKey.assign({static_cast<std::int64_t>(length)});
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
        rawSumKey.push_back(static_cast<std::int64_t>(serial));
        fixedSumKey.insert(fixedSumKey.end(), {static_cast<std::int64_t>(serial), fixed.offset,
                                               fixed.sign < 0.0f ? 1 : 0});
    }
    updateSumStats(rawSumStats, rawSumKey, lastRawSumKey, rawParts, 0, length, block);
    updateSumStats(fixedSumStats, fixedSumKey, lastFixedSumKey, fixedParts, maxOffset, length,
                   block);

    // The matches' values and the gains, when due.
    const auto gainInterval = static_cast<std::uint64_t>(0.1 * sampleRate);
    if (searched || !comparisonCache.valid || now < comparisonCache.at ||
        now - comparisonCache.at >= gainInterval) {
        std::vector<float>& matchValues = comparisonCache.matches;
        matchValues.assign(laneOrder.size() * protocol::kMatchValues, 0.0f);
        for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
            const std::uint64_t serial = laneOrder[lane];
            const auto found = matchStates.find(serial);
            if (!lanes.at(serial)->compared || found == matchStates.end()) {
                continue; // This track, and tracks not compared, have zeros.
            }
            const PairAnalysis& match = found->second.analysis;
            float* const values = matchValues.data() + lane * protocol::kMatchValues;
            values[0] = static_cast<float>(match.correlation);
            values[1] = static_cast<float>(match.offset);
            values[2] = !match.related ? 0.0f : match.inverted ? -1.0f : 1.0f;
        }
        collectEnergies(first, end, length, block);
        comparisonCache.sumGain = static_cast<float>(overlapGainOfBlocks(energySpans, sumEnergies));
        comparisonCache.fixedGain =
            static_cast<float>(overlapGainOfBlocks(energySpans, fixedSumEnergies));
        comparisonCache.at = now;
        comparisonCache.valid = true;
    }

    // The sum lane, with its parts as previewed. Its key: the span and visible part, the columns,
    // and each part as summed.
    const std::vector<SumPart>& parts = previewFix ? fixedParts : rawParts;
    sumKey.assign({static_cast<std::int64_t>(length), static_cast<std::int64_t>(first),
                   static_cast<std::int64_t>(end), static_cast<std::int64_t>(columns)});
    for (const SumPart& part : parts) {
        sumKey.insert(sumKey.end(), {static_cast<std::int64_t>(laneOrder[part.lane]), part.offset,
                                     part.sign < 0.0f ? 1 : 0});
    }
    const auto readVisibleSum = [&](std::size_t from, std::span<float> into) {
        readSum(parts, length, first + from, into);
        return true;
    };
    const std::size_t visibleCount = end - first;
    if (sumKey != lastSumKey || !sumReduction.fits(visibleCount, columns)) {
        sumReduction.reset(visibleCount, columns);
        sumReduction.update(0, visibleCount, readVisibleSum);
        lastSumKey = sumKey;
    } else {
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
    }
    frameData.insert(frameData.end(), sumReduction.values().begin(), sumReduction.values().end());
    frameData.insert(frameData.end(), comparisonCache.matches.begin(),
                     comparisonCache.matches.end());
    header.laneCount = static_cast<std::uint8_t>(laneOrder.size() + 1);
    header.flags = static_cast<std::uint8_t>(header.flags | flag(FrameFlag::kCompared));
    header.sumGain = comparisonCache.sumGain;
    header.fixedGain = comparisonCache.fixedGain;
}

// A Compare frame with the seconds time base: every track's lane over the window, then the sum of
// the compared tracks, then each track's match against this track. Rolling, each lane and the
// sum keep their columns and reduce only those that scrolled in, the sum through a reader that
// adds the compared lanes (shifted and flipped as previewed); otherwise the visible part
// [first, end) is reduced afresh. Matches are searched one per frame, over the newest part of the
// visible part; the summing gains (over the visible part) and the readouts (over the whole
// window) are measured at most every 100 ms of audio.
void Analyzer::renderCompare(FrameHeaderValues header, const Window& window, std::uint64_t start,
                             std::uint64_t sampleCount, std::size_t first, std::size_t end,
                             bool rolling, const Commands& command, std::vector<std::byte>& frame)
{
    const double sampleRate = engine.sampleRate();
    const auto maxOffset = static_cast<std::size_t>(std::lround(0.01 * sampleRate));
    const std::size_t block = statsBlock(sampleRate);
    const std::size_t columns = command.columns;
    const bool previewFix = command.previewFix;
    const auto count = static_cast<std::size_t>(sampleCount);
    const std::size_t visibleCount = end - first;
    const auto ownIndex =
        static_cast<std::size_t>(std::ranges::find(laneOrder, ownLane) - laneOrder.begin());
    const std::uint64_t now = engine.captureRing().endIndex();

    // Lane `index`'s Mid over window samples from `from`: false, and silent, where its capture
    // does not hold them (yet).
    const auto readLaneWindow = [&](std::size_t index, std::uint64_t from, std::span<float> into) {
        const auto [source, laneStart] = laneSources[index];
        readScratch.resize(std::max(readScratch.size(), into.size()));
        if (readView(*source, lanes.at(laneOrder[index])->engine->inputChannels(),
                     ChannelView::kMid, laneStart + from, into, readScratch)) {
            return true;
        }
        std::ranges::fill(into, 0.0f);
        return false;
    };

    // The match search: one track per frame, over the newest part of the visible part, which
    // ends at its right edge.
    const auto windowSamples =
        static_cast<std::size_t>(std::lround(kMatchWindowSeconds * sampleRate));
    const std::size_t searchFirst = end - std::min(windowSamples, visibleCount);
    bool searched = false;
    if (const auto lane = matchToSearch(header.laneSet, first, end, count, now)) {
        matchOwn.resize(end - searchFirst);
        matchTrack.resize(end - searchFirst);
        readLaneWindow(ownIndex, searchFirst, matchOwn);
        readLaneWindow(*lane, searchFirst, matchTrack);
        matchStates[laneOrder[*lane]] = {
            .analysis = analysePair(matchOwn, matchTrack, maxOffset, block, scratch),
            .analysedAt = now};
        searched = true;
    }

    // The compared tracks as parts of the sum: as recorded, and shifted and flipped as matched.
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

    // The matches' values and the gains, when due, over the visible part.
    const auto gainInterval = static_cast<std::uint64_t>(0.1 * sampleRate);
    if (searched || !comparisonCache.valid || now < comparisonCache.at ||
        now - comparisonCache.at >= gainInterval) {
        std::vector<float>& matchValues = comparisonCache.matches;
        matchValues.assign(laneOrder.size() * protocol::kMatchValues, 0.0f);
        for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
            const std::uint64_t serial = laneOrder[lane];
            const auto found = matchStates.find(serial);
            if (!lanes.at(serial)->compared || found == matchStates.end()) {
                continue; // This track, and tracks not compared, have zeros.
            }
            const PairAnalysis& match = found->second.analysis;
            float* const values = matchValues.data() + lane * protocol::kMatchValues;
            values[0] = static_cast<float>(match.correlation);
            values[1] = static_cast<float>(match.offset);
            values[2] = !match.related ? 0.0f : match.inverted ? -1.0f : 1.0f;
        }
        laneSamples.resize(laneOrder.size());
        fixedLanes.resize(laneOrder.size());
        rawTracks.clear();
        fixedTracks.clear();
        for (std::size_t part = 0; part < rawParts.size(); ++part) {
            const std::size_t lane = rawParts[part].lane;
            std::vector<float>& samples = laneSamples[lane];
            samples.resize(visibleCount);
            readLaneWindow(lane, first, samples);
            std::vector<float>& fixed = fixedLanes[lane];
            fixed.resize(visibleCount);
            applyFix(samples, fixedParts[part].offset, fixedParts[part].sign < 0.0f, fixed);
            rawTracks.emplace_back(samples);
            fixedTracks.emplace_back(fixed);
        }
        comparisonCache.sumGain = static_cast<float>(overlapGain(rawTracks, block));
        comparisonCache.fixedGain = static_cast<float>(overlapGain(fixedTracks, block));
        comparisonCache.at = now;
        comparisonCache.valid = true;
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
        header.mode = reduceToColumns(samples, columns, reduced);
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : samples.size();
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    };
    for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
        RollColumns& roll = lanes.at(laneOrder[lane])->roll;
        if (rolling) {
            const std::uint64_t laneStart = laneSources[lane].second;
            addColumns(roll.update(laneStart, columns, count / columns,
                                   [&](std::uint64_t from, std::span<float> destination) {
                                       return readLaneWindow(lane, from - laneStart, destination);
                                   }));
            continue;
        }
        roll.clear();
        viewSamples.resize(visibleCount);
        readLaneWindow(lane, first, viewSamples);
        addVisible(viewSamples);
    }

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
            const std::ptrdiff_t source = static_cast<std::ptrdiff_t>(position) + part.offset;
            const std::ptrdiff_t begin = std::max<std::ptrdiff_t>(0, -source);
            const std::ptrdiff_t stop =
                std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(into.size()),
                                         static_cast<std::ptrdiff_t>(count) - source);
            if (begin >= stop) {
                continue;
            }
            sumScratch.resize(static_cast<std::size_t>(stop - begin));
            const std::ptrdiff_t from = source + begin;
            complete =
                readLaneWindow(part.lane, static_cast<std::uint64_t>(from), sumScratch) && complete;
            for (std::ptrdiff_t i = begin; i < stop; ++i) {
                into[static_cast<std::size_t>(i)] +=
                    part.sign * sumScratch[static_cast<std::size_t>(i - begin)];
            }
        }
        return complete;
    };
    if (rolling) {
        addColumns(sumRoll.update(
            start, columns, count / columns, [&](std::uint64_t from, std::span<float> destination) {
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
    header.samplesPerPoint = header.mode == protocol::FrameMode::kMinMax
                                 ? static_cast<float>(static_cast<double>(header.visibleSamples) /
                                                      static_cast<double>(columns))
                                 : 1.0f;

    // Readouts measure this track's Mid over the whole span, at most every 100 ms of audio.
    const std::uint64_t ownEnd = engine.captureRing().endIndex();
    if (readoutsDue(ownEnd)) {
        const std::uint64_t readoutStart = window.readoutCount > 0 ? window.readoutStart : start;
        const std::uint64_t readoutCount =
            window.readoutCount > 0 ? window.readoutCount : sampleCount;
        viewSamples.resize(static_cast<std::size_t>(readoutCount));
        scratch.resize(std::max(scratch.size(), viewSamples.size()));
        if (readView(*laneSources[ownIndex].first, engine.inputChannels(), ChannelView::kMid,
                     readoutStart, viewSamples, scratch)) {
            storeReadouts(computeReadouts(viewSamples, sampleRate), ownEnd);
        }
    }
    applyReadouts(header);

    encodeFrame(header, frameData, frame);
    ++sequence;
}

} // namespace orizon::engine
