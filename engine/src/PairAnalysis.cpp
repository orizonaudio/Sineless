#include "engine/PairAnalysis.h"

#include "protocol/Protocol.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>

namespace orizon::engine {

namespace {

// Which of two matches to prefer: a larger shift only if its match is more than 2% stronger. It
// chooses between the periods of a steady tone, and between a polarity flip and a half-period
// shift, which match alike.
constexpr double kPreferSmallerShift = 0.98;
// The weakest best match that still counts as the same sound arriving twice.
constexpr double kRelatedMatch = 0.3;
// Overlap: tracks sound together within 20 dB of each other, and above 60 dB below the loudest.
constexpr double kTogether = 0.01;
constexpr double kAudible = 1e-6;
// The smallest transform of a search: smaller ones spend more of each on the overlap between
// blocks, which makes a search over a short reach slower, not faster.
constexpr std::size_t kSmallestTransform = std::size_t{1} << 14U;
// A track's mean power (RMS squared) at the analysis gate: quieter blocks are never analysed.
double silentPower()
{
    return std::pow(10.0, protocol::kAnalysisGateDbfs / 10.0);
}

// A transform's work, in the dot product's multiply-adds: each of its size / 2 butterflies a pass
// takes about as long as four of them (measured: 0.09 ms for 16384 points, 7.7 ms for 524288,
// against 0.2 to 0.4 ns a multiply-add), over log2(size) passes.
std::size_t transformWork(std::size_t size)
{
    return 2 * size * static_cast<std::size_t>(std::countr_zero(size));
}

// The dot product of a[0, count) and b[0, count). Eight independent float sums, so the compiler
// can run them as vector lanes; each sums an eighth of the terms. Over one block of a search
// (10 ms) their rounding stays far below what a match needs; the blocks' sums are added in
// double.
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

// The lag the correlation `values` points to, of lags -reach to reach (`values` holds 2 * reach
// + 1 of them, the middle one at no shift). Which peak: from the smallest shift out, a larger
// shift wins only if its match is clearly (more than 2%) stronger. Then that peak's top: uphill
// from there, since the first lag within 2% of a broad peak's top lies well short of it (some
// twenty samples on a bass note).
std::ptrdiff_t pickPeak(std::span<const float> values)
{
    const auto reach = static_cast<std::ptrdiff_t>(values.size() / 2);
    const auto strength = [&](std::ptrdiff_t lag) {
        return static_cast<double>(std::abs(values[static_cast<std::size_t>(lag + reach)]));
    };
    std::ptrdiff_t best = 0;
    for (std::ptrdiff_t shift = 1; shift <= reach; ++shift) {
        for (const std::ptrdiff_t lag : {-shift, shift}) {
            if (strength(lag) * kPreferSmallerShift > strength(best)) {
                best = lag;
            }
        }
    }
    while (true) {
        std::ptrdiff_t higher = best;
        if (best > -reach && strength(best - 1) > strength(higher)) {
            higher = best - 1;
        }
        if (best < reach && strength(best + 1) > strength(higher)) {
            higher = best + 1;
        }
        if (higher == best) {
            return best;
        }
        best = higher;
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

// Which blocks of one track are searched, from its energy per block of `block` samples: those
// above the analysis gate and not more than 60 dB below its loudest. A track is gated on its own
// for the search, not where both sound: a short hit keeps its samples however late its copy
// arrives in the other track.
std::vector<bool> audibleBlocks(std::span<const double> energies, std::size_t block)
{
    const double loudest = energies.empty() ? 0.0 : *std::ranges::max_element(energies);
    const double silentBlock = silentPower() * static_cast<double>(block);
    std::vector<bool> audible(energies.size());
    for (std::size_t index = 0; index < energies.size(); ++index) {
        audible[index] = energies[index] > kAudible * loudest && energies[index] > silentBlock;
    }
    return audible;
}

// Copies `count` samples of `track` from position `from` (which may lie before its start, or the
// copy run past its end: silence there) into `into`, with silence too in the blocks of `block`
// samples that `audible` leaves out and for samples that are not finite.
void copyGated(std::span<const float> track, const std::vector<bool>& audible, std::size_t block,
               std::ptrdiff_t from, std::size_t count, float* into)
{
    const auto length = static_cast<std::ptrdiff_t>(track.size());
    const auto stop = from + static_cast<std::ptrdiff_t>(count);
    const std::ptrdiff_t first = std::clamp<std::ptrdiff_t>(from, 0, length);
    const std::ptrdiff_t last = std::clamp<std::ptrdiff_t>(stop, first, length);
    std::fill(into, into + (first - from), 0.0f);
    std::fill(into + (last - from), into + count, 0.0f);
    std::ptrdiff_t position = first;
    while (position < last) {
        const auto index = static_cast<std::size_t>(position) / block;
        const std::ptrdiff_t blockEnd =
            std::min(last, static_cast<std::ptrdiff_t>((index + 1) * block));
        float* const out = into + (position - from);
        if (!audible[index]) {
            std::fill(out, out + (blockEnd - position), 0.0f);
        } else {
            const float* const in = track.data() + position;
            for (std::ptrdiff_t i = 0; i < blockEnd - position; ++i) {
                // Finite, tested without a branch, so the copy runs as vector lanes.
                const float value = in[i];
                out[i] = std::abs(value) <= std::numeric_limits<float>::max() ? value : 0.0f;
            }
        }
        position = blockEnd;
    }
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
    trackA.assign(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(length));
    trackB.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(length));
    blockLength = block == 0 ? std::max<std::size_t>(length, 1) : block;
    const std::size_t blocks = (length + blockLength - 1) / blockLength;
    energiesA.assign(blocks, 0.0);
    energiesB.assign(blocks, 0.0);
    keptA.assign(blockLength, 0.0f);
    keptB.assign(blockLength, 0.0f);
    cursor = 0;
    found = {};

    // The offsets searched, and a transform that holds them: `stride` samples of one track a
    // time against those of the other and the reach either side of them, so that no offset
    // within the reach wraps round. One block when a transform holds the whole stretch.
    reach = static_cast<std::ptrdiff_t>(std::min(maxOffset, length / 2));
    const std::size_t margin = 2 * static_cast<std::size_t>(reach);
    std::size_t size = std::max(std::bit_ceil(2 * margin), kSmallestTransform);
    if (length + margin <= size) {
        size = std::bit_ceil(std::max<std::size_t>(length + margin, 2));
        stride = std::max<std::size_t>(length, 1);
    } else {
        stride = size - margin;
    }
    if (fft.size() != size) {
        fft = Fft(size);
    }
    real.assign(size, 0.0f);
    imaginary.assign(size, 0.0f);
    crossReal.assign(size, 0.0f);
    crossImaginary.assign(size, 0.0f);
    stage = Stage::kEnergies;
}

void PairSearch::spend(std::size_t& budget, std::size_t cost)
{
    budget -= std::min(budget, cost);
    done += cost;
    progressed = true;
}

// Whether the step goes on to a transform now: always when it has done nothing yet, so that every
// step makes progress; otherwise only with the budget for it, since a transform cannot stop
// halfway and one begun on the last of a budget would run far beyond it.
bool PairSearch::affords(std::size_t budget) const
{
    return !progressed || budget >= transformWork(fft.size());
}

// Each track's energy per block, a block at a time; then which blocks of each the search reads.
bool PairSearch::measureEnergies(std::size_t& budget)
{
    const std::size_t length = trackA.size();
    const std::size_t blocks = energiesA.size();
    while (cursor < blocks) {
        const std::size_t first = cursor * blockLength;
        const std::size_t size = std::min(blockLength, length - first);
        energiesA[cursor] = energyOf(std::span(trackA).subspan(first, size));
        energiesB[cursor] = energyOf(std::span(trackB).subspan(first, size));
        ++cursor;
        spend(budget, 2 * size);
        if (budget == 0 && cursor < blocks) {
            return false;
        }
    }
    audibleA = audibleBlocks(energiesA, blockLength);
    audibleB = audibleBlocks(energiesB, blockLength);
    return true;
}

void PairSearch::beginMeasure(std::ptrdiff_t offset)
{
    measured = {.offset = offset};
    cursor = 0;
    if (offset == 0) {
        measured.shifted = energiesB;
        measured.together = energiesA.empty() ? std::vector<bool>{} : togetherOf(measured.shifted);
        measured.placed = true;
    } else {
        measured.shifted.assign(energiesA.size(), 0.0);
    }
}

std::vector<bool> PairSearch::togetherOf(const std::vector<double>& shifted) const
{
    const std::array<std::span<const double>, 2> energies{energiesA, shifted};
    return togetherBlocks(energies, blockLength);
}

// How the tracks match with the second shifted by the offset: over the blocks of the first where
// both then sound (elsewhere a sample of one meets silence in the other, which would water the
// match down), their energies and their dot product, each block's added in double. Two passes
// when shifted: the second track's energy per block as shifted, which says where both sound,
// then the sums.
bool PairSearch::measure(std::size_t& budget)
{
    const auto length = static_cast<std::ptrdiff_t>(trackA.size());
    const std::size_t blocks = energiesA.size();
    // The second track's samples that meet block `index` of the first, as [from, to) of it, and
    // how many positions of the block lie before them.
    struct Meeting
    {
        std::size_t first;
        std::size_t size;
        std::ptrdiff_t from;
        std::ptrdiff_t to;
    };
    const auto meeting = [&](std::size_t index) {
        const std::size_t first = index * blockLength;
        const std::size_t size = std::min(blockLength, static_cast<std::size_t>(length) - first);
        const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(first) + measured.offset;
        const std::ptrdiff_t from = std::clamp<std::ptrdiff_t>(start, 0, length);
        const std::ptrdiff_t to =
            std::clamp<std::ptrdiff_t>(start + static_cast<std::ptrdiff_t>(size), from, length);
        return Meeting{.first = first, .size = size, .from = from, .to = to};
    };
    if (!measured.placed) {
        while (cursor < blocks) {
            const Meeting met = meeting(cursor);
            measured.shifted[cursor] = energyOf(std::span(trackB).subspan(
                static_cast<std::size_t>(met.from), static_cast<std::size_t>(met.to - met.from)));
            ++cursor;
            spend(budget, met.size);
            if (budget == 0 && cursor < blocks) {
                return false;
            }
        }
        measured.together = togetherOf(measured.shifted);
        measured.placed = true;
        cursor = 0;
        if (budget == 0 && blocks > 0) {
            return false;
        }
    }
    while (cursor < blocks) {
        const Meeting met = meeting(cursor);
        if (measured.together[cursor]) {
            // The second track's samples where they fall in the block, silence around them.
            const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(met.first) + measured.offset;
            std::fill(keptB.begin(), keptB.begin() + static_cast<std::ptrdiff_t>(met.size), 0.0f);
            std::copy(trackB.begin() + met.from, trackB.begin() + met.to,
                      keptB.begin() + (met.from - start));
            // A sample that is not finite (from a broken signal upstream) counts as silence in
            // both, so the match is measured around it. Both finite, tested without a branch:
            // their difference is finite then, and NaN or infinite when either is not (or both
            // are the same infinity); only values near float's largest, far beyond any audio,
            // would overflow it.
            for (std::size_t i = 0; i < met.size; ++i) {
                const float x = trackA[met.first + i];
                const float y = keptB[i];
                const bool keep = std::abs(x - y) <= std::numeric_limits<float>::max();
                keptA[i] = keep ? x : 0.0f;
                keptB[i] = keep ? y : 0.0f;
            }
            measured.energyA += energyOf(std::span(keptA).first(met.size));
            measured.energyB += energyOf(std::span(keptB).first(met.size));
            measured.dot += dot(keptA.data(), keptB.data(), met.size);
        }
        ++cursor;
        spend(budget, 3 * met.size);
        if (budget == 0 && cursor < blocks) {
            return false;
        }
    }
    return true;
}

// One block of the cross-correlation: `stride` samples of the first track and the samples of the
// second around them, the reach either side, as one complex signal, whose transform holds both
// spectra; the first's conjugate times the second's is added to the running cross-spectrum.
void PairSearch::correlateBlock(std::size_t& budget)
{
    const std::size_t size = fft.size();
    const std::size_t count = std::min(stride, trackA.size() - cursor);
    const std::size_t around = count + 2 * static_cast<std::size_t>(reach);
    const auto first = static_cast<std::ptrdiff_t>(cursor);
    copyGated(trackA, audibleA, blockLength, first, count, real.data());
    std::fill(real.begin() + static_cast<std::ptrdiff_t>(count), real.end(), 0.0f);
    copyGated(trackB, audibleB, blockLength, first - reach, around, imaginary.data());
    std::fill(imaginary.begin() + static_cast<std::ptrdiff_t>(around), imaginary.end(), 0.0f);
    fft.forward(real, imaginary);
    // With z = a + j·b, Z[k] + conj(Z[N - k]) is twice a's spectrum and Z[k] - conj(Z[N - k])
    // twice j times b's; the product's other half is its mirror image, conjugated.
    for (std::size_t k = 0; k <= size / 2; ++k) {
        const std::size_t mirror = (size - k) & (size - 1);
        const float aRe = real[k] + real[mirror];
        const float aIm = imaginary[k] - imaginary[mirror];
        const float bRe = imaginary[k] + imaginary[mirror];
        const float bIm = real[mirror] - real[k];
        const float re = aRe * bRe + aIm * bIm;
        const float im = aRe * bIm - aIm * bRe;
        crossReal[k] += re;
        crossImaginary[k] += im;
        if (mirror != k) {
            crossReal[mirror] += re;
            crossImaginary[mirror] -= im;
        }
    }
    cursor += count;
    spend(budget, transformWork(size) + size);
}

bool PairSearch::step(std::size_t budget)
{
    progressed = false;
    if (stage == Stage::kEnergies) {
        if (!measureEnergies(budget)) {
            return false;
        }
        beginMeasure(0);
        stage = Stage::kNone;
        if (budget == 0) {
            return false;
        }
    }
    if (stage == Stage::kNone) {
        if (!measure(budget)) {
            return false;
        }
        const double norm = std::sqrt(measured.energyA * measured.energyB);
        if (norm > 0.0) {
            found.measured = true;
            found.correlation = measured.dot / norm;
        }
        // A track with nothing to search has no match anywhere.
        if (std::ranges::none_of(audibleA, std::identity{}) ||
            std::ranges::none_of(audibleB, std::identity{})) {
            stage = Stage::kDone;
            return true;
        }
        cursor = 0;
        stage = Stage::kCorrelate;
        if (budget == 0) {
            return false;
        }
    }
    while (stage == Stage::kCorrelate) {
        if (!affords(budget)) {
            return false;
        }
        correlateBlock(budget);
        if (cursor >= trackA.size()) {
            stage = Stage::kLags;
        }
        if (budget == 0) {
            return false;
        }
    }
    if (stage == Stage::kLags) {
        if (!affords(budget)) {
            return false;
        }
        // The correlation at each offset within the reach: the offset -reach comes out first.
        fft.inverse(crossReal, crossImaginary);
        spend(budget, transformWork(fft.size()));
        const std::ptrdiff_t picked =
            pickPeak(std::span(crossReal).first(2 * static_cast<std::size_t>(reach) + 1));
        if (picked == 0) {
            finish(0, found.correlation);
            return true;
        }
        beginMeasure(picked);
        stage = Stage::kMatch;
        if (budget == 0) {
            return false;
        }
    }
    if (stage == Stage::kMatch) {
        if (!measure(budget)) {
            return false;
        }
        const double norm = std::sqrt(measured.energyA * measured.energyB);
        finish(measured.offset, norm > 0.0 ? measured.dot / norm : 0.0);
    }
    return stage == Stage::kDone;
}

void PairSearch::finish(std::ptrdiff_t offset, double value)
{

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
