#include "engine/Meter.h"

#include "protocol/Protocol.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace orizon::engine {

namespace {

namespace protocol = orizon::protocol;

// The most samples read at once: bounds the working space however long the span.
constexpr std::size_t kChunkSamples = 16384;

// A block's channel, as K-weighting and correlation read it: 0 for a sample that is not finite, so
// the filters' state stays usable.
double finiteOrZero(float sample)
{
    return std::isfinite(sample) ? static_cast<double>(sample) : 0.0;
}

// Whether a join lies inside (first, last]: the samples first to last are not one stretch.
bool crossesJoin(std::span<const std::size_t> joins, std::size_t first, std::size_t last)
{
    const auto join = std::ranges::upper_bound(joins, first);
    return join != joins.end() && *join <= last;
}

// Whether two filter states are the same to within what a block's energy can show (1e-12 of
// the larger, or both all but zero).
bool sameState(const KWeighting::State& a, const KWeighting::State& b)
{
    for (std::size_t index = 0; index < a.size(); ++index) {
        const double scale = std::max({std::abs(a.at(index)), std::abs(b.at(index)), 1e-30});
        if (std::abs(a.at(index) - b.at(index)) > 1e-12 * scale) {
            return false;
        }
    }
    return true;
}

// A power as a level in LUFS, BS.1770's offset included.
double lufsOf(double meanSquare)
{
    return protocol::kLoudnessOffsetLufs + (10.0 * std::log10(meanSquare));
}

} // namespace

// The standard's coefficients, by phase (ITU-R BS.1770-5, Annex 2, the order-48, 4-phase FIR).
const std::array<std::array<double, TruePeak::kTaps>, 4> TruePeak::kPhases{{
    {0.0017089843750, 0.0109863281250, -0.0196533203125, 0.0332031250000, -0.0594482421875,
     0.1373291015625, 0.9721679687500, -0.1022949218750, 0.0476074218750, -0.0266113281250,
     0.0148925781250, -0.0083007812500},
    {-0.0291748046875, 0.0292968750000, -0.0517578125000, 0.0891113281250, -0.1665039062500,
     0.4650878906250, 0.7797851562500, -0.2003173828125, 0.1015625000000, -0.0582275390625,
     0.0330810546875, -0.0189208984375},
    {-0.0189208984375, 0.0330810546875, -0.0582275390625, 0.1015625000000, -0.2003173828125,
     0.7797851562500, 0.4650878906250, -0.1665039062500, 0.0891113281250, -0.0517578125000,
     0.0292968750000, -0.0291748046875},
    {-0.0083007812500, 0.0148925781250, -0.0266113281250, 0.0476074218750, -0.1022949218750,
     0.9721679687500, 0.1373291015625, -0.0594482421875, 0.0332031250000, -0.0196533203125,
     0.0109863281250, 0.0017089843750},
}};

