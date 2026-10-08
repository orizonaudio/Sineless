#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace orizon::engine {

// How two tracks sum, from the same stretch of each.
struct PairAnalysis
{
    // Where both sound, the sum's level against the level two unrelated signals of the same
    // levels would give, in dB: 0 harmless, about +3 for identical signals, below 0 when they
    // cancel.
    double sumGain = 0.0;
    double correlation = 0.0; // -1 to +1, at no offset.
    // Whether `b` matches `a` clearly at some shift. Unrelated tracks (a kick and a hi-hat) still
    // have a strongest match, but a weak one; it suggests no fix.
    bool related = false;
    // The shift, in samples, that best lines `b` up with `a`: positive when `b` lags. With
    // `inverted`, `b` lines up best with its polarity flipped. 0 and false when not related.
    std::ptrdiff_t offset = 0;
    bool inverted = false;
    double fixedGain = 0.0; // The summing gain with `b` shifted and flipped as found.
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
// `partsEnergy`, the tracks' energies added. 0 when the tracks are silent.
[[nodiscard]] double summingGain(double sumEnergy, double partsEnergy);

// The summing gain of `tracks` (equal length) where at least two of them sound together, so that
// stretches where one plays alone do not water it down: over blocks of `block` samples in which
// the second-loudest track is within 20 dB of the loudest, leaving out blocks more than 60 dB
// below the loudest block. 0 when they never sound together.
[[nodiscard]] double overlapGain(std::span<const std::span<const float>> tracks, std::size_t block);

// The same gain from energies kept per block: `tracks` holds each track's energy (sum of squares)
// per block, and `sums` the energy of the tracks' sum per block, over the same blocks.
[[nodiscard]] double overlapGainOfBlocks(std::span<const std::span<const double>> tracks,
                                         std::span<const double> sums);

// `b` as the analysis suggests fixing it: shifted earlier by `offset` and flipped if `inverted`,
// silent where it has no samples.
void applyFix(std::span<const float> b, std::ptrdiff_t offset, bool inverted,
              std::span<float> fixed);

} // namespace orizon::engine
