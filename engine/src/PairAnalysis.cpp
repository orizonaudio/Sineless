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

} // namespace

// Of the finite values: one that is not (from a broken signal upstream) is measured around.
// Eight independent sums, with no branch, so the compiler can run them as vector lanes: a match
// search measures a few million samples this way, and one running sum would take as long as
// the search itself.
double energyOf(std::span<const float> values)
{
    constexpr std::size_t kLanes = 8;
    std::array<double, kLanes> sums{};
    const std::size_t count = values.size();
    std::size_t i = 0;
    for (; i + kLanes <= count; i += kLanes) {
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            const double value = values[i + lane];
            sums[lane] += std::isfinite(value) ? value * value : 0.0;
        }
    }
    double total = 0.0;
    for (const double sum : sums) {
        total += sum;
    }
    for (; i < count; ++i) {
        const double value = values[i];
        total += std::isfinite(value) ? value * value : 0.0;
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

void PairSearch::start(std::span<const float> a, std::span<const float> b, std::size_t maxOffset,
                       std::size_t block)
{
    const std::size_t length = std::min(a.size(), b.size());
    maskedA.assign(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(length));
    maskedB.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(length));
    maxReach = maxOffset;
    blockLength = block == 0 ? std::max<std::size_t>(length, 1) : block;
    const std::size_t blocks = (length + blockLength - 1) / blockLength;
    energiesA.assign(blocks, 0.0);
    energiesB.assign(blocks, 0.0);
    coarseA.assign(length / kDecimation, 0.0f);
    coarseB.assign(length / kDecimation, 0.0f);
    cursor = 0;
    energyA = 0.0;
    energyB = 0.0;
    coarseEnergyA = 0.0;
    coarseEnergyB = 0.0;
    dotAtNone = 0.0;
    found = {};
    stage = Stage::kEnergies;
}

// Two passes over the stretch, a block at a time: each track's energy per block, then, once it
// is known where both sound, only there (elsewhere a sample of one meets silence in the other,
// which would water the correlation down) the samples kept, measured and decimated.
bool PairSearch::prepare(std::size_t& budget)
{
    const std::size_t length = maskedA.size();
    const std::size_t blocks = energiesA.size();
    const auto spend = [&](std::size_t cost) {
        budget -= std::min(budget, cost);
        done += cost;
    };
    while (stage == Stage::kEnergies) {
        if (cursor == blocks) {
            const std::array<std::span<const double>, 2> energies{energiesA, energiesB};
            together = blocks == 0 ? std::vector<bool>{} : togetherBlocks(energies, blockLength);
            cursor = 0;
            stage = Stage::kMask;
            break;
        }
        const std::size_t first = cursor * blockLength;
        const std::size_t size = std::min(blockLength, length - first);
        energiesA[cursor] = energyOf(std::span(maskedA).subspan(first, size));
        energiesB[cursor] = energyOf(std::span(maskedB).subspan(first, size));
        ++cursor;
        spend(2 * size);
        if (budget == 0) {
            return false;
        }
    }
    while (cursor < blocks) {
        const std::size_t first = cursor * blockLength;
        const std::size_t end = std::min(length, first + blockLength);
        const auto from = static_cast<std::ptrdiff_t>(first);
        const auto to = static_cast<std::ptrdiff_t>(end);
        if (!together[cursor]) {
            std::fill(maskedA.begin() + from, maskedA.begin() + to, 0.0f);
            std::fill(maskedB.begin() + from, maskedB.begin() + to, 0.0f);
        } else {
            // A sample that is not finite (from a broken signal upstream) counts as silence in
            // both, so the match is measured around it. Block by block, so the copy runs as
            // vector lanes.
            for (std::size_t i = first; i < end; ++i) {
                // Both finite, tested without a branch: their difference is finite then, and NaN
                // or infinite when either is not (or both are the same infinity); only values
                // near float's largest, far beyond any audio, would overflow it.
                const float x = maskedA[i];
                const float y = maskedB[i];
                const bool keep = std::abs(x - y) <= std::numeric_limits<float>::max();
                maskedA[i] = keep ? x : 0.0f;
                maskedB[i] = keep ? y : 0.0f;
            }
            const std::span<const float> keptA = std::span(maskedA).subspan(first, end - first);
            const std::span<const float> keptB = std::span(maskedB).subspan(first, end - first);
            energyA += energyOf(keptA);
            energyB += energyOf(keptB);
            dotAtNone += dot(keptA.data(), keptB.data(), keptA.size());
        }
        // The coarse samples whose eight samples are all kept now: from the one that straddles
        // this block's start, whose first samples the block before kept, when a block is not a
        // whole number of them (441 samples, at 44.1 kHz).
        const std::size_t coarseFirst = first / kDecimation;
        const std::size_t coarseEnd = std::min(coarseA.size(), end / kDecimation);
        if (coarseFirst < coarseEnd) {
            const std::size_t count = coarseEnd - coarseFirst;
            const auto pieceA = std::span(coarseA).subspan(coarseFirst, count);
            const auto pieceB = std::span(coarseB).subspan(coarseFirst, count);
            decimate(std::span(maskedA).subspan(coarseFirst * kDecimation), pieceA);
            decimate(std::span(maskedB).subspan(coarseFirst * kDecimation), pieceB);
            coarseEnergyA += energyOf(pieceA);
            coarseEnergyB += energyOf(pieceB);
        }
        ++cursor;
        spend(3 * (end - first));
        if (budget == 0 && cursor < blocks) {
            return false;
        }
    }

    norm = std::sqrt(energyA * energyB);
    if (norm <= 0.0) {
        stage = Stage::kDone;
        return true;
    }
    found.measured = true;
    found.correlation = dotAtNone / norm;

    // Coarse search on decimated copies, then a fine one around the coarse match.
    reach = static_cast<std::ptrdiff_t>(std::min(maxReach, length / 2));
    const auto coarseReach = reach / static_cast<std::ptrdiff_t>(kDecimation);
    coarseNorm = std::sqrt(coarseEnergyA * coarseEnergyB);
    beginScan(scan, -coarseReach, coarseReach);
    stage = Stage::kCoarse;
    return true;
}

