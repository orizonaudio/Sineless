#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace orizon::engine {

// How two tracks sum, from the same stretch of each.
struct PairAnalysis
{
    // False when the two never sound together above the analysis gate: nothing was measured, and
    // the rest holds its defaults.
    bool measured = false;
    double correlation = 0.0; // -1 to +1, at no offset.
    // Whether `b` matches `a` clearly at some shift. Unrelated tracks (a kick and a hi-hat) still
    // have a strongest match, but a weak one; it suggests no fix.
    bool related = false;
    // The shift, in samples, that best lines `b` up with `a`: positive when `b` lags. With
    // `inverted`, `b` lines up best with its polarity flipped. 0 and false when not related.
    std::ptrdiff_t offset = 0;
    bool inverted = false;
};

// Analyses `a` and `b` (equal length), searching offsets up to `maxOffset` samples either way.
// Everything is measured only where both sound, in blocks of `block` samples as overlapGain
// decides, so a track that plays briefly in a long stretch is judged where it plays. The search
// runs coarse then fine over the whole stretch, and offsets whose match is within 2% of
// the best prefer the smaller shift, so that a polarity
// flip wins over a half-period shift of a steady tone. A best match whose correlation is below 0.3
// counts as unrelated. `scratch` is working space.
[[nodiscard]] PairAnalysis analysePair(std::span<const float> a, std::span<const float> b,
                                       std::size_t maxOffset, std::size_t block,
                                       std::vector<float>& scratch);

// The energy of `values`: the sum of their squares.
[[nodiscard]] double energyOf(std::span<const float> values);

// How loud a sum of tracks is against the same tracks unrelated, in dB: the sum's energy against
// `partsEnergy`, the tracks' energies added. -infinity when the sum cancels exactly (a measurement:
// the sum is silent), NaN when the tracks are silent: nothing to measure.
[[nodiscard]] double summingGain(double sumEnergy, double partsEnergy);

// The summing gain of `tracks` (equal length) where at least two of them sound together, so that
// stretches where one plays alone do not water it down: over blocks of `block` samples in which
// the second-loudest track is within 20 dB of the loudest, leaving out blocks more than 60 dB
// below the loudest block or at or below the analysis gate. 0 for fewer than two tracks (the sum
// is the track); NaN when they never sound together: nothing was measured.
[[nodiscard]] double overlapGain(std::span<const std::span<const float>> tracks, std::size_t block);

// The same gain from energies kept per block of `block` samples: `tracks` holds each track's
// energy (sum of squares) per block, and `sums` the energy of the tracks' sum per block, over the
// same blocks.
[[nodiscard]] double overlapGainOfBlocks(std::span<const std::span<const double>> tracks,
                                         std::span<const double> sums, std::size_t block);

} // namespace orizon::engine