KWeighting::KWeighting(double sampleRate)
{
    // The analog prototype the standard's 48 kHz coefficients come from (libebur128
    // ebur128.c:251-288; FFmpeg f_ebur128.c:396-420; Audacity EBUR128.cpp:42-80).
    {
        constexpr double kFrequency = 1681.974450955533;
        constexpr double kGainDb = 3.999843853973347;
        constexpr double kQ = 0.7071752369554196;
        const double k = std::tan(std::numbers::pi * kFrequency / sampleRate);
        const double vh = std::pow(10.0, kGainDb / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + (k / kQ) + (k * k);
        shelfStage = {.b0 = (vh + (vb * k / kQ) + (k * k)) / a0,
                      .b1 = 2.0 * ((k * k) - vh) / a0,
                      .b2 = (vh - (vb * k / kQ) + (k * k)) / a0,
                      .a1 = 2.0 * ((k * k) - 1.0) / a0,
                      .a2 = (1.0 - (k / kQ) + (k * k)) / a0};
    }
    {
        constexpr double kFrequency = 38.13547087602444;
        constexpr double kQ = 0.5003270373238773;
        const double k = std::tan(std::numbers::pi * kFrequency / sampleRate);
        const double a0 = 1.0 + (k / kQ) + (k * k);
        highPassStage = {.b0 = 1.0,
                         .b1 = -2.0,
                         .b2 = 1.0,
                         .a1 = 2.0 * ((k * k) - 1.0) / a0,
                         .a2 = (1.0 - (k / kQ) + (k * k)) / a0};
    }
}

double KWeighting::process(double sample, State& state) const
{
    const auto stage = [](const Biquad& biquad, double in, double& z1, double& z2) {
        const double out = (biquad.b0 * in) + z1;
        z1 = (biquad.b1 * in) - (biquad.a1 * out) + z2;
        z2 = (biquad.b2 * in) - (biquad.a2 * out);
        return out;
    };
    const double shelved = stage(shelfStage, sample, state[0], state[1]);
    return stage(highPassStage, shelved, state[2], state[3]);
}

TruePeak::TruePeak(double sampleRate)
{
    if (sampleRate < protocol::kTruePeakHalfRateHz) {
        usedPhases = {0, 1, 2, 3};
        phaseCount = 4;
    } else if (sampleRate < protocol::kTruePeakSampleRateHz) {
        usedPhases = {0, 2, 0, 0};
        phaseCount = 2;
    }
}

double TruePeak::between(std::span<const float, kTaps> around) const
{
    double peak = 0.0;
    const float* const newest = around.data() + (kTaps - 1);
    for (std::size_t index = 0; index < phaseCount; ++index) {
        const double* const taps = kPhases[usedPhases[index]].data();
        double point = 0.0;
        // The filter is causal: tap j weighs the sample j before the newest one read.
        for (std::size_t tap = 0; tap < kTaps; ++tap) {
            point += taps[tap] * static_cast<double>(*(newest - tap));
        }
        peak = std::max(peak, std::abs(point));
    }
    return peak;
}

void MeterBlocks::reset(std::size_t count, std::size_t blockSamples, double sampleRate,
                        int channels, std::ptrdiff_t readableFirst, std::ptrdiff_t readableEnd)
{
    spanCount = count;
    blockLength = std::max<std::size_t>(blockSamples, 1);
    if (sampleRate != rate) {
        weighting = KWeighting(sampleRate);
        interpolator = TruePeak(sampleRate);
    }
    rate = sampleRate;
    channelCount = std::clamp(channels, 1, 2);
    readFirst = std::min<std::ptrdiff_t>(readableFirst, 0);
    readEnd = std::max(readableEnd, static_cast<std::ptrdiff_t>(count));
    values.assign((count + blockLength - 1) / blockLength, Block{});
    valid = true;
}

bool MeterBlocks::prime(std::size_t samples, const Reader& read)
{
    if (!valid || values.empty()) {
        return valid;
    }
    const std::ptrdiff_t from = std::max(readFirst, -static_cast<std::ptrdiff_t>(samples));
    Block& first = values.front();
    for (std::ptrdiff_t position = from; position < 0;) {
        const auto size =
            static_cast<std::size_t>(std::min<std::ptrdiff_t>(kChunkSamples, -position));
        viewScratch.resize(size);
        leftScratch.resize(size);
        rightScratch.resize(channelCount == 2 ? size : 0);
        if (!read(position, viewScratch, leftScratch, rightScratch)) {
            valid = false;
            return false;
        }
        for (std::size_t index = 0; index < size; ++index) {
            (void)weighting.process(finiteOrZero(leftScratch[index]), first.leftState);
            if (channelCount == 2) {
                (void)weighting.process(finiteOrZero(rightScratch[index]), first.rightState);
            }
        }
        position += static_cast<std::ptrdiff_t>(size);
    }
    return true;
}

void MeterBlocks::clear()
{
    valid = false;
    spanCount = 0;
    values = {};
    viewScratch = {};
    leftScratch = {};
    rightScratch = {};
    weightedPrefix = {};
}

bool MeterBlocks::follow(std::size_t droppedBlocks, std::size_t count, std::ptrdiff_t readableFirst,
                         std::ptrdiff_t readableEnd, const Reader& read)
{
    if (!valid || droppedBlocks >= values.size()) {
        valid = false;
        return false;
    }
    values.erase(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(droppedBlocks));
    const std::size_t kept = spanCount - (droppedBlocks * blockLength);
    spanCount = std::max(count, kept);
    values.resize((spanCount + blockLength - 1) / blockLength);
    readFirst = std::min<std::ptrdiff_t>(readableFirst, 0);
    // What is read from here on: the positions added, all readable, and the samples at a view's
    // edges, which lie inside what is readable now (a hold may end before the capture did).
    readEnd = std::min(readableEnd, static_cast<std::ptrdiff_t>(spanCount));
    // From the old end: the block it lies in is measured again from its start, with the state
    // kept there, and hands the blocks after it theirs (update).
    return update(kept, spanCount, {}, read);
}

bool MeterBlocks::measureRange(std::size_t blockStart, std::size_t from, std::size_t to,
                               std::span<const std::size_t> joins, const Reader& read, Block& into,
                               KWeighting::State& left, KWeighting::State& right, bool weightedOnly)
{
    // Read from the block's start (the filters run from there) and the interpolator's neighbours
    // either side, as far as the reader has them.
    const std::ptrdiff_t readFrom =
        std::max(readFirst, static_cast<std::ptrdiff_t>(std::min(blockStart, from)) -
                                static_cast<std::ptrdiff_t>(TruePeak::kBefore));
    const std::ptrdiff_t readTo =
        std::min(readEnd, static_cast<std::ptrdiff_t>(to + TruePeak::kAfter + 1));
    const auto size = static_cast<std::size_t>(std::max<std::ptrdiff_t>(readTo - readFrom, 0));
    viewScratch.resize(weightedOnly ? 0 : size);
    leftScratch.resize(size);
    rightScratch.resize(channelCount == 2 ? size : 0);
    if (size > 0 && !read(readFrom, viewScratch, leftScratch, rightScratch)) {
        return false;
    }
    const bool stereo = channelCount == 2;
    if (weightedOnly) {
        // The K-weighting alone, for a block whose other statistics do not depend on the audio
        // before it: its energy from the state passed on to it.
        into.weighted = 0.0;
        const auto first = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(from) - readFrom);
        const auto last = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(to) - readFrom);
        for (std::size_t index = first; index < last; ++index) {
            const double weightedLeft = weighting.process(finiteOrZero(leftScratch[index]), left);
            into.weighted += weightedLeft * weightedLeft;
            if (stereo) {
                const double weightedRight =
                    weighting.process(finiteOrZero(rightScratch[index]), right);
                into.weighted += weightedRight * weightedRight;
            }
        }
        return true;
    }
    const auto at = [&](std::size_t position) {
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(position) - readFrom);
    };
    for (std::size_t position = blockStart; position < from; ++position) {
        (void)weighting.process(finiteOrZero(leftScratch[at(position)]), left);
        if (stereo) {
            (void)weighting.process(finiteOrZero(rightScratch[at(position)]), right);
        }
    }
    const bool interpolates = !interpolator.phases().empty();
    // Most blocks hold no join and no sample that is not finite: their points need no check.
    const bool checkJoins =
        crossesJoin(joins, from > TruePeak::kBefore ? from - TruePeak::kBefore - 1 : 0,
                    to + TruePeak::kAfter) ||
        (from <= TruePeak::kBefore && !joins.empty() && joins.front() == 0);
    const bool allFinite =
        std::ranges::all_of(viewScratch, [](float value) { return std::isfinite(value); });
    for (std::size_t position = from; position < to; ++position) {
        const std::size_t index = at(position);
        const float sample = viewScratch[index];
        const float leftSample = leftScratch[index];
        const float rightSample = stereo ? rightScratch[index] : leftSample;
        const bool viewFinite = std::isfinite(sample);
        const bool pairFinite = std::isfinite(leftSample) && std::isfinite(rightSample);
        if (viewFinite) {
            into.peak = std::max(into.peak, std::abs(sample));
            into.sumSquares += static_cast<double>(sample) * sample;
            into.sum += sample;
        } else {
            ++into.viewInvalid;
        }
        if (!viewFinite || !pairFinite) {
            ++into.invalid;
        }
        const double weightedLeft = weighting.process(finiteOrZero(leftSample), left);
        into.weighted += weightedLeft * weightedLeft;
        if (stereo) {
            const double weightedRight = weighting.process(finiteOrZero(rightSample), right);
            into.weighted += weightedRight * weightedRight;
        }
        if (pairFinite) {
            into.leftSquares += static_cast<double>(leftSample) * leftSample;
            into.rightSquares += static_cast<double>(rightSample) * rightSample;
            into.products += static_cast<double>(leftSample) * rightSample;
        }
        // The points between this sample and the next.
        if (interpolates && position >= TruePeak::kBefore &&
            std::cmp_greater_equal(position - TruePeak::kBefore, readFrom) &&
            std::cmp_less(position + TruePeak::kAfter, readTo) &&
            (!checkJoins ||
             !crossesJoin(joins, position - TruePeak::kBefore, position + TruePeak::kAfter))) {
            const std::span<const float, TruePeak::kTaps> around(
                viewScratch.data() + at(position - TruePeak::kBefore), TruePeak::kTaps);
            if (allFinite ||
                std::ranges::all_of(around, [](float value) { return std::isfinite(value); })) {
                const auto point = static_cast<float>(interpolator.between(around));
                float& peak = position + 1 == to ? into.truePeakAfter : into.truePeak;
                peak = std::max(peak, point);
            }
        }
    }
    return true;
}

