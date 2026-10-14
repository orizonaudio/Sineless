#include "engine/PairAnalysis.h"

#include "protocol/Protocol.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>

namespace orizon::engine {

namespace {

constexpr std::size_t kDecimation = 8;
constexpr double kPreferSmallerShift = 0.98;
// The weakest best match that still counts as the same sound arriving twice.
constexpr double kRelatedMatch = 0.3;
// Overlap: tracks sound together within 20 dB of each other, and above 60 dB below the loudest.
constexpr double kTogether = 0.01;
constexpr double kAudible = 1e-6;
// A track's mean power (RMS squared) at the analysis gate: quieter blocks are never analysed.
double silentPower()
{
    return std::pow(10.0, protocol::kAnalysisGateDbfs / 10.0);
}

// The dot product of a[0, count) and b[0, count). Eight independent float sums, so the compiler
// can run them as vector lanes; each sums an eighth of the terms, which keeps float rounding far
// below what a correlation needs.
double dot(const float* a, const float* b, std::size_t count)
{
    constexpr std::size_t kLanes = 8;
    std::array<float, kLanes> sums{};
    std::size_t i = 0;
    for (; i + kLanes <= count; i += kLanes) {
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            sums[lane] += a[i + lane] * b[i + lane];
        }
    }
    double total = 0.0;
    for (const float sum : sums) {
        total += sum;
    }
    for (; i < count; ++i) {
        total += static_cast<double>(a[i]) * b[i];
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
    if (last <= first || norm <= 0.0) {
        return 0.0;
    }
    return dot(a.data() + first, b.data() + first + offset,
               static_cast<std::size_t>(last - first)) /
           norm;
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

// Each track's energy per block of `block` samples.
std::vector<std::vector<double>> blockEnergies(std::span<const std::span<const float>> tracks,
                                               std::size_t block)
{
    std::vector<std::vector<double>> energies(tracks.size());
    for (std::size_t track = 0; track < tracks.size(); ++track) {
        const std::span<const float> samples = tracks[track];
        const std::size_t blocks = (samples.size() + block - 1) / block;
        energies[track].assign(blocks, 0.0);
        for (std::size_t index = 0; index < blocks; ++index) {
            const std::size_t first = index * block;
            energies[track][index] =
                energyOf(samples.subspan(first, std::min(block, samples.size() - first)));
        }
    }
    return energies;
}

std::vector<std::span<const double>> spansOf(const std::vector<std::vector<double>>& energies)
{
    return {energies.begin(), energies.end()};
}

// Which blocks hold at least two tracks sounding together, from each track's energy per block
// of `block` samples (equal lengths): the second-loudest within 20 dB of the loudest, not more
// than 60 dB below the loudest block, and above the analysis gate; for two tracks or more. A
// shorter last block is judged by its energy as a whole one is: its tracks are
// equally short, so only one of a few samples falls under the other two limits.
std::vector<bool> togetherBlocks(std::span<const std::span<const double>> energies,
                                 std::size_t block)
{
    assert(energies.size() >= 2);
    const std::size_t blocks = energies.front().size();
    std::vector<double> loudest(blocks, 0.0);
    std::vector<double> second(blocks, 0.0);
    double loudestBlock = 0.0;
    for (std::size_t index = 0; index < blocks; ++index) {
        for (const std::span<const double> track : energies) {
            const double level = track[index];
            if (level > loudest[index]) {
                second[index] = loudest[index];
                loudest[index] = level;
            } else if (level > second[index]) {
                second[index] = level;
            }
        }
        loudestBlock = std::max(loudestBlock, loudest[index]);
    }
    const double silentBlock = silentPower() * static_cast<double>(block);
    std::vector<bool> together(blocks);
    for (std::size_t index = 0; index < blocks; ++index) {
        together[index] = loudest[index] > kAudible * loudestBlock &&
                          second[index] >= kTogether * loudest[index] &&
                          second[index] > silentBlock;
    }
    return together;
}

// Which blocks of `block` samples (more than 0) hold at least two of `tracks` (two or more, of
// equal length) sounding together.
std::vector<bool> overlapBlocks(std::span<const std::span<const float>> tracks, std::size_t block)
{
    assert(tracks.size() >= 2 && block > 0);
    return togetherBlocks(spansOf(blockEnergies(tracks, block)), block);
}

} // namespace

// Of the finite values: one that is not (from a broken signal upstream) is measured around.
double energyOf(std::span<const float> values)
{
    double total = 0.0;
    for (const float value : values) {
        if (std::isfinite(value)) {
            total += static_cast<double>(value) * value;
        }
    }
    return total;
}

double summingGain(double sumEnergy, double partsEnergy)
{
    if (partsEnergy <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // Raw, with no floor: an exact cancellation (a sum of exactly 0) is -infinity, which the UI
    // shows as such below its gain floor (protocol::kGainFloorDb, applied by its formatter).
    return 10.0 * std::log10(sumEnergy / partsEnergy);
}

PairAnalysis analysePair(std::span<const float> a, std::span<const float> b, std::size_t maxOffset,
                         std::size_t block, std::vector<float>& scratch)
{
    // Only where both sound: elsewhere a sample of one meets silence in the other, which would
    // water the correlation down.
    const std::size_t length = std::min(a.size(), b.size());
    const std::size_t coarseLength = length / kDecimation;
    scratch.resize(2 * length + 2 * coarseLength);
    const std::span<float> maskedA(scratch.data(), length);
    const std::span<float> maskedB(scratch.data() + length, length);
    const std::array<std::span<const float>, 2> pair{a.first(length), b.first(length)};
    const std::vector<bool> together = overlapBlocks(pair, block == 0 ? length : block);
    // A sample that is not finite (from a broken signal upstream) counts as silence in both, so
    // the match is measured around it.
    for (std::size_t i = 0; i < length; ++i) {
        const bool keep = !together.empty() && together[i / (block == 0 ? length : block)] &&
                          std::isfinite(a[i]) && std::isfinite(b[i]);
        maskedA[i] = keep ? a[i] : 0.0f;
        maskedB[i] = keep ? b[i] : 0.0f;
    }
    a = maskedA;
    b = maskedB;
    const double energyA = energyOf(a);
    const double energyB = energyOf(b);
    const double norm = std::sqrt(energyA * energyB);
    PairAnalysis result;
    if (norm <= 0.0) {
        return result;
    }
    result.measured = true;
    result.correlation = correlationAt(a, b, 0, norm);

    // Coarse search on decimated copies, then a fine one around the coarse match.
    const auto reach = static_cast<std::ptrdiff_t>(std::min(maxOffset, length / 2));
    const std::span<float> coarseA(scratch.data() + 2 * length, coarseLength);
    const std::span<float> coarseB(scratch.data() + 2 * length + coarseLength, coarseLength);
    decimate(a, coarseA);
    decimate(b, coarseB);
    const auto coarseReach = reach / static_cast<std::ptrdiff_t>(kDecimation);
    const double coarseNorm = std::sqrt(energyOf(coarseA) * energyOf(coarseB));
    const Match coarse = bestMatch(coarseA, coarseB, -coarseReach, coarseReach, coarseNorm);
    const auto centre = coarse.offset * static_cast<std::ptrdiff_t>(kDecimation);
    const auto span = static_cast<std::ptrdiff_t>(kDecimation);
    Match fine =
        bestMatch(a, b, std::max(-reach, centre - span), std::min(reach, centre + span), norm);
    // A shift that is not clearly better than none is not worth suggesting.
    if (std::abs(fine.value) * kPreferSmallerShift <= std::abs(result.correlation)) {
        fine = {.offset = 0, .value = result.correlation};
    }
    result.related = std::abs(fine.value) >= kRelatedMatch;
    if (result.related) {
        result.offset = fine.offset;
        result.inverted = fine.value < 0.0;
    }
    return result;
}

double overlapGain(std::span<const std::span<const float>> tracks, std::size_t block)
{
    if (tracks.size() < 2 || block == 0) {
        return 0.0;
    }
    // Positions where a track is not finite (a broken signal upstream) are left out of the sum
    // and of every part alike, so that the gain is measured around them.
    const std::size_t length = tracks.front().size();
    const std::size_t blocks = (length + block - 1) / block;
    std::vector<double> sums(blocks, 0.0);
    std::vector<std::vector<double>> energies(tracks.size(), std::vector<double>(blocks, 0.0));
    for (std::size_t index = 0; index < blocks; ++index) {
        const std::size_t first = index * block;
        const std::size_t last = std::min(first + block, length);
        for (std::size_t i = first; i < last; ++i) {
            if (!std::ranges::all_of(tracks, [i](std::span<const float> track) {
                    return std::isfinite(track[i]);
                })) {
                continue;
            }
            double total = 0.0;
            for (std::size_t track = 0; track < tracks.size(); ++track) {
                const double value = tracks[track][i];
                total += value;
                energies[track][index] += value * value;
            }
            sums[index] += total * total;
        }
    }
    return overlapGainOfBlocks(spansOf(energies), sums, block);
}

double overlapGainOfBlocks(std::span<const std::span<const double>> tracks,
                           std::span<const double> sums, std::size_t block)
{
    if (tracks.size() < 2) {
        return 0.0; // One track alone: the sum is that track, as loud as itself.
    }
    const std::vector<bool> together = togetherBlocks(tracks, block);
    double sum = 0.0;
    double parts = 0.0;
    for (std::size_t index = 0; index < together.size(); ++index) {
        if (!together[index]) {
            continue;
        }
        sum += sums[index];
        for (const std::span<const double> track : tracks) {
            parts += track[index];
        }
    }
    return summingGain(sum, parts);
}

} // namespace orizon::engine
