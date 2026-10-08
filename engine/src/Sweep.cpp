#include "engine/Sweep.h"

#include <cmath>
#include <cstddef>

namespace orizon::engine {

using protocol::BeatSpan;

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
    if (count == 0 || end < count) {
        return std::nullopt;
    }
    const double playhead = Timeline::quarterNotesAt(stamp, end, sampleRate);
    const double cycleStart = std::floor(playhead / length) * length;
    const auto cycleStartIndex = static_cast<std::int64_t>(
        std::llround(Timeline::sampleIndexAt(stamp, cycleStart, sampleRate)));
    const std::uint64_t start = end - count;
    const std::int64_t offset = cycleStartIndex - static_cast<std::int64_t>(start);
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

} // namespace orizon::engine
