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

// The fewest and most points per sample of a raw frame (protocol/src/limits.ts says why).
inline constexpr std::size_t kMinPointsPerSample = protocol::kMinPointsPerSample;
inline constexpr std::size_t kMaxPointsPerSample = protocol::kMaxPointsPerSample;

class Reconstruction
{
public:
    // The raw view's filter: kReconstructionReach samples each side of a point, Kaiser β 8.6. The
    // min/max columns use a shorter one of the same kind (Reducer.h, kColumnFilterReach).
    static constexpr double kRawBeta = 8.6;
    explicit Reconstruction(std::size_t reach = kReconstructionReach, double beta = kRawBeta)
        : reach(reach), beta(beta)
    {}

    // Points per sample for a raw frame of `samples` samples drawn across `columns` columns: about
    // one point per column, at least kMinPointsPerSample (never the samples alone), at most
    // kMaxPointsPerSample; 1 with no samples known.
    [[nodiscard]] static std::size_t pointsPerSample(double samples, std::size_t columns);

    // Writes the wave from samples[lead] to samples[samples.size() - 1 - trail], `factor` points
    // per sample, into `output`. Samples before `lead` and after the last one are only read as
    // neighbours. A point whose neighbours run out (at the edge of what is held, as the newest
    // audio) or include one not finite is drawn straight between its two samples instead; one
    // beside a sample that is not finite is not finite. `seam` is the first sample after a join
    // (a sweep's playhead, between the newest pass and the previous one): a point whose neighbours
    // reach across it is drawn straight too, so neither side rings into the other; `seams`, the
    // same for several joins.
    void upsample(std::span<const float> samples, std::size_t lead, std::size_t trail,
                  std::size_t factor, std::vector<float>& output,
                  std::span<const std::size_t> seams);
    void upsample(std::span<const float> samples, std::size_t lead, std::size_t trail,
                  std::size_t factor, std::vector<float>& output,
                  std::size_t seam = std::numeric_limits<std::size_t>::max())
    {
        upsample(samples, lead, trail, factor, output, std::span(&seam, 1));
    }

    // Whether the gap after samples[n] is drawn as the wave, not straight: it and its neighbours
    // n + 1 - reach … n + reach are all held and finite, and none lies across a seam (the first
    // sample after a join). The one rule for the raw view and the min/max columns.
    [[nodiscard]] bool reconstructsGap(std::span<const float> samples, std::size_t n,
                                       std::span<const std::size_t> seams) const;

    // The weights of the point `phase / factor` of a sample after a sample n, for samples
    // n + 1 - reach … n + reach (designing them for `factor` if needed).
    [[nodiscard]] std::span<const float> weights(std::size_t factor, std::size_t phase)
    {
        design(factor);
        return std::span<const float>(taps).subspan(phase * 2 * reach, 2 * reach);
    }

private:
    std::size_t reach;
    double beta;
    // Taps per phase of `factor`, phase p at [p * 2 * reach, (p + 1) * 2 * reach).
    std::vector<float> taps;
    std::size_t tapsFactor = 0;

    void design(std::size_t factor);
};

} // namespace orizon::engine