bool MeterBlocks::update(std::size_t first, std::size_t end, std::span<const std::size_t> joins,
                         const Reader& read)
{
    end = std::min(end, spanCount);
    if (!valid || first >= end) {
        return valid;
    }
    std::size_t block = first / blockLength;
    // The previous block's last points read into the changed samples, and a join that moved or a
    // jump that landed there changes which of them count.
    if (block > 0 && first - (block * blockLength) < TruePeak::kAfter) {
        --block;
    }
    const std::size_t changedEnd = ((end - 1) / blockLength) + 1;
    const std::size_t stop = std::min(values.size(), changedEnd + kSettleBlocks);
    for (; block < stop; ++block) {
        Block& current = values[block];
        KWeighting::State left = current.leftState;
        KWeighting::State right = current.rightState;
        const std::size_t from = block * blockLength;
        const std::size_t to = std::min(from + blockLength, spanCount);
        // Past the changed blocks and the one after (whose first points read into them), only
        // the K-weighting depends on what changed: the state passed on.
        const bool weightedOnly = block > changedEnd;
        Block measured = weightedOnly ? current : Block{};
        measured.leftState = left;
        measured.rightState = right;
        if (!measureRange(from, from, to, joins, read, measured, left, right, weightedOnly)) {
            valid = false;
            return false;
        }
        current = measured;
        if (block + 1 >= values.size()) {
            break;
        }
        Block& next = values[block + 1];
        // Settled: past the changed blocks, the state passed on is the one the next block was
        // measured from, so it and those after read as they did.
        const bool settled = block + 1 > changedEnd && sameState(next.leftState, left) &&
                             sameState(next.rightState, right);
        next.leftState = left;
        next.rightState = right;
        if (settled) {
            break;
        }
    }
    return true;
}

