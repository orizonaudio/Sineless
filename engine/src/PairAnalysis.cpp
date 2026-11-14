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
// can run them as vector lanes; each sums an eighth of the terms. For the filters that read the
// wave between samples, whose few taps float holds well; a match is measured with exactDot.
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

// The dot product summed in double, in the lanes energyOf sums a track's squares in: the match
// between two tracks is measured with it, so that a track and its exact copy give a gain of
// exactly 1 and null completely. In float the sum over a block of low material, whose products
// all have one sign, is off by a few parts in ten million, which is a null of -126 dB.
double exactDot(const float* a, const float* b, std::size_t count)
{
    constexpr std::size_t kLanes = 8;
    std::array<double, kLanes> sums{};
    std::size_t i = 0;
    for (; i + kLanes <= count; i += kLanes) {
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            sums[lane] += static_cast<double>(a[i + lane]) * b[i + lane];
        }
    }
    double total = 0.0;
    for (const double sum : sums) {
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
                       std::size_t block, bool align)
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
    aligning = align;

    // The offsets searched, and a transform that holds them: `stride` samples of one track a
    // time against those of the other and the lags either side of them, so that none wraps
    // round. One block when a transform holds the whole stretch. An alignment takes the
    // correlation kLagMargin lags beyond its reach as well, which the wave between its lags is
    // read from at the reach's edge.
    reach = static_cast<std::ptrdiff_t>(std::min(maxOffset, length / 2));
    computed = reach + (aligning ? static_cast<std::ptrdiff_t>(kLagMargin) : 0);
    const std::size_t margin = 2 * static_cast<std::size_t>(computed);
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

