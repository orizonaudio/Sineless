#include "engine/Analyzer.h"

#include "engine/ChannelViews.h"
#include "engine/FrameEncoder.h"
#include "engine/Readouts.h"
#include "engine/Reducer.h"
#include "engine/Roll.h"
#include "engine/Sweep.h"

#include <algorithm>
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

TriggerParameters triggerParameters(const Settings& settings, double sampleRate)
{
    return {.level = settings.value(SettingId::kTriggerLevel),
            .hysteresis = settings.value(SettingId::kTriggerHysteresis),
            .slope = settings.enumValue<protocol::TriggerSlope>(SettingId::kTriggerSlope),
            .holdOffSamples = settings.value(SettingId::kTriggerHoldOff) * sampleRate};
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

void Analyzer::arm()
{
    const std::scoped_lock lock(commandMutex);
    commands.armRequested = true;
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
    followTransport(settings, command);
    const double sampleRate = engine.sampleRate();
    if (command.armRequested) {
        trigger.arm();
    }
    const bool synced =
        settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase) == protocol::TimeBase::kBeats;
    // The longest sweep the capture can hold; a frozen sweep keeps that much, so a longer beat
    // span chosen while frozen still has its audio.
    const CaptureRing& captureRing = engine.captureRing();
    const auto longestSweep =
        static_cast<std::uint64_t>(captureRing.capacity() - captureRing.capacity() / 8);
    if (command.frozen && !isFrozen && !freeze(sampleRate, synced ? longestSweep : 0)) {
        return false;
    }
    if (!command.frozen) {
        isFrozen = false;
    }
    if (command.columns == 0) {
        return false;
    }
    if (synced && settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode) ==
                      protocol::DisplayMode::kMulti) {
        return produceLanes(settings, command, frame);
    }
    lanes.clear(); // Frees the lanes' sweeps outside the multi-track view.

    const double spanSeconds = settings.value(SettingId::kTimeSpan);
    const double span = spanSeconds * sampleRate;
    const bool roll = isRollSpan(spanSeconds);
    const bool changed = settings != lastSettings || command.columns != lastColumns ||
                         command.panSeconds != lastPan || isFrozen != lastFrozen ||
                         command.viewStart != lastViewStart || command.viewEnd != lastViewEnd;

    Window window{.span = span, .roll = roll};
    auto sampleCount = static_cast<std::uint64_t>(std::llround(span));
    double samplesPerColumn = span / static_cast<double>(command.columns);
    bool freezeAfterRender = false;
    const SampleSource* source =
        isFrozen ? static_cast<const SampleSource*>(&hold) : &engine.captureRing();

    if (synced) {
        // Tempo-synced sweep. While frozen, the sweep is rebuilt from the clock and
        // capture end at the moment of freezing, so views and span still apply.
        const CaptureRing& ring = engine.captureRing();
        const std::uint64_t end = isFrozen ? frozenEnd : ring.endIndex();
        if (!changed && end == lastEnd) {
            return false;
        }
        // Frozen, only what was held can be shown; longer spans step down and are flagged.
        const std::uint64_t maxSamples =
            isFrozen ? std::min(longestSweep, frozenEnd - hold.beginIndex()) : longestSweep;
        const auto sweep =
            sweepWindow(isFrozen ? frozenStamp : engine.timeline().latest(), end, sampleRate,
                        settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan), maxSamples);
        if (!sweep) {
            return false;
        }
        // Live, only new samples are placed. Frozen, the screen stays as it was, unless the
        // span changed: then it is rebuilt from the held audio.
        const bool placed =
            isFrozen ? sweep->sampleCount == image.length() || image.update(hold, *sweep, true)
                     : image.update(ring, *sweep, false);
        if (!placed) {
            return false;
        }
        source = &image;
        window = {.start = 0.0,
                  .span = static_cast<double>(sweep->sampleCount),
                  .synced = true,
                  .limited = sweep->limited,
                  .cursor = sweep->cursor,
                  .cycleStart = sweep->cycleStart,
                  .spanQuarterNotes = sweep->spanQuarterNotes,
                  .barQuarterNotes = sweep->barQuarterNotes};
        sampleCount = sweep->sampleCount;
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
        const ChannelView triggerView = effectiveTriggerSource(
            settings.enumValue<ChannelView>(SettingId::kTriggerSource), engine.inputChannels());
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
        window.cropStart = command.viewStart;
        window.cropEnd = command.viewEnd;
    }
    if (!render(*source, settings, sampleRate, window, sampleCount, samplesPerColumn,
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
    viewSamples.resize(static_cast<std::size_t>(sampleCount));
    scratch.resize(std::max(scratch.size(), viewSamples.size()));

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
                                           (window.limited ? flag(FrameFlag::kLimited) : 0U)),
        .views = views,
        .startFraction = static_cast<float>(crop.startFraction),
        .cursor = static_cast<float>(window.cursor),
        .cycleStart = static_cast<float>(window.cycleStart),
        .spanQuarterNotes = static_cast<float>(window.spanQuarterNotes),
        .barQuarterNotes = static_cast<float>(window.barQuarterNotes),
        .viewStart = static_cast<float>(window.viewStart),
        .viewEnd = static_cast<float>(window.viewEnd),
        .visibleSamples = static_cast<float>(visibleSamples)};
    frameData.clear();
    std::size_t pointCount = 0;
    for (std::size_t index = 0; index < protocol::kChannelViewCount; ++index) {
        if ((views & (1U << index)) == 0) {
            continue;
        }
        if (!readView(source, inputChannels, static_cast<ChannelView>(index), start, viewSamples,
                      scratch)) {
            return false;
        }
        const std::span<const float> visible =
            std::span(viewSamples).subspan(crop.first, crop.end - crop.first);
        header.mode = reduceToColumns(visible, columns, reduced);
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : visible.size();
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    }
    header.pointCount = static_cast<std::uint32_t>(pointCount);
    const double croppedSamplesPerColumn = window.cropEnd - window.cropStart < 1.0
                                               ? visibleSamples / static_cast<double>(columns)
                                               : samplesPerColumn;
    header.samplesPerPoint = header.mode == protocol::FrameMode::kMinMax
                                 ? static_cast<float>(croppedSamplesPerColumn)
                                 : 1.0f;

    // Readouts cover the whole span, whatever part is on screen.
    const std::uint64_t readoutStart = window.readoutCount > 0 ? window.readoutStart : start;
    const std::uint64_t readoutCount = window.readoutCount > 0 ? window.readoutCount : sampleCount;
    viewSamples.resize(static_cast<std::size_t>(readoutCount));
    scratch.resize(std::max(scratch.size(), viewSamples.size()));
    if (!readView(source, inputChannels, triggerView, readoutStart, viewSamples, scratch)) {
        return false;
    }
    const Readouts readouts = computeReadouts(viewSamples, sampleRate);
    header.peak = readouts.peak;
    header.rms = readouts.rms;
    header.frequency = static_cast<float>(readouts.frequencyHz.value_or(0.0));

    encodeFrame(header, frameData, frame);
    ++sequence;
    return true;
}

