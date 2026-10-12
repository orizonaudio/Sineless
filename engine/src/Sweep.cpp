#include "engine/Sweep.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace orizon::engine {

using protocol::BeatSpan;

namespace {

constexpr double kEpsilon = 1e-9;

// Whether `value` is a whole multiple of `of`.
bool isMultiple(double value, double of)
{
    const double ratio = value / of;
    return std::abs(ratio - std::round(ratio)) < kEpsilon;
}

} // namespace

double beatSpanQuarterNotes(BeatSpan span, double barQuarterNotes)
{
    const protocol::BeatSpanLength& length =
        protocol::kBeatSpanLengths.at(static_cast<std::size_t>(span));
    return length.inBars ? length.amount * barQuarterNotes : length.amount;
}

std::optional<SweepWindow> sweepWindow(const MusicalStamp& stamp, std::uint64_t end,
                                       double sampleRate, BeatSpan span, std::uint64_t maxSamples)
{
    const double samplesPerQuarterNote = 60.0 * sampleRate / stamp.tempo;
    const double bar = Timeline::barQuarterNotes(stamp);
    bool limited = false;
    double length = beatSpanQuarterNotes(span, bar);
    while (length * samplesPerQuarterNote > static_cast<double>(maxSamples) &&
           span != BeatSpan::kSixteenthBeat) {
        span = static_cast<BeatSpan>(static_cast<int>(span) - 1);
        length = beatSpanQuarterNotes(span, bar);
        limited = true;
    }

    const auto count = static_cast<std::uint64_t>(std::llround(length * samplesPerQuarterNote));
    if (count == 0) {
        return std::nullopt;
    }
    const double playhead = Timeline::quarterNotesAt(stamp, end, sampleRate);
    const double cycleStart = std::floor(playhead / length) * length;
    const auto cycleStartIndex = static_cast<std::int64_t>(
        std::llround(Timeline::sampleIndexAt(stamp, cycleStart, sampleRate)));
    const auto start = static_cast<std::int64_t>(end) - static_cast<std::int64_t>(count);
    const std::int64_t offset = cycleStartIndex - start;
    const auto windowLength = static_cast<std::int64_t>(count);
    const std::uint64_t rotation =
        offset <= 0 || offset >= windowLength ? 0 : static_cast<std::uint64_t>(offset);
    return SweepWindow{.start = start,
                       .sampleCount = count,
                       .rotation = rotation,
                       .cursor = rotation == 0 ? 1.0
                                               : 1.0 - static_cast<double>(rotation) /
                                                           static_cast<double>(count),
                       .limited = limited,
                       .cycleStart = cycleStart,
                       .spanQuarterNotes = length,
                       .barQuarterNotes = bar,
                       .cycleSeconds = Timeline::secondsAt(stamp, cycleStart)};
}

std::optional<FrozenSweep> frozenSweep(const MusicalStamp& stamp, std::uint64_t end,
                                       double sampleRate, BeatSpan span,
                                       const std::optional<SweepWindow>& image,
                                       std::uint64_t imageLength)
{
    const auto requested = sweepWindow(stamp, end, sampleRate, span, kMaxSweepSamples);
    if (!requested) {
        return std::nullopt;
    }
    if (image && imageLength > 0) {
        if (sweepLengthsSimilar(requested->sampleCount, imageLength) &&
            std::abs(requested->spanQuarterNotes - image->spanQuarterNotes) < kEpsilon) {
            return FrozenSweep{.window = *requested};
        }
        if (requested->spanQuarterNotes < image->spanQuarterNotes &&
            isMultiple(image->spanQuarterNotes, requested->spanQuarterNotes)) {
            // The span's current cycle lies within the image's cycle.
            const double start =
                (requested->cycleStart - image->cycleStart) / image->spanQuarterNotes;
            const double length = requested->spanQuarterNotes / image->spanQuarterNotes;
            if (start >= -kEpsilon && start + length <= 1.0 + kEpsilon) {
                return FrozenSweep{.window = *requested,
                                   .start = std::clamp(start, 0.0, 1.0),
                                   .end = std::clamp(start + length, 0.0, 1.0)};
            }
        }
    }
    return FrozenSweep{.window = *requested, .rebuild = true};
}

} // namespace orizon::engine
