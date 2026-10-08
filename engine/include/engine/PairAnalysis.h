#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace orizon::engine {

// How two tracks sum, from the same stretch of each.
struct PairAnalysis
{
    // The sum's level against the level two unrelated signals of the same levels would give, in
    // dB: 0 harmless, about +3 for identical signals, below 0 when they cancel.
    double sumGain = 0.0;
    double correlation = 0.0; // -1 to +1, at no offset.
    // The shift, in samples, that best lines `b` up with `a`: positive when `b` lags. With
    // `inverted`, `b` lines up best with its polarity flipped.
    std::ptrdiff_t offset = 0;
    bool inverted = false;
    double fixedGain = 0.0; // The summing gain with `b` shifted and flipped as found.
};

// Analyses `a` and `b` (equal length), searching offsets up to `maxOffset` samples either way.
// Long stretches are analysed over their middle 32768 samples, the search runs coarse then fine,
// and offsets whose match is within 2% of the best prefer the smaller shift, so that a polarity
// flip wins over a half-period shift of a steady tone. `scratch` is working space.
[[nodiscard]] PairAnalysis analysePair(std::span<const float> a, std::span<const float> b,
                                       std::size_t maxOffset, std::vector<float>& scratch);

// `b` as the analysis suggests fixing it: shifted earlier by `offset` and flipped if `inverted`,
// silent where it has no samples.
void applyFix(std::span<const float> b, std::ptrdiff_t offset, bool inverted,
              std::span<float> fixed);

} // namespace orizon::engine
