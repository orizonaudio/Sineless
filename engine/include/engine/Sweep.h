#pragma once

#include "engine/Timeline.h"
#include "protocol/Protocol.h"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace orizon::engine {

// The longest tempo-synced sweep kept per lane, in samples: 32 MB per channel of audio in screen
// order, 87 s at 96 kHz and 44 s at 192 kHz. Longer spans step down (see sweepWindow).
inline constexpr std::uint64_t kMaxSweepSamples = std::uint64_t{1} << 23U;

// Length of a beat span in quarter notes; bars follow the time signature.
[[nodiscard]] double beatSpanQuarterNotes(protocol::BeatSpan span, double barQuarterNotes);

// The samples a tempo-synced sweep shows: the newest `sampleCount` samples, rotated left
// by `rotation` so that the current cycle's start is at the left edge. The cycle so far then
// fills the screen up to `cursor` (a 0 to 1 fraction), and the end of the previous cycle fills
// the rest.
struct SweepWindow
{
    // The source index of the first sample shown: before the capture began (negative) while the
    // capture does not cover the span yet.
    std::int64_t start = 0;
    std::uint64_t sampleCount = 0;
    std::uint64_t rotation = 0;
    double cursor = 0.0;
    bool limited = false; // The requested span did not fit in `maxSamples`; a shorter one is used.
    // Musical context, in quarter notes: the left edge's project position, the span shown and
    // the bar length.
    double cycleStart = 0.0;
    double spanQuarterNotes = 0.0;
    double barQuarterNotes = 4.0;
    double cycleSeconds = 0.0; // The left edge's project time.
};

// Whether two sweep lengths differ by no more than kSweepLengthTolerance of the larger: a tempo
// change moves the length a little from block to block, which no screen column resolves, so a
// sweep image keeps its length through it instead of being rebuilt.
[[nodiscard]] constexpr bool sweepLengthsSimilar(std::uint64_t a, std::uint64_t b)
{
    const std::uint64_t larger = std::max(a, b);
    return (larger - std::min(a, b)) <=
           static_cast<std::uint64_t>(static_cast<double>(larger) * kSweepLengthTolerance);
}

// Cycles align to multiples of the span from the project start, i.e. to bars for bar spans.
// Nothing only for an empty span: a window the capture does not cover yet starts before the
// capture, and the sweep image shows what the capture holds.
[[nodiscard]] std::optional<SweepWindow> sweepWindow(const MusicalStamp& stamp, std::uint64_t end,
                                                     double sampleRate, protocol::BeatSpan span,
                                                     std::uint64_t maxSamples);

// What a frozen sweep shows for `span`, given the sweep image held (`image`, the window it was
// built for, of `imageLength` samples): the image as it is for the same span; the part of it
// holding the span's current cycle for a shorter span that divides the image's; otherwise a
// sweep rebuilt from the audio held on freeze, as far as that goes.
struct FrozenSweep
{
    SweepWindow window;
    double start = 0.0; // The part of the image shown, as fractions of it.
    double end = 1.0;
    bool rebuild = false; // The image must be rebuilt from the held audio for `window`.
};

[[nodiscard]] std::optional<FrozenSweep> frozenSweep(const MusicalStamp& stamp, std::uint64_t end,
                                                     double sampleRate, protocol::BeatSpan span,
                                                     const std::optional<SweepWindow>& image,
                                                     std::uint64_t imageLength);

} // namespace orizon::engine