void PairSearch::beginMeasure(std::ptrdiff_t offset, double fraction)
{
    measured = {.offset = offset};
    cursor = 0;
    if (fraction > 0.0) {
        delayFilter.weightsAt(fraction, measured.taps);
    }
    if (offset == 0 && measured.taps.empty()) {
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

// The second track as the match being measured shifts it, where it meets `size` samples of the
// first from `first`: its samples a whole number of samples away, or, for a shift between
// samples, the wave between them there (the filter Align draws with); silence where it has none.
void PairSearch::shiftedBlock(std::size_t first, std::size_t size, std::span<float> into)
{
    const auto length = static_cast<std::ptrdiff_t>(trackB.size());
    const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(first) + measured.offset;
    const auto copy = [&](std::ptrdiff_t from, std::size_t count, float* out) {
        const std::ptrdiff_t begin = std::clamp<std::ptrdiff_t>(from, 0, length);
        const std::ptrdiff_t end =
            std::clamp<std::ptrdiff_t>(from + static_cast<std::ptrdiff_t>(count), begin, length);
        std::fill(out, out + count, 0.0f);
        std::copy(trackB.begin() + begin, trackB.begin() + end, out + (begin - from));
    };
    if (measured.taps.empty()) {
        copy(start, size, into.data());
        return;
    }
    // Point i is the wave a fraction of a sample after sample start + i, read from the samples
    // start + i + 1 - reach to start + i + reach.
    const std::size_t taps = measured.taps.size();
    const std::size_t around = taps / 2;
    neighbours.resize(size + taps);
    copy(start + 1 - static_cast<std::ptrdiff_t>(around), size + taps, neighbours.data());
    for (std::size_t i = 0; i < size; ++i) {
        into[i] = static_cast<float>(dot(measured.taps.data(), neighbours.data() + i, taps));
    }
}

// How the tracks match with the second shifted as the measure says: over the blocks of the first
// where both then sound (elsewhere a sample of one meets silence in the other, which would water
// the match down), their energies and their dot product, each block's added in double. Two passes
// when shifted: the second track's energy per block as shifted, which says where both sound,
// then the sums.
bool PairSearch::measure(std::size_t& budget)
{
    const std::size_t length = trackA.size();
    const std::size_t blocks = energiesA.size();
    // A shift between samples reads a filter's taps for each.
    const std::size_t perSample = 1 + measured.taps.size();
    const auto sizeOf = [&](std::size_t index) {
        return std::min(blockLength, length - index * blockLength);
    };
    if (!measured.placed) {
        while (cursor < blocks) {
            const std::size_t size = sizeOf(cursor);
            shiftedBlock(cursor * blockLength, size, keptB);
            measured.shifted[cursor] = energyOf(std::span(keptB).first(size));
            ++cursor;
            spend(budget, perSample * size);
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
        const std::size_t first = cursor * blockLength;
        const std::size_t size = sizeOf(cursor);
        if (measured.together[cursor]) {
            shiftedBlock(first, size, keptB);
            // A sample that is not finite (from a broken signal upstream) counts as silence in
            // both, so the match is measured around it. Both finite, tested without a branch:
            // their difference is finite then, and NaN or infinite when either is not (or both
            // are the same infinity); only values near float's largest, far beyond any audio,
            // would overflow it.
            // Only where the second track, as shifted, has samples of its own: past its ends the
            // first would meet silence, which lowers the match by the shift's share of the
            // stretch, more than a lag changes it on low material, and would have a late copy's
            // best lag read a sample short.
            const auto reads = static_cast<std::ptrdiff_t>(measured.taps.size() / 2);
            const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(first) + measured.offset;
            // Through a filter, point i reads from sample start + i + 1 - reads on; shifted by
            // whole samples, sample start + i alone.
            const std::ptrdiff_t from =
                std::max<std::ptrdiff_t>(0, (reads > 0 ? reads - 1 : 0) - start);
            const std::ptrdiff_t to =
                std::min(static_cast<std::ptrdiff_t>(size),
                         static_cast<std::ptrdiff_t>(trackB.size()) - reads - start);
            for (std::size_t i = 0; i < size; ++i) {
                const float x = trackA[first + i];
                const float y = keptB[i];
                const auto at = static_cast<std::ptrdiff_t>(i);
                const bool keep =
                    at >= from && at < to && std::abs(x - y) <= std::numeric_limits<float>::max();
                keptA[i] = keep ? x : 0.0f;
                keptB[i] = keep ? y : 0.0f;
            }
            measured.energyA += energyOf(std::span(keptA).first(size));
            measured.energyB += energyOf(std::span(keptB).first(size));
            measured.dot += exactDot(keptA.data(), keptB.data(), size);
        }
        ++cursor;
        spend(budget, (2 + perSample) * size);
        if (budget == 0 && cursor < blocks) {
            return false;
        }
    }
    return true;
}

double PairSearch::strength() const
{
    const double norm = std::sqrt(measured.energyA * measured.energyB);
    return norm > 0.0 ? measured.dot / norm : 0.0;
}

// One block of the cross-correlation: `stride` samples of the first track and the samples of the
// second around them, the lags computed either side, as one complex signal, whose transform holds
// both spectra; the first's conjugate times the second's is added to the running cross-spectrum.
void PairSearch::correlateBlock(std::size_t& budget)
{
    const std::size_t size = fft.size();
    const std::size_t count = std::min(stride, trackA.size() - cursor);
    const std::size_t around = count + 2 * static_cast<std::size_t>(computed);
    const auto first = static_cast<std::ptrdiff_t>(cursor);
    copyGated(trackA, audibleA, blockLength, first, count, real.data());
    std::fill(real.begin() + static_cast<std::ptrdiff_t>(count), real.end(), 0.0f);
    copyGated(trackB, audibleB, blockLength, first - computed, around, imaginary.data());
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

// The correlation between its lags, four points a lag over the reach: a peak that lies between
// two samples (as a delay does that is not a whole number of them) shows at no whole lag on
// bright material, where the neighbouring lags can even read stronger with the sign flipped.
bool PairSearch::interpolateLags(std::size_t& budget)
{
    const auto lagCount = static_cast<std::size_t>(2 * reach);
    constexpr std::size_t kTaps = 2 * kBetweenReach;
    while (cursor < lagCount) {
        // Lag cursor - reach; its neighbours start kBetweenReach - 1 lags before it.
        const float* const from = padded.data() + cursor + kLagMargin + 1 - kBetweenReach;
        fine[kBetween * cursor] = padded[cursor + kLagMargin];
        for (std::size_t phase = 1; phase < kBetween; ++phase) {
            fine[kBetween * cursor + phase] =
                static_cast<float>(dot(between.weights(kBetween, phase).data(), from, kTaps));
        }
        ++cursor;
        spend(budget, (kBetween - 1) * kTaps);
        if (budget == 0 && cursor < lagCount) {
            return false;
        }
    }
    fine.back() = padded[lagCount + kLagMargin];
    return true;
}

// The delay the correlation points to, to a fraction of a sample: the peak picked on the wave
// between the lags, then, within a quarter of a sample of it, the top of that wave read with a
// long filter at thirty-seconds of a sample and a parabola through the three points around it.
double PairSearch::estimateDelay()
{
    const std::ptrdiff_t picked = pickPeak(fine);
    constexpr auto kSteps = static_cast<std::ptrdiff_t>(kRefine / kBetween);
    constexpr std::size_t kTaps = 2 * kLagMargin;
    // In thirty-seconds of a lag from -reach.
    const std::ptrdiff_t centre = (picked + static_cast<std::ptrdiff_t>(kBetween) * reach) * kSteps;
    const std::ptrdiff_t last = 2 * reach * static_cast<std::ptrdiff_t>(kRefine);
    const auto at = [&](std::ptrdiff_t position) {
        const std::ptrdiff_t clamped = std::clamp<std::ptrdiff_t>(position, 0, last);
        const auto lag = static_cast<std::size_t>(clamped) / kRefine;
        const auto phase = static_cast<std::size_t>(clamped) % kRefine;
        // The lag's neighbours start kLagMargin - 1 lags before it; the padding holds them.
        return static_cast<double>(
            dot(refine.weights(kRefine, phase).data(), padded.data() + lag + 1, kTaps));
    };
    std::ptrdiff_t best = centre;
    double bestValue = at(centre);
    for (std::ptrdiff_t step = -kSteps; step <= kSteps; ++step) {
        const double value = at(centre + step);
        if (std::abs(value) > std::abs(bestValue)) {
            best = std::clamp<std::ptrdiff_t>(centre + step, 0, last);
            bestValue = value;
        }
    }
    // The parabola's vertex, of the three values about the best with the best's sign.
    const double sign = bestValue < 0.0 ? -1.0 : 1.0;
    estimateSign = sign;
    const double before = sign * at(best - 1);
    const double after = sign * at(best + 1);
    const double middle = sign * bestValue;
    const double bend = before - 2.0 * middle + after;
    const double vertex = bend < 0.0 ? 0.5 * (before - after) / bend : 0.0;
    const double delay =
        (static_cast<double>(best) + std::clamp(vertex, -0.5, 0.5)) / static_cast<double>(kRefine) -
        static_cast<double>(reach);
    return std::clamp(delay, -static_cast<double>(reach), static_cast<double>(reach));
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
        // The correlation at each lag computed: the lag -computed comes out first.
        fft.inverse(crossReal, crossImaginary);
        spend(budget, transformWork(fft.size()));
        if (!aligning) {
            const std::ptrdiff_t picked =
                pickPeak(std::span(crossReal).first(2 * static_cast<std::size_t>(reach) + 1));
            if (picked == 0) {
                finish(0, found.correlation);
                return true;
            }
            beginMeasure(picked);
            stage = Stage::kMatch;
        } else {
            // The lags within the reach, with kLagMargin more either side for the filters that
            // read between them: computed where the stretch allowed, silence beyond.
            const auto lagCount = static_cast<std::size_t>(2 * reach) + 1;
            padded.assign(lagCount + 2 * kLagMargin, 0.0f);
            const std::ptrdiff_t spare =
                static_cast<std::ptrdiff_t>(kLagMargin) - (computed - reach);
            std::copy_n(crossReal.begin(), 2 * static_cast<std::size_t>(computed) + 1,
                        padded.begin() + spare);
            fine.assign(kBetween * (lagCount - 1) + 1, 0.0f);
            cursor = 0;
            stage = Stage::kBetween;
        }
        if (budget == 0) {
            return false;
        }
    }
    if (stage == Stage::kMatch) {
        if (!measure(budget)) {
            return false;
        }
        finish(measured.offset, strength());
    }
    if (stage == Stage::kBetween) {
        if (!interpolateLags(budget)) {
            return false;
        }
        estimate = estimateDelay();
        settled = {.lag = static_cast<std::ptrdiff_t>(std::llround(estimate))};
        beginMeasure(settled.lag);
        stage = Stage::kSettle;
        if (budget == 0) {
            return false;
        }
    }
    while (stage == Stage::kSettle) {
        if (!measure(budget)) {
            return false;
        }
        settle();
        if (budget == 0 && stage == Stage::kSettle) {
            return false;
        }
    }
    if (stage == Stage::kFraction) {
        if (!measure(budget)) {
            return false;
        }
        // The fraction only where the wave can tell: where it matches better than the whole lag
        // by more than rounding. A plain latency is a whole number of samples, read as an exact
        // copy.
        constexpr double kClearly = 1.0 + 1e-6;
        if (strength() * estimateSign > settled.strength * estimateSign * kClearly) {
            finishAligned(estimate, strength(), measured.energyB);
        } else {
            finishAligned(static_cast<double>(settled.lag), settled.strength, settled.energyB);
        }
    }
    return stage == Stage::kDone;
}

// One whole lag measured: kept if it matches better than the best so far, and then the next one
// the same way; otherwise the other way from the best, and, once neither neighbour of the best
// matches better, the fraction the estimate pointed to, if it lies beside the best.
void PairSearch::settle()
{
    // Better on the peak the estimate found: with its sign. On bright material the lags next to a
    // peak between two samples match more strongly than those either side of it, flipped, and
    // are another peak.
    const double value = strength();
    const bool better = !settled.any || value * estimateSign > settled.strength * estimateSign;
    if (better) {
        const bool first = !settled.any;
        settled.any = true;
        settled.lag = measured.offset;
        settled.strength = value;
        settled.dot = measured.dot;
        settled.energyB = measured.energyB;
        if (!first) {
            // Further the same way; the lag just left is known to match less well.
            (settled.direction < 0 ? settled.triedAfter : settled.triedBefore) = true;
            (settled.direction < 0 ? settled.triedBefore : settled.triedAfter) = false;
        }
    } else {
        (settled.direction < 0 ? settled.triedBefore : settled.triedAfter) = true;
    }
    for (const int direction : {-1, 1}) {
        bool& tried = direction < 0 ? settled.triedBefore : settled.triedAfter;
        const std::ptrdiff_t lag = settled.lag + direction;
        if (tried || lag < -reach || lag > reach) {
            tried = true;
            continue;
        }
        settled.direction = direction;
        beginMeasure(lag);
        return;
    }
    const double beside = estimate - static_cast<double>(settled.lag);
    const double floor = std::floor(estimate);
    // A hundredth of a sample from a whole lag is that lag: nothing to tell apart.
    constexpr double kWhole = 0.01;
    if (std::abs(beside) < 1.0 && std::abs(beside) > kWhole) {
        beginMeasure(static_cast<std::ptrdiff_t>(floor), estimate - floor);
        stage = Stage::kFraction;
        return;
    }
    finishAligned(static_cast<double>(settled.lag), settled.strength, settled.energyB);
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

void PairSearch::finishAligned(double delay, double value, double energyB)
{
    found.related = std::abs(value) >= kRelatedMatch && energyB > 0.0;
    if (found.related) {
        found.delay = delay;
        found.offset = static_cast<std::ptrdiff_t>(std::llround(delay));
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

PairAnalysis alignPair(std::span<const float> a, std::span<const float> b, std::size_t maxOffset,
                       std::size_t block)
{
    PairSearch search;
    search.start(a, b, maxOffset, block, true);
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

double GainMeasure::depth(std::size_t reference) const
{
    if (tracks.size() < 2 || block == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return nullDepthOfBlocks(spansOf(energies), sums, block, reference);
}

double nullDepthOfBlocks(std::span<const std::span<const double>> tracks,
                         std::span<const double> sums, std::size_t block, std::size_t reference)
{
    if (tracks.size() < 2 || reference >= tracks.size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // The loudest of the other tracks per block: with the reference, the two whose sounding
    // together is judged.
    const std::span<const double> own = tracks[reference];
    std::vector<double> others(own.size(), 0.0);
    for (std::size_t track = 0; track < tracks.size(); ++track) {
        if (track == reference) {
            continue;
        }
        for (std::size_t index = 0; index < others.size(); ++index) {
            others[index] = std::max(others[index], tracks[track][index]);
        }
    }
    const std::array<std::span<const double>, 2> pair{own, others};
    const std::vector<bool> together = togetherBlocks(pair, block);
    double difference = 0.0;
    double level = 0.0;
    for (std::size_t index = 0; index < together.size(); ++index) {
        if (together[index]) {
            difference += sums[index];
            level += own[index];
        }
    }
    // The same ratio as a summing gain's, of the difference to this track alone: raw, with no
    // floor, so that an exact null is -infinity.
    return summingGain(difference, level);
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
