#include "engine/Reconstruction.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace orizon::engine {

namespace {

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

// A sinc under a Kaiser window `reach` samples each side. The raw view's β 8.6 puts sidelobes
// about 90 dB down, so a full-scale tone's ripple stays well under a pixel.
double windowedSinc(double t, double reach, double beta)
{
    if (std::abs(t) >= reach) {
        return 0.0;
    }
    const double sinc = t == 0.0 ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
    const double ratio = t / reach;
    return sinc * besselI0(beta * std::sqrt(1.0 - ratio * ratio)) / besselI0(beta);
}

} // namespace

std::size_t Reconstruction::pointsPerSample(double samples, std::size_t columns)
{
    if (!(samples > 0.0)) {
        return 1;
    }
    const double points = std::min(std::ceil(static_cast<double>(columns) / samples),
                                   static_cast<double>(kMaxPointsPerSample));
    return std::max(kMinPointsPerSample, static_cast<std::size_t>(points));
}

void Reconstruction::design(std::size_t factor)
{
    if (tapsFactor == factor) {
        return;
    }
    tapsFactor = factor;
    taps.assign(factor * 2 * reach, 0.0f);
    for (std::size_t phase = 0; phase < factor; ++phase) {
        // A point `phase / factor` of a sample after sample n reads samples n - reach + 1 to
        // n + reach.
        const double fraction = static_cast<double>(phase) / static_cast<double>(factor);
        double sum = 0.0;
        for (std::size_t k = 0; k < 2 * reach; ++k) {
            const double offset = static_cast<double>(k) - static_cast<double>(reach) + 1.0;
            const double weight = windowedSinc(fraction - offset, static_cast<double>(reach), beta);
            taps[phase * 2 * reach + k] = static_cast<float>(weight);
            sum += weight;
        }
        // Unity gain at DC, so a constant stays the constant.
        for (std::size_t k = 0; k < 2 * reach; ++k) {
            taps[phase * 2 * reach + k] = static_cast<float>(taps[phase * 2 * reach + k] / sum);
        }
    }
}

bool Reconstruction::reconstructsGap(std::span<const float> samples, std::size_t n,
                                     std::span<const std::size_t> seams) const
{
    const std::size_t count = samples.size();
    // Neighbours n + 1 - reach to n + reach, all held and none across a seam.
    for (const std::size_t seam : seams) {
        if (seam < count && seam + reach > n + 1 && seam <= n + reach) {
            return false;
        }
    }
    if (n + 1 < reach || n + reach >= count) {
        return false;
    }
    for (std::size_t k = 0; k < 2 * reach; ++k) {
        if (!std::isfinite(samples[n + 1 - reach + k])) {
            return false;
        }
    }
    return true;
}

void Reconstruction::upsample(std::span<const float> samples, std::size_t lead, std::size_t trail,
                              std::size_t factor, std::vector<float>& output,
                              std::span<const std::size_t> seams,
                              std::vector<std::uint8_t>* reconstructed)
{
    const std::size_t count = samples.size();
    if (reconstructed != nullptr) {
        reconstructed->clear();
    }
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
        const bool full = reconstructsGap(samples, n, seams);
        if (reconstructed != nullptr) {
            reconstructed->push_back(finite && full ? 1 : 0);
        }
        for (std::size_t phase = 1; phase < factor; ++phase) {
            float value = kNaN;
            if (finite && full) {
                const float* weights = taps.data() + phase * 2 * reach;
                const float* from = samples.data() + (n + 1 - reach);
                float sum = 0.0f;
                for (std::size_t k = 0; k < 2 * reach; ++k) {
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
