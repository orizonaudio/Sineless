#include "engine/PairAnalysis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace orizon::engine {

namespace {

constexpr std::size_t kMaxAnalysed = 32768;
constexpr std::size_t kDecimation = 8;
constexpr double kPreferSmallerShift = 0.98;

double energy(std::span<const float> values)
{
    double total = 0.0;
    for (const float value : values) {
        total += static_cast<double>(value) * value;
    }
    return total;
}

// Normalised cross-correlation of a[i] with b[i + offset], over the samples both have.
double correlationAt(std::span<const float> a, std::span<const float> b, std::ptrdiff_t offset,
                     double norm)
{
    const auto size = static_cast<std::ptrdiff_t>(a.size());
    const std::ptrdiff_t first = std::max<std::ptrdiff_t>(0, -offset);
    const std::ptrdiff_t last = std::min(size, size - offset);
    double total = 0.0;
    for (std::ptrdiff_t i = first; i < last; ++i) {
        total += static_cast<double>(a[static_cast<std::size_t>(i)]) *
                 b[static_cast<std::size_t>(i + offset)];
    }
    return norm > 0.0 ? total / norm : 0.0;
}

struct Match
{
    std::ptrdiff_t offset = 0;
    double value = 0.0;
};

// The offset in [from, to] with the strongest match. Offsets are visited from the smallest shift
// out, and a larger shift wins only if its match is clearly (more than 2%) stronger.
Match bestMatch(std::span<const float> a, std::span<const float> b, std::ptrdiff_t from,
                std::ptrdiff_t to, double norm)
{
    const std::ptrdiff_t nearest = std::clamp<std::ptrdiff_t>(0, from, to);
    Match best{.offset = nearest, .value = correlationAt(a, b, nearest, norm)};
    for (std::ptrdiff_t step = 1; nearest - step >= from || nearest + step <= to; ++step) {
        for (const std::ptrdiff_t offset : {nearest - step, nearest + step}) {
            if (offset < from || offset > to) {
                continue;
            }
            const double value = correlationAt(a, b, offset, norm);
            if (std::abs(value) * kPreferSmallerShift > std::abs(best.value)) {
                best = {.offset = offset, .value = value};
            }
        }
    }
    return best;
}

void decimate(std::span<const float> values, std::span<float> out)
{
    for (std::size_t i = 0; i < out.size(); ++i) {
        float total = 0.0f;
        for (std::size_t k = 0; k < kDecimation; ++k) {
            total += values[i * kDecimation + k];
        }
        out[i] = total / static_cast<float>(kDecimation);
    }
}

double gainDb(double sumEnergy, double unrelatedEnergy)
{
    if (unrelatedEnergy <= 0.0) {
        return 0.0;
    }
    return 10.0 * std::log10(std::max(sumEnergy, 1e-30) / unrelatedEnergy);
}

} // namespace

PairAnalysis analysePair(std::span<const float> a, std::span<const float> b, std::size_t maxOffset,
                         std::vector<float>& scratch)
{
    const std::size_t size = std::min(a.size(), b.size());
    const std::size_t length = std::min(size, kMaxAnalysed);
    const std::size_t start = (size - length) / 2;
    a = a.subspan(start, length);
    b = b.subspan(start, length);
    const double energyA = energy(a);
    const double energyB = energy(b);
    const double norm = std::sqrt(energyA * energyB);
    PairAnalysis result;
    if (norm <= 0.0) {
        return result;
    }
    double sumEnergy = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
        const double sum = static_cast<double>(a[i]) + b[i];
        sumEnergy += sum * sum;
    }
    result.sumGain = gainDb(sumEnergy, energyA + energyB);
    result.correlation = correlationAt(a, b, 0, norm);

    // Coarse search on decimated copies, then a fine one around the coarse match.
    const auto reach = static_cast<std::ptrdiff_t>(std::min(maxOffset, length / 2));
    const std::size_t coarseLength = length / kDecimation;
    scratch.resize(coarseLength * 2);
    const std::span<float> coarseA(scratch.data(), coarseLength);
    const std::span<float> coarseB(scratch.data() + coarseLength, coarseLength);
    decimate(a, coarseA);
    decimate(b, coarseB);
    const auto coarseReach = reach / static_cast<std::ptrdiff_t>(kDecimation);
    const double coarseNorm = std::sqrt(energy(coarseA) * energy(coarseB));
    const Match coarse = bestMatch(coarseA, coarseB, -coarseReach, coarseReach, coarseNorm);
    const auto centre = coarse.offset * static_cast<std::ptrdiff_t>(kDecimation);
    const auto span = static_cast<std::ptrdiff_t>(kDecimation);
    Match fine =
        bestMatch(a, b, std::max(-reach, centre - span), std::min(reach, centre + span), norm);
    // A shift that is not clearly better than none is not worth suggesting.
    if (std::abs(fine.value) * kPreferSmallerShift <= std::abs(result.correlation)) {
        fine = {.offset = 0, .value = result.correlation};
    }
    result.offset = fine.offset;
    result.inverted = fine.value < 0.0;

    double fixedEnergy = 0.0;
    double fixedA = 0.0;
    double fixedB = 0.0;
    const float sign = result.inverted ? -1.0f : 1.0f;
    const auto signedLength = static_cast<std::ptrdiff_t>(length);
    for (std::ptrdiff_t i = std::max<std::ptrdiff_t>(0, -fine.offset);
         i < std::min(signedLength, signedLength - fine.offset); ++i) {
        const double valueA = a[static_cast<std::size_t>(i)];
        const double valueB = sign * b[static_cast<std::size_t>(i + fine.offset)];
        fixedEnergy += (valueA + valueB) * (valueA + valueB);
        fixedA += valueA * valueA;
        fixedB += valueB * valueB;
    }
    result.fixedGain = gainDb(fixedEnergy, fixedA + fixedB);
    return result;
}

void applyFix(std::span<const float> b, std::ptrdiff_t offset, bool inverted,
              std::span<float> fixed)
{
    const float sign = inverted ? -1.0f : 1.0f;
    const auto size = static_cast<std::ptrdiff_t>(b.size());
    for (std::size_t i = 0; i < fixed.size(); ++i) {
        const auto source = static_cast<std::ptrdiff_t>(i) + offset;
        fixed[i] = source >= 0 && source < size ? sign * b[static_cast<std::size_t>(source)] : 0.0f;
    }
}

} // namespace orizon::engine