bool MeterBlocks::weightedBefore(std::size_t position, const Reader& read, double& energy)
{
    energy = 0.0;
    const std::size_t block = position / blockLength;
    const std::size_t start = block * blockLength;
    if (position == start || block >= values.size()) {
        return true;
    }
    Block part;
    KWeighting::State left = values[block].leftState;
    KWeighting::State right = values[block].rightState;
    if (!measureRange(start, start, position, {}, read, part, left, right, true)) {
        return false;
    }
    energy = part.weighted;
    return true;
}

Readouts MeterBlocks::measure(std::size_t first, std::size_t end,
                              std::span<const std::size_t> joins, const Reader& read)
{
    Readouts readouts;
    end = std::min(end, spanCount);
    if (!valid || first >= end) {
        return readouts;
    }
    const std::size_t firstBlock = first / blockLength;
    const std::size_t lastBlock = (end - 1) / blockLength;
    // The parts measured, each a block's statistics over the positions of it that are visible.
    float peak = 0.0f;
    float truePeak = 0.0f;
    double sumSquares = 0.0;
    double sum = 0.0;
    std::size_t viewInvalid = 0;
    std::size_t invalid = 0;
    // Correlation counts the parts where both channels sound: their energies first.
    struct Part
    {
        double left;
        double right;
        double products;
        std::size_t length;
    };
    std::vector<Part> parts;
    parts.reserve(lastBlock - firstBlock + 1);
    for (std::size_t block = firstBlock; block <= lastBlock; ++block) {
        const std::size_t blockStart = block * blockLength;
        const std::size_t blockEnd = std::min(blockStart + blockLength, spanCount);
        const std::size_t from = std::max(first, blockStart);
        const std::size_t to = std::min(end, blockEnd);
        Block part;
        if (from == blockStart && to == blockEnd) {
            part = values[block];
        } else {
            KWeighting::State left = values[block].leftState;
            KWeighting::State right = values[block].rightState;
            if (!measureRange(blockStart, from, to, joins, read, part, left, right)) {
                return {};
            }
        }
        peak = std::max(peak, part.peak);
        truePeak = std::max(truePeak, part.truePeak);
        // The point after the part's last sample counts where that sample's successor is visible.
        if (to < end) {
            truePeak = std::max(truePeak, part.truePeakAfter);
        }
        sumSquares += part.sumSquares;
        sum += part.sum;
        viewInvalid += part.viewInvalid;
        invalid += part.invalid;
        parts.push_back({.left = part.leftSquares,
                         .right = part.rightSquares,
                         .products = part.products,
                         .length = to - from});
    }
    readouts.invalid = static_cast<std::uint32_t>(invalid);
    const std::size_t count = end - first;
    const std::size_t finite = count - viewInvalid;
    if (finite > 0) {
        readouts.peak = peak;
        readouts.rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(finite)));
        readouts.truePeak = std::max(peak, truePeak);
        if (static_cast<double>(count) >= std::round(protocol::kDcOffsetMinimumSeconds * rate)) {
            readouts.dcOffset = static_cast<float>(sum / static_cast<double>(finite));
        }
    }

    // Correlation over the parts where both channels sound: each above the analysis gate and not
    // more than 60 dB below that channel's loudest part.
    if (channelCount == 2) {
        constexpr double kAudible = 1e-6; // 60 dB, in power.
        const double gate = std::pow(10.0, protocol::kAnalysisGateDbfs / 10.0);
        double loudestLeft = 0.0;
        double loudestRight = 0.0;
        for (const Part& part : parts) {
            const auto length = static_cast<double>(part.length);
            loudestLeft = std::max(loudestLeft, part.left / length);
            loudestRight = std::max(loudestRight, part.right / length);
        }
        double left = 0.0;
        double right = 0.0;
        double products = 0.0;
        for (const Part& part : parts) {
            const auto length = static_cast<double>(part.length);
            const double leftPower = part.left / length;
            const double rightPower = part.right / length;
            if (leftPower > gate && rightPower > gate && leftPower > kAudible * loudestLeft &&
                rightPower > kAudible * loudestRight) {
                left += part.left;
                right += part.right;
                products += part.products;
            }
        }
        if (left > 0.0 && right > 0.0) {
            readouts.correlation =
                static_cast<float>(std::clamp(products / std::sqrt(left * right), -1.0, 1.0));
        }
    }

    // Loudness: BS.1770's gating blocks from the visible part's start, from the K-weighted energy
    // before each block edge (the blocks' sums, and the samples of a block an edge falls inside).
    const auto blockSize =
        static_cast<std::size_t>(std::llround(protocol::kLoudnessBlockSeconds * rate));
    if (blockSize > 0 && count >= blockSize) {
        weightedPrefix.assign(values.size() + 1, 0.0);
        for (std::size_t block = 0; block < values.size(); ++block) {
            weightedPrefix[block + 1] = weightedPrefix[block] + values[block].weighted;
        }
        const auto energyBefore = [&](std::size_t position, double& energy) {
            double part = 0.0;
            if (!weightedBefore(position, read, part)) {
                return false;
            }
            energy = weightedPrefix[std::min(position / blockLength, values.size())] + part;
            return true;
        };
        // Each edge's energy once: a block's end is usually a later block's start.
        std::vector<std::pair<std::size_t, double>> edges;
        const auto energyAt = [&](std::size_t position, double& energy) {
            const auto known =
                std::ranges::find(edges, position, &std::pair<std::size_t, double>::first);
            if (known != edges.end()) {
                energy = known->second;
                return true;
            }
            if (!energyBefore(position, energy)) {
                return false;
            }
            edges.emplace_back(position, energy);
            return true;
        };
        std::vector<double> blockPowers;
        for (std::size_t index = 0;; ++index) {
            const std::size_t start =
                first + static_cast<std::size_t>(std::llround(
                            static_cast<double>(index) * protocol::kLoudnessStepSeconds * rate));
            const std::size_t stop = start + blockSize;
            if (stop > end) {
                break;
            }
            double before = 0.0;
            double after = 0.0;
            if (!energyAt(start, before) || !energyAt(stop, after)) {
                return {};
            }
            blockPowers.push_back(std::max(after - before, 0.0) / static_cast<double>(blockSize));
        }
        const auto gated = [&](double threshold) {
            double total = 0.0;
            std::size_t counted = 0;
            for (const double power : blockPowers) {
                if (power > 0.0 && lufsOf(power) > threshold) {
                    total += power;
                    ++counted;
                }
            }
            return counted > 0 ? total / static_cast<double>(counted) : 0.0;
        };
        const double aboveAbsolute = gated(protocol::kLoudnessAbsoluteGateLufs);
        if (aboveAbsolute > 0.0) {
            const double relative = lufsOf(aboveAbsolute) + protocol::kLoudnessRelativeGateLu;
            const double aboveBoth = gated(std::max(relative, protocol::kLoudnessAbsoluteGateLufs));
            if (aboveBoth > 0.0) {
                readouts.loudness = static_cast<float>(lufsOf(aboveBoth));
            }
        }
    }
    return readouts;
}

} // namespace orizon::engine
