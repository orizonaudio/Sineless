#pragma once

#include "protocol/Protocol.h"

#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace orizon::engine {

// The band-limited wave between samples, for a view zoomed in so far that lines drawn straight
// from sample to sample would show corners the audio cannot hold. Each point between two samples
// is a Kaiser-windowed sinc over `kReconstructionReach` samples each side, as an oversampling DAC
// or a true-peak meter computes it; the samples themselves are kept exactly.
inline constexpr std::size_t kReconstructionReach = 16;

// The most points per sample (protocol/src/limits.ts says why).
inline constexpr std::size_t kMaxPointsPerSample = protocol::kMaxPointsPerSample;

class Reconstruction
{
public:
    // Points per sample for `samples` samples drawn across `columns` columns: 1 (the samples
    // alone) below two columns per sample, else about one point per column.
    [[nodiscard]] static std::size_t pointsPerSample(double samples, std::size_t columns);

    // Writes the wave from samples[lead] to samples[samples.size() - 1 - trail], `factor` points
    // per sample, into `output`. Samples before `lead` and after the last one are only read as
    // neighbours. A point whose neighbours run out (at the edge of what is held, as the newest
    // audio) or include one not finite is drawn straight between its two samples instead; one
    // beside a sample that is not finite is not finite. `seam` is the first sample after a join
    // (a sweep's playhead, between the newest pass and the previous one): a point whose neighbours
    // reach across it is drawn straight too, so neither side rings into the other.
    void upsample(std::span<const float> samples, std::size_t lead, std::size_t trail,
                  std::size_t factor, std::vector<float>& output,
                  std::size_t seam = std::numeric_limits<std::size_t>::max());

private:
    // Taps per phase of `factor`, phase p at [p * 2 * reach, (p + 1) * 2 * reach).
    std::vector<float> taps;
    std::size_t tapsFactor = 0;

    void design(std::size_t factor);
};

} // namespace orizon::engine