// The multi-track view in Beats mode: one lane per track of the group, each the track's Mid as a
// persistent sweep on its own musical clock, so that lanes line up on bar lines. While frozen
// the lanes are not updated, so all of them hold the same moment.
bool Analyzer::produceLanes(const Settings& settings, const Commands& command,
                            std::vector<std::byte>& frame)
{
    // The group in order, at most kMaxLanes tracks, always including this instance's own.
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
                        .serial = ~std::uint64_t{0}});
        own = members.begin();
    }
    const std::uint64_t ownSerial = own->serial;
    std::size_t hidden = 0;
    if (members.size() > kMaxLanes) {
        hidden = members.size() - kMaxLanes;
        if (std::cmp_greater_equal(std::distance(members.begin(), own), kMaxLanes)) {
            std::rotate(members.begin() + static_cast<std::ptrdiff_t>(kMaxLanes - 1), own, own + 1);
        }
        members.resize(kMaxLanes);
    }
    std::vector<LaneLabel> labels;
    labels.reserve(members.size());
    for (const GroupMember& member : members) {
        labels.push_back({.name = member.track.name,
                          .colour = member.track.colour,
                          .own = member.serial == ownSerial});
    }
    publishLanes(std::move(labels), hidden);
    const std::uint32_t laneSetId = laneSet().id;

    // Each lane follows its own capture and clock; only new audio is placed. Frozen, each lane
    // holds its audio from the moment of freezing (or of joining), and is rebuilt from it when the
    // span changes.
    const auto beatSpan = settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan);
    bool fresh = false;
    std::map<std::uint64_t, std::unique_ptr<Lane>> next;
    laneOrder.clear();
    for (GroupMember& member : members) {
        const auto found = lanes.find(member.serial);
        auto lane = found != lanes.end() ? std::move(found->second) : std::make_unique<Lane>();
        lane->engine = std::move(member.engine);
        const CaptureRing& ring = lane->engine->captureRing();
        const std::uint64_t end = ring.endIndex();
        const auto longestSweep = ring.capacity() - ring.capacity() / 8;
        if (isFrozen) {
            if (!lane->hold.held()) {
                lane->hold.capture(*lane->engine, longestSweep, scratch);
            }
            const LaneHold& hold = lane->hold;
            const auto sweep =
                hold.held() ? sweepWindow(hold.clock(), hold.endIndex(), lane->engine->sampleRate(),
                                          beatSpan, hold.endIndex() - hold.beginIndex())
                            : std::nullopt;
            if (sweep && (!lane->window || sweep->sampleCount != lane->image.length()) &&
                lane->image.update(hold, *sweep, true)) {
                lane->window = sweep;
                fresh = true;
            }
        } else {
            if (lane->hold.held()) {
                lane->hold.release();
            }
            if (end != lane->end || !lane->window || settings != lastSettings) {
                const auto sweep = sweepWindow(lane->engine->timeline().latest(), end,
                                               lane->engine->sampleRate(), beatSpan, longestSweep);
                if (sweep && lane->image.update(ring, *sweep, false)) {
                    lane->window = sweep;
                    fresh = true;
                }
                lane->end = end;
            }
        }
        laneOrder.push_back(member.serial);
        next.insert_or_assign(member.serial, std::move(lane));
    }
    lanes = std::move(next);

    const Lane& ownLane = *lanes.at(ownSerial);
    if (!ownLane.window) {
        return false;
    }
    const bool changed = fresh || settings != lastSettings || command.columns != lastColumns ||
                         isFrozen != lastFrozen || command.viewStart != lastViewStart ||
                         command.viewEnd != lastViewEnd || laneSetId != lastLaneSet;
    if (!changed) {
        return false;
    }

    // Lanes share the own track's window. A lane whose sweep differs in length for a moment (a
    // tempo change reaching it a block later) is drawn silent until it catches up.
    const SweepWindow& window = *ownLane.window;
    const std::uint64_t count = window.sampleCount;
    const Crop crop = cropOf(static_cast<double>(count), 0.0, command.viewStart, command.viewEnd);
    const std::size_t columns = command.columns;
    viewSamples.resize(static_cast<std::size_t>(count));
    scratch.resize(std::max(scratch.size(), viewSamples.size()));
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
        .viewStart = static_cast<float>(command.viewStart),
        .viewEnd = static_cast<float>(command.viewEnd),
        .visibleSamples = static_cast<float>(crop.visibleSamples),
        .laneSet = laneSetId};
    std::size_t pointCount = 0;
    for (const std::uint64_t serial : laneOrder) {
        const Lane& lane = *lanes.at(serial);
        if (lane.image.length() != count || !readView(lane.image, lane.engine->inputChannels(),
                                                      ChannelView::kMid, 0, viewSamples, scratch)) {
            std::ranges::fill(viewSamples, 0.0f);
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

    // Readouts measure the own track's Mid over the whole span.
    if (readView(ownLane.image, engine.inputChannels(), ChannelView::kMid, 0, viewSamples,
                 scratch)) {
        const Readouts readouts = computeReadouts(viewSamples, engine.sampleRate());
        header.peak = readouts.peak;
        header.rms = readouts.rms;
        header.frequency = static_cast<float>(readouts.frequencyHz.value_or(0.0));
    }

    encodeFrame(header, frameData, frame);
    ++sequence;
    lastSettings = settings;
    lastColumns = command.columns;
    lastViewStart = command.viewStart;
    lastViewEnd = command.viewEnd;
    lastFrozen = isFrozen;
    lastLaneSet = laneSetId;
    return true;
}

} // namespace orizon::engine