void PairSearch::beginScan(Scan& scan, std::ptrdiff_t from, std::ptrdiff_t to)
{
    scan = {.from = from, .to = to, .nearest = std::clamp<std::ptrdiff_t>(0, from, to)};
}

// The offset in [from, to] with the strongest match. Offsets are visited from the smallest shift
// out, and a larger shift wins only if its match is clearly (more than 2%) stronger.
bool PairSearch::advance(std::span<const float> a, std::span<const float> b, double scale,
                         std::size_t& budget)
{
    const auto size = static_cast<std::ptrdiff_t>(a.size());
    // One offset's cost: the samples both have at it.
    const auto spend = [&](std::ptrdiff_t offset) {
        const auto cost =
            static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, size - std::abs(offset)));
        budget -= std::min(budget, cost);
        done += cost;
    };
    while (true) {
        if (scan.step == 0) {
            scan.bestOffset = scan.nearest;
            scan.bestValue = correlationAt(a, b, scan.nearest, scale);
            spend(scan.nearest);
            scan.step = 1;
        } else {
            if (scan.nearest - scan.step < scan.from && scan.nearest + scan.step > scan.to) {
                return true;
            }
            const std::ptrdiff_t offset =
                scan.side == 0 ? scan.nearest - scan.step : scan.nearest + scan.step;
            if (scan.side == 1) {
                scan.side = 0;
                ++scan.step;
            } else {
                scan.side = 1;
            }
            if (offset < scan.from || offset > scan.to) {
                continue;
            }
            const double value = correlationAt(a, b, offset, scale);
            spend(offset);
            if (std::abs(value) * kPreferSmallerShift > std::abs(scan.bestValue)) {
                scan.bestOffset = offset;
                scan.bestValue = value;
            }
        }
        if (budget == 0) {
            return scan.nearest - scan.step < scan.from && scan.nearest + scan.step > scan.to;
        }
    }
}

bool PairSearch::step(std::size_t budget)
{
    if (stage == Stage::kEnergies || stage == Stage::kMask) {
        if (!prepare(budget)) {
            return false;
        }
        if (stage == Stage::kDone) {
            return true;
        }
        if (budget == 0) {
            return false;
        }
    }
    if (stage == Stage::kCoarse) {
        if (!advance(coarseA, coarseB, coarseNorm, budget)) {
            return false;
        }
        const auto centre = scan.bestOffset * static_cast<std::ptrdiff_t>(kDecimation);
        const auto span = static_cast<std::ptrdiff_t>(kDecimation);
        beginScan(scan, std::max(-reach, centre - span), std::min(reach, centre + span));
        stage = Stage::kFine;
        if (budget == 0) {
            return false;
        }
    }
    if (stage == Stage::kFine) {
        if (!advance(maskedA, maskedB, norm, budget)) {
            return false;
        }
        finish();
    }
    return stage == Stage::kDone;
}

void PairSearch::finish()
{
    double value = scan.bestValue;
    std::ptrdiff_t offset = scan.bestOffset;
    // A shift that is not clearly better than none is not worth suggesting.
    if (std::abs(value) * kPreferSmallerShift <= std::abs(found.correlation)) {
        offset = 0;
        value = found.correlation;
    }
    found.related = std::abs(value) >= kRelatedMatch;
    if (found.related) {
        found.offset = offset;
        found.inverted = value < 0.0;
    }
    stage = Stage::kDone;
}

