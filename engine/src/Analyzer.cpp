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
                         laneSetId != lastLaneSet || command.previewFix != lastPreviewFix;

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
        window.cropStart = command.viewStart;
        window.cropEnd = command.viewEnd;
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

// The multi-track view in Beats mode: one lane per track of the group, each the track's Mid as a
// persistent sweep on its own musical clock, so that lanes line up on bar lines. While frozen
// the lanes are not updated, so all of them hold the same moment.
bool Analyzer::produceLanes(const Settings& settings, const Commands& command,
                            std::vector<std::byte>& frame)
{
    const std::uint64_t ownSerial = selectLanes(settings, command);
    const std::uint32_t laneSetId = laneSet().id;

    // Each lane follows its own capture and clock; only new audio is placed. Frozen, each lane
    // is rebuilt from its held audio when the span changes.
    const auto beatSpan = settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan);
    bool fresh = false;
    for (const std::uint64_t serial : laneOrder) {
        Lane& lane = *lanes.at(serial);
        const CaptureRing& ring = lane.engine->captureRing();
        const std::uint64_t end = ring.endIndex();
        if (isFrozen) {
            const LaneHold& hold = lane.hold;
            const auto sweep =
                hold.held() ? sweepWindow(hold.clock(), hold.endIndex(), lane.engine->sampleRate(),
                                          beatSpan, hold.endIndex() - hold.beginIndex())
                            : std::nullopt;
            if (sweep && (!lane.window || sweep->sampleCount != lane.image.length()) &&
                lane.image.update(hold, *sweep, true)) {
                lane.window = sweep;
                fresh = true;
            }
        } else if (end != lane.end || !lane.window || settings != lastSettings) {
            const auto longestSweep = ring.capacity() - ring.capacity() / 8;
            const auto sweep = sweepWindow(lane.engine->timeline().latest(), end,
                                           lane.engine->sampleRate(), beatSpan, longestSweep);
            if (sweep && lane.image.update(ring, *sweep, false)) {
                lane.window = sweep;
                fresh = true;
            }
            lane.end = end;
        }
    }

    const Lane& ownLane = *lanes.at(ownSerial);
    if (!ownLane.window) {
        return false;
    }
    const bool changed = fresh || settings != lastSettings || command.columns != lastColumns ||
                         isFrozen != lastFrozen || command.viewStart != lastViewStart ||
                         command.viewEnd != lastViewEnd || laneSetId != lastLaneSet ||
                         command.previewFix != lastPreviewFix;
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
    if (comparing) {
        // Every lane, read whole from its sweep.
        laneSamples.resize(laneOrder.size());
        for (std::size_t i = 0; i < laneOrder.size(); ++i) {
            const Lane& lane = *lanes.at(laneOrder[i]);
            std::vector<float>& into = laneSamples[i];
            into.resize(static_cast<std::size_t>(count));
            if (lane.image.length() != count || !readView(lane.image, lane.engine->inputChannels(),
                                                          ChannelView::kMid, 0, into, scratch)) {
                std::ranges::fill(into, 0.0f);
            }
        }
        renderCompare(header, crop.first, crop.end, columns, command.previewFix, frame);
        lastSettings = settings;
        lastColumns = command.columns;
        lastViewStart = command.viewStart;
        lastViewEnd = command.viewEnd;
        lastFrozen = isFrozen;
        lastLaneSet = laneSetId;
        lastPreviewFix = command.previewFix;
        return true;
    }
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
    if (comparing) {
        laneSamples.resize(laneOrder.size());
        for (std::size_t i = 0; i < laneOrder.size(); ++i) {
            const std::uint64_t serial = laneOrder[i];
            const auto [source, laneStart] = sourceOf(serial);
            std::vector<float>& into = laneSamples[i];
            into.resize(static_cast<std::size_t>(sampleCount));
            if (!readView(*source, lanes.at(serial)->engine->inputChannels(), ChannelView::kMid,
                          laneStart, into, scratch)) {
                std::ranges::fill(into, 0.0f);
            }
        }
        renderCompare(header, crop.first, crop.end, columns, command.previewFix, frame);
        return true;
    }
    frameData.clear();
    std::size_t pointCount = 0;
    for (const std::uint64_t serial : laneOrder) {
        const Lane& lane = *lanes.at(serial);
        const SampleSource* source = &ownSource;
        std::uint64_t laneStart = start;
        if (serial != ownLane) {
            const MusicalStamp clock =
                isFrozen ? lane.hold.clock() : lane.engine->timeline().latest();
            source = isFrozen ? static_cast<const SampleSource*>(&lane.hold)
                              : &lane.engine->captureRing();
            laneStart = static_cast<std::uint64_t>(std::max(
                std::round(Timeline::sampleIndexAt(clock, startQuarterNotes, sampleRate)), 0.0));
        }
        if (!readView(*source, lane.engine->inputChannels(), ChannelView::kMid, laneStart,
                      viewSamples, scratch)) {
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

    // Readouts measure this track's Mid over the whole span.
    const std::uint64_t readoutStart = window.readoutCount > 0 ? window.readoutStart : start;
    const std::uint64_t readoutCount = window.readoutCount > 0 ? window.readoutCount : sampleCount;
    viewSamples.resize(static_cast<std::size_t>(readoutCount));
    scratch.resize(std::max(scratch.size(), viewSamples.size()));
    if (readView(ownSource, engine.inputChannels(), ChannelView::kMid, readoutStart, viewSamples,
                 scratch)) {
        const Readouts readouts = computeReadouts(viewSamples, sampleRate);
        header.peak = readouts.peak;
        header.rms = readouts.rms;
        header.frequency = static_cast<float>(readouts.frequencyHz.value_or(0.0));
    }

    encodeFrame(header, frameData, frame);
    ++sequence;
    return true;
}

// A Compare frame: every track's lane from `laneSamples` (raw Mid samples of the window), then
// the sum of the compared tracks, then each track's match against this track over the visible
// part [first, end). With the preview, the sum uses every compared track shifted and flipped as
// its match suggests.
//
// Matches are searched over the whole visible part, which is costly, so live they are refreshed
// one track per frame, each at most every 100 ms of audio; frozen, or when the tracks or the
// visible part change, all of them at once. The summing gains count only where tracks overlap.
void Analyzer::renderCompare(FrameHeaderValues header, std::size_t first, std::size_t end,
                             std::size_t columns, bool previewFix, std::vector<std::byte>& frame)
{
    const double sampleRate = engine.sampleRate();
    const auto maxOffset = static_cast<std::size_t>(std::lround(0.01 * sampleRate));
    const auto block = static_cast<std::size_t>(std::lround(0.01 * sampleRate)); // Overlap blocks.
    const auto ownIndex =
        static_cast<std::size_t>(std::ranges::find(laneOrder, ownLane) - laneOrder.begin());
    const std::vector<float>& own = laneSamples.at(ownIndex);
    const std::size_t count = own.size();
    const auto visible = [&](std::span<const float> values) {
        return values.subspan(first, end - first);
    };

    // Which matches to search now.
    const std::uint64_t now = engine.captureRing().endIndex();
    const auto refresh = static_cast<std::uint64_t>(std::lround(0.1 * sampleRate));
    if (isFrozen || header.laneSet != matchLaneSet || first != matchFirst || end != matchEnd ||
        count != matchCount) {
        matchStates.clear();
    }
    matchLaneSet = header.laneSet;
    matchFirst = first;
    matchEnd = end;
    matchCount = count;
    std::optional<std::uint64_t> stalest;
    for (const std::uint64_t serial : laneOrder) {
        const auto found = matchStates.find(serial);
        if (serial == ownLane || !lanes.at(serial)->compared || found == matchStates.end() ||
            now - found->second.analysedAt < refresh) {
            continue;
        }
        if (!stalest || found->second.analysedAt < matchStates.at(*stalest).analysedAt) {
            stalest = serial;
        }
    }

    rawSum.assign(count, 0.0f);
    fixedSum.assign(count, 0.0f);
    fixedLanes.resize(laneOrder.size());
    matches.assign(laneOrder.size() * protocol::kMatchValues, 0.0f);
    rawTracks.clear();
    fixedTracks.clear();
    for (std::size_t lane = 0; lane < laneOrder.size(); ++lane) {
        const std::uint64_t serial = laneOrder[lane];
        if (!lanes.at(serial)->compared) {
            continue;
        }
        const std::vector<float>& samples = laneSamples[lane];
        std::vector<float>& fixed = fixedLanes[lane];
        fixed.resize(count);
        if (lane == ownIndex) {
            std::ranges::copy(samples, fixed.begin());
        } else {
            if (!matchStates.contains(serial) || stalest == serial) {
                matchStates[serial] = {.analysis = analysePair(visible(own), visible(samples),
                                                               maxOffset, block, scratch),
                                       .analysedAt = now};
            }
            const PairAnalysis& match = matchStates.at(serial).analysis;
            applyFix(samples, match.offset, match.inverted, fixed);
            float* const values = matches.data() + lane * protocol::kMatchValues;
            values[0] = static_cast<float>(match.correlation);
            values[1] = static_cast<float>(match.offset);
            values[2] = !match.related ? 0.0f : match.inverted ? -1.0f : 1.0f;
        }
        for (std::size_t i = 0; i < count; ++i) {
            rawSum[i] += samples[i];
            fixedSum[i] += fixed[i];
        }
        rawTracks.push_back(visible(samples));
        fixedTracks.push_back(visible(fixed));
    }
    std::erase_if(matchStates, [&](const auto& entry) {
        return std::ranges::find(laneOrder, entry.first) == laneOrder.end();
    });

    header.laneCount = static_cast<std::uint8_t>(laneOrder.size() + 1);
    header.flags = static_cast<std::uint8_t>(header.flags | flag(FrameFlag::kCompared));
    header.sumGain = static_cast<float>(overlapGain(rawTracks, block));
    header.fixedGain = static_cast<float>(overlapGain(fixedTracks, block));
    frameData.clear();
    std::size_t pointCount = 0;
    const auto addLane = [&](std::span<const float> values) {
        header.mode = reduceToColumns(visible(values), columns, reduced);
        pointCount = header.mode == protocol::FrameMode::kMinMax ? columns : end - first;
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    };
    for (const std::vector<float>& samples : laneSamples) {
        addLane(samples);
    }
    addLane(previewFix ? fixedSum : rawSum);
    frameData.insert(frameData.end(), matches.begin(), matches.end());
    header.pointCount = static_cast<std::uint32_t>(pointCount);
    header.samplesPerPoint = header.mode == protocol::FrameMode::kMinMax
                                 ? static_cast<float>(static_cast<double>(header.visibleSamples) /
                                                      static_cast<double>(columns))
                                 : 1.0f;

    // Readouts measure this track over the whole window.
    const Readouts readouts = computeReadouts(own, sampleRate);
    header.peak = readouts.peak;
    header.rms = readouts.rms;
    header.frequency = static_cast<float>(readouts.frequencyHz.value_or(0.0));

    encodeFrame(header, frameData, frame);
    ++sequence;
}

} // namespace orizon::engine
