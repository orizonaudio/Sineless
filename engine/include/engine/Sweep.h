#pragma once

#include "engine/Timeline.h"
#include "protocol/Protocol.h"

#include <cstdint>
#include <optional>

namespace orizon::engine {

// Length of a beat span in quarter notes; bars follow the time signature (FR-9).
[[nodiscard]] double beatSpanQuarterNotes(protocol::BeatSpan span, double barQuarterNotes);

// The samples a tempo-synced sweep shows (FR-9): the newest `sampleCount` samples, rotated left
// by `rotation` so that the current cycle's start is at the left edge. The cycle so far then
// fills the screen up to `cursor` (a 0 to 1 fraction), and the end of the previous cycle fills
// the rest.
struct SweepWindow
{
    std::uint64_t start = 0;
    std::uint64_t sampleCount = 0;
    std::uint64_t rotation = 0;
    double cursor = 0.0;
    bool limited = false; // The requested span did not fit in `maxSamples`; a shorter one is used.
    // Musical context, in quarter notes: the left edge's project position, the span shown and
    // the bar length.
    double cycleStart = 0.0;
    double spanQuarterNotes = 0.0;
    double barQuarterNotes = 4.0;
};

// Cycles align to multiples of the span from the project start, i.e. to bars for bar spans.
// Returns nothing until `end` samples cover the span.
[[nodiscard]] std::optional<SweepWindow> sweepWindow(const MusicalStamp& stamp, std::uint64_t end,
                                                     double sampleRate, protocol::BeatSpan span,
                                                     std::uint64_t maxSamples);

} // namespace orizon::engine