PairAnalysis analysePair(std::span<const float> a, std::span<const float> b, std::size_t maxOffset,
                         std::size_t block)
{
    PairSearch search;
    search.start(a, b, maxOffset, block);
    while (!search.step(std::numeric_limits<std::size_t>::max())) {
    }
    return search.result();
}

double overlapGain(std::span<const std::span<const float>> tracks, std::size_t block)
{
    GainMeasure measure;
    measure.start(tracks, block);
    while (!measure.step(std::numeric_limits<std::size_t>::max())) {
    }
    return measure.result();
}

void GainMeasure::start(std::span<const std::span<const float>> measured, std::size_t blockSize)
{
    tracks.assign(measured.begin(), measured.end());
    block = blockSize;
    next = 0;
    if (tracks.size() < 2 || block == 0) {
        gain = 0.0;
        stage = Stage::kDone;
        return;
    }
    length = tracks.front().size();
    totals.assign(length, 0.0);
    energies.resize(tracks.size());
    stage = Stage::kFused;
}

bool GainMeasure::step(std::size_t budget)
{
    if (stage == Stage::kDone) {
        return true; // Also with fewer than two tracks, or no block, which measure nothing.
    }
    const std::size_t blocks = (length + block - 1) / block;
    // Each block's energy, of the values valueAt gives where keep says, eight sums at a time so
    // that they run as vector lanes; along the way, visit sees each position and its value.
    const auto blockEnergies = [&](std::vector<double>& into, const auto& valueAt, const auto& keep,
                                   const auto& visit) {
        into.assign(blocks, 0.0);
        for (std::size_t index = 0; index < blocks; ++index) {
            const std::size_t first = index * block;
            const std::size_t last = std::min(first + block, length);
            constexpr std::size_t kLanes = 8;
            std::array<double, kLanes> lanes{};
            std::size_t i = first;
            for (; i + kLanes <= last; i += kLanes) {
                for (std::size_t lane = 0; lane < kLanes; ++lane) {
                    const double value = valueAt(i + lane);
                    visit(i + lane, value);
                    const double kept = keep(i + lane) ? value : 0.0;
                    lanes[lane] += kept * kept;
                }
            }
            double energy = 0.0;
            for (const double sum : lanes) {
                energy += sum;
            }
            for (; i < last; ++i) {
                const double value = valueAt(i);
                visit(i, value);
                const double kept = keep(i) ? value : 0.0;
                energy += kept * kept;
            }
            into[index] = energy;
        }
    };
    const auto always = [](std::size_t) { return true; };
    const auto nothing = [](std::size_t, double) {};
    const auto finiteHere = [&](std::size_t i) { return broken[i] == 0; };
    bool passed = false; // At least one pass a step, whatever the budget.
    while (stage != Stage::kDone && (!passed || budget > 0)) {
        passed = true;
        budget -= std::min(budget, length);
        done += length;
        if (stage == Stage::kFused) {
            // A track's energies and its part of the sum in one pass, as long as every value is
            // finite: value - value is 0 for each one that is, and not for one that is not.
            if (next < tracks.size()) {
                const std::span<const float> track = tracks[next];
                double check = 0.0;
                blockEnergies(
                    energies[next], [&](std::size_t i) { return static_cast<double>(track[i]); },
                    always,
                    [&](std::size_t i, double value) {
                        totals[i] += value;
                        check += value - value;
                    });
                ++next;
                if (check != 0.0) {
                    // A broken signal upstream: measured again around the positions it breaks.
                    totals.assign(length, 0.0);
                    broken.assign(length, 0);
                    stage = Stage::kTotals;
                    next = 0;
                }
            } else {
                blockEnergies(sums, [&](std::size_t i) { return totals[i]; }, always, nothing);
                gain = overlapGainOfBlocks(spansOf(energies), sums, block);
                stage = Stage::kDone;
            }
        } else if (stage == Stage::kTotals) {
            // Positions where a track is not finite are left out of the sum and of every part
            // alike, so that the gain is measured around them.
            const std::span<const float> track = tracks[next];
            for (std::size_t i = 0; i < length; ++i) {
                const double value = track[i];
                const bool finite = std::abs(value) <= std::numeric_limits<double>::max();
                broken[i] = static_cast<std::uint8_t>(broken[i] | (finite ? 0U : 1U));
                totals[i] += finite ? value : 0.0;
            }
            if (++next == tracks.size()) {
                stage = Stage::kEnergies;
                next = 0;
            }
        } else if (next < tracks.size()) {
            const std::span<const float> track = tracks[next++];
            blockEnergies(
                energies[next - 1], [&](std::size_t i) { return static_cast<double>(track[i]); },
                finiteHere, nothing);
        } else {
            blockEnergies(sums, [&](std::size_t i) { return totals[i]; }, finiteHere, nothing);
            gain = overlapGainOfBlocks(spansOf(energies), sums, block);
            stage = Stage::kDone;
        }
    }
    return stage == Stage::kDone;
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
