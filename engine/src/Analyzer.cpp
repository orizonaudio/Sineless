#include "engine/Analyzer.h"

#include "engine/ChannelViews.h"
#include "engine/FrameEncoder.h"
#include "engine/Readouts.h"
#include "engine/Reducer.h"
#include "engine/Roll.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace orizon::engine {

using protocol::ChannelView;
using protocol::FrameFlag;
using protocol::SettingId;

namespace {

constexpr std::uint8_t flag(FrameFlag value)
{
    return static_cast<std::uint8_t>(value);
}

TriggerParameters triggerParameters(const Settings& settings, double sampleRate)
{
    return {.level = settings.value(SettingId::kTriggerLevel),
            .hysteresis = settings.value(SettingId::kTriggerHysteresis),
            .slope = settings.enumValue<protocol::TriggerSlope>(SettingId::kTriggerSlope),
            .holdOffSamples = settings.value(SettingId::kTriggerHoldOff) * sampleRate};
}

} // namespace

Analyzer::Analyzer(Engine& engine) : engine(engine) {}

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

bool Analyzer::freeze(double sampleRate)
{
    if (!hold.capture(engine.captureRing(), hold.holdLength(sampleRate))) {
        return false; // The writer outran the copy; try again on the next step.
    }
    isFrozen = true;
    const std::scoped_lock lock(commandMutex);
    commands.frozen = true; // Single mode freezes on its own; keep the command in step.
    return true;
}

bool Analyzer::produce(std::vector<std::byte>& frame)
{
    const Commands command = takeCommands();
    const Settings settings = engine.settings().get();
    const double sampleRate = engine.sampleRate();
    if (command.armRequested) {
        trigger.arm();
    }
    if (command.frozen && !isFrozen && !freeze(sampleRate)) {
        return false;
    }
    if (!command.frozen) {
        isFrozen = false;
    }
    if (command.columns == 0) {
        return false;
    }

    const double spanSeconds = settings.value(SettingId::kTimeSpan);
    const double span = spanSeconds * sampleRate;
    const bool roll = isRollSpan(spanSeconds);
    const bool changed = settings != lastSettings || command.columns != lastColumns ||
                         command.panSeconds != lastPan || isFrozen != lastFrozen;

    Window window{.span = span, .roll = roll};
    auto sampleCount = static_cast<std::uint64_t>(std::llround(span));
    double samplesPerColumn = span / static_cast<double>(command.columns);
    bool freezeAfterRender = false;
    const SampleSource& source =
        isFrozen ? static_cast<const SampleSource&>(hold) : engine.captureRing();

    if (isFrozen) {
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
        const RollWindow rolled =
            rollWindow(engine.captureRing().endIndex(), span, command.columns);
        window.start = static_cast<double>(rolled.start);
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

    if (!render(source, settings, sampleRate, window, sampleCount, samplesPerColumn,
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

    FrameHeaderValues header{
        .sequence = sequence,
        .flags = static_cast<std::uint8_t>((window.triggered ? flag(FrameFlag::kTriggered) : 0U) |
                                           (isFrozen ? flag(FrameFlag::kFrozen) : 0U) |
                                           (window.roll ? flag(FrameFlag::kRoll) : 0U)),
        .views = views,
        .startFraction = static_cast<float>(window.start - first)};
    frameData.clear();
    for (std::size_t index = 0; index < protocol::kChannelViewCount; ++index) {
        if ((views & (1U << index)) == 0) {
            continue;
        }
        if (!readView(source, inputChannels, static_cast<ChannelView>(index), start, viewSamples,
                      scratch)) {
            return false;
        }
        header.mode = reduceToColumns(viewSamples, columns, reduced);
        frameData.insert(frameData.end(), reduced.begin(), reduced.end());
    }
    header.pointCount = static_cast<std::uint32_t>(
        header.mode == protocol::FrameMode::kMinMax ? columns : viewSamples.size());
    header.samplesPerPoint =
        header.mode == protocol::FrameMode::kMinMax ? static_cast<float>(samplesPerColumn) : 1.0f;

    if (!readView(source, inputChannels, triggerView, start, viewSamples, scratch)) {
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

} // namespace orizon::engine
