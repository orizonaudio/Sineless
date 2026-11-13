#include "engine/Reconstruction.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace orizon::engine {

namespace {

constexpr std::size_t kReach = kReconstructionReach;
// The Kaiser window's shape: sidelobes about 90 dB down, so a full-scale tone's ripple stays well
// under a pixel.
constexpr double kKaiserBeta = 8.6;

// The modified Bessel function of the first kind, order 0, by its series.
double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    const double quarter = x * x / 4.0;
    for (int k = 1; k < 50; ++k) {
        term *= quarter / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

double windowedSinc(double t)
{
    const auto reach = static_cast<double>(kReach);
    if (std::abs(t) >= reach) {
        return 0.0;
    }
    const double sinc = t == 0.0 ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
    const double ratio = t / reach;
    return sinc * besselI0(kKaiserBeta * std::sqrt(1.0 - ratio * ratio)) / besselI0(kKaiserBeta);
}

} // namespace

std::size_t Reconstruction::pointsPerSample(double samples, std::size_t columns)
{
    if (!(samples > 0.0)) {
        return 1;
    }
    const double perSample = static_cast<double>(columns) / samples;
    if (perSample < 2.0) {
        return 1;
    }
    return std::min(kMaxPointsPerSample, static_cast<std::size_t>(std::ceil(perSample)));
}

void Reconstruction::design(std::size_t factor)
{
    if (tapsFactor == factor) {
        return;
    }
    tapsFactor = factor;
    taps.assign(factor * 2 * kReach, 0.0f);
    for (std::size_t phase = 0; phase < factor; ++phase) {
        // A point `phase / factor` of a sample after sample n reads samples n - reach + 1 to
        // n + reach.
        const double fraction = static_cast<double>(phase) / static_cast<double>(factor);
        double sum = 0.0;
        for (std::size_t k = 0; k < 2 * kReach; ++k) {
            const double offset = static_cast<double>(k) - static_cast<double>(kReach) + 1.0;
            const double weight = windowedSinc(fraction - offset);
            taps[phase * 2 * kReach + k] = static_cast<float>(weight);
            sum += weight;
        }
        // Unity gain at DC, so a constant stays the constant.
        for (std::size_t k = 0; k < 2 * kReach; ++k) {
            taps[phase * 2 * kReach + k] = static_cast<float>(taps[phase * 2 * kReach + k] / sum);
        }
    }
}

void Reconstruction::upsample(std::span<const float> samples, std::size_t lead, std::size_t trail,
                              std::size_t factor, std::vector<float>& output, std::size_t seam)
{
    const std::size_t count = samples.size();
    if (factor <= 1 || count <= lead + trail) {
        output.assign(samples.begin() + static_cast<std::ptrdiff_t>(std::min(lead, count)),
                      samples.end() - static_cast<std::ptrdiff_t>(
                                          std::min(trail, count - std::min(lead, count))));
        return;
    }
    design(factor);
    const std::size_t last = count - 1 - trail;
    output.resize((last - lead) * factor + 1);
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    std::size_t point = 0;
    for (std::size_t n = lead; n <= last; ++n) {
        output[point++] = samples[n];
        if (n == last) {
            break;
        }
        const float here = samples[n];
        const float next = samples[n + 1];
        const bool finite = std::isfinite(here) && std::isfinite(next);
        // Neighbours n + 1 - reach to n + reach, all held and none across the seam.
        const bool acrossSeam = seam < count && seam + kReach > n + 1 && seam <= n + kReach;
        bool full = finite && !acrossSeam && n + 1 >= kReach && n + kReach < count;
        for (std::size_t k = 0; full && k < 2 * kReach; ++k) {
            full = std::isfinite(samples[n + 1 - kReach + k]);
        }
        for (std::size_t phase = 1; phase < factor; ++phase) {
            float value = kNaN;
            if (finite && full) {
                const float* weights = taps.data() + phase * 2 * kReach;
                const float* from = samples.data() + (n + 1 - kReach);
                float sum = 0.0f;
                for (std::size_t k = 0; k < 2 * kReach; ++k) {
                    sum += weights[k] * from[k];
                }
                value = sum;
            } else if (finite) {
                const float fraction = static_cast<float>(phase) / static_cast<float>(factor);
                value = here + (next - here) * fraction;
            }
            output[point++] = value;
        }
    }
}

} // namespace orizon::engine
