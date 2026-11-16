#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace orizon::engine {

// What the Meter shows, over the visible part (DESIGN.md FR-7). Of the readout channel (in Single
// the readout source when drawn, else the first view drawn; this track's lane channel in Multi):
// the sample peak, the RMS, the true peak (ITU-R BS.1770 Annex 2) and the DC offset. Of the
// track's Left and Right (its one channel when mono): the gated loudness (BS.1770, in LUFS) and
// their correlation at zero offset. NaN for a value with nothing to measure, and for the true peak,
// DC offset, loudness and correlation while the Meter is off, when the engine measures the levels
// alone (from BlockStats, as the summing gains read them).
struct Readouts
{
    float peak = std::numeric_limits<float>::quiet_NaN();
    float rms = std::numeric_limits<float>::quiet_NaN();
    float truePeak = std::numeric_limits<float>::quiet_NaN();
    float dcOffset = std::numeric_limits<float>::quiet_NaN();
    float loudness = std::numeric_limits<float>::quiet_NaN();
    float correlation = std::numeric_limits<float>::quiet_NaN();
    // Positions measured around: where a channel the Meter reads is not finite (NaN or infinite);
    // the readout channel alone while the Meter is off.
    std::uint32_t invalid = 0;
};

// BS.1770's K-weighting for a sample rate: a high shelf, then a high-pass, each a biquad designed
// from the analog prototype through the bilinear transform (the standard tabulates 48 kHz only and
// asks other rates for the same response; libebur128, FFmpeg and Audacity use these constants).
class KWeighting
{
public:
    struct Biquad
    {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };
    // One channel's state: the two biquads', transposed direct form II.
    using State = std::array<double, 4>;

    explicit KWeighting(double sampleRate);

    [[nodiscard]] const Biquad& shelf() const { return shelfStage; }
    [[nodiscard]] const Biquad& highPass() const { return highPassStage; }

    // The weighted sample for `sample`, advancing `state`.
    [[nodiscard]] double process(double sample, State& state) const;

private:
    Biquad shelfStage;
    Biquad highPassStage;
};

// BS.1770 Annex 2's interpolator: 48 taps in 4 phases of 12, each phase a point between two
// samples (at 1/8, 3/8, 5/8 and 7/8 of a sample, the prototype being symmetric about tap 23.5).
// Which phases a rate uses: all four below truePeakHalfRateHz, two half a sample apart below
// truePeakSampleRateHz, none from there (the samples alone).
class TruePeak
{
public:
    // Samples read either side of a point: the point after sample i reads samples i − kBefore to
    // i + kAfter.
    static constexpr std::size_t kBefore = 5;
    static constexpr std::size_t kAfter = 6;
    static constexpr std::size_t kTaps = 12;
    static const std::array<std::array<double, kTaps>, 4> kPhases;

    explicit TruePeak(double sampleRate);

    [[nodiscard]] std::span<const std::size_t> phases() const
    {
        return std::span(usedPhases).first(phaseCount);
    }
    // The largest absolute value of the points between sample i and i + 1, from `around`, the
    // samples i − kBefore to i + kAfter; 0 with no phase used.
    [[nodiscard]] double between(std::span<const float, kTaps> around) const;

private:
    std::array<std::size_t, 4> usedPhases{};
    std::size_t phaseCount = 0;
};

// The Meter's statistics of a span kept per block of samples, so that measuring any part of it
// costs a pass over the blocks inside that part and the samples of the two at its edges, and after
// a change only the blocks holding changed samples (and those whose values depend on them) are
// measured again. Positions are the span's, 0 to count; a reader may also give positions before 0
// and after the count (`readableFirst`, `readableEnd`): audio before the span settles the
// K-weighting filters, and the neighbours either side are read for the true peak.
class MeterBlocks
{
public:
    // Reads positions [first, first + view.size()): the readout channel into `view`, Left into
    // `left` and Right into `right` (each of that size or empty: `right` for a mono track, the
    // readout channel where the K-weighting alone is measured again).
    using Reader = std::function<bool(std::ptrdiff_t first, std::span<float> view,
                                      std::span<float> left, std::span<float> right)>;

    struct Block
    {
        // Of the readout channel's finite samples.
        float peak = 0.0f;
        double sumSquares = 0.0;
        double sum = 0.0;
        std::uint32_t viewInvalid = 0;
        // The true peak of the points between the block's samples, and of the points between its
        // last sample and the next (which counts only where that next sample is measured too);
        // points whose interpolator would read across a join, past what the reader has, or a
        // sample that is not finite are left out.
        float truePeak = 0.0f;
        float truePeakAfter = 0.0f;
        // Of Left and Right: the K-weighted sum of squares, both channels added; their sums of
        // squares and of products, over positions where both are finite.
        double weighted = 0.0;
        double leftSquares = 0.0;
        double rightSquares = 0.0;
        double products = 0.0;
        // Positions where the readout channel, Left or Right is not finite.
        std::uint32_t invalid = 0;
        // The K-weighting filters' state at the block's first sample, Left then Right.
        KWeighting::State leftState{};
        KWeighting::State rightState{};
    };

    // Blocks re-measured after a change, for the K-weighting state that the changed blocks pass on:
    // 0.1 s at 10 ms blocks, after which the filters have forgotten the earlier audio to about 1e-9
    // (the high-pass stage's time constant is 4.2 ms).
    static constexpr std::size_t kSettleBlocks = 10;

    // Starts over for a span of `count` samples in blocks of `blockSamples` (at least 1), at
    // `sampleRate`, of `channels` (1 or 2), which the reader can read over [readableFirst,
    // readableEnd) (around [0, count)). Every block reads as silence until measured; the filters
    // start at rest unless primed.
    void reset(std::size_t count, std::size_t blockSamples, double sampleRate, int channels,
               std::ptrdiff_t readableFirst, std::ptrdiff_t readableEnd);
    [[nodiscard]] bool fits(std::size_t count, std::size_t blockSamples, double sampleRate,
                            int channels) const
    {
        return valid && count == spanCount && blockSamples == blockLength && sampleRate == rate &&
               channels == channelCount;
    }
    // Forgets the statistics and frees their memory: nothing fits until the next reset. While the
    // Meter is off they are not kept, so they must not be read as current when it comes back on.
    void clear();
    // Whether statistics are kept, whatever their span.
    [[nodiscard]] bool measuring() const { return valid; }

    // Follows audio that goes on, as a capture's: drops the first `droppedBlocks` blocks, so that
    // every position moves down by that many blocks, and covers `count` positions from there on
    // (never fewer than it has left), measuring those added. The blocks kept stay as measured,
    // with the filters' state at their starts: they describe the audio, wherever the span starts.
    // The reader's positions are the new ones; [readableFirst, readableEnd) is what it can read
    // now, the positions added among them (a part measured later must lie inside it). False if
    // nothing is measured yet, every block was dropped, or `read` fails; the statistics are then
    // invalid until the next reset.
    bool follow(std::size_t droppedBlocks, std::size_t count, std::ptrdiff_t readableFirst,
                std::ptrdiff_t readableEnd, const Reader& read);

    // Settles the filters on up to `samples` of the audio before position 0, as far as readable.
    bool prime(std::size_t samples, const Reader& read);

    // Measures again the blocks holding positions [first, end), with the sweep's `joins` (sorted
    // positions where the audio is not continuous with the sample before). False if `read` fails;
    // the statistics are then invalid until the next reset.
    bool update(std::size_t first, std::size_t end, std::span<const std::size_t> joins,
                const Reader& read);

    // The readouts over positions [first, end) of the span, as measured: from the blocks inside it
    // and from the samples of the blocks at its edges. Readouts with nothing measured (NaN values)
    // if a read fails.
    [[nodiscard]] Readouts measure(std::size_t first, std::size_t end,
                                   std::span<const std::size_t> joins, const Reader& read);

    [[nodiscard]] std::size_t sampleCount() const { return spanCount; }
    [[nodiscard]] std::size_t blockSamples() const { return blockLength; }
    [[nodiscard]] std::span<const Block> blocks() const { return values; }

private:
    // Positions [from, to) of the block starting at `blockStart`, measured from the filter state at
    // the block's start (the samples before `from` only advance it). `after` says whether to
    // measure the point after the last position too. The state at `to` is left in `left`/`right`.
    // With `weightedOnly`, only `into`'s K-weighted energy is measured, the rest kept.
    bool measureRange(std::size_t blockStart, std::size_t from, std::size_t to,
                      std::span<const std::size_t> joins, const Reader& read, Block& into,
                      KWeighting::State& left, KWeighting::State& right, bool weightedOnly = false);
    // The K-weighted energy of positions [blockStart(position), position).
    bool weightedBefore(std::size_t position, const Reader& read, double& energy);

    std::size_t spanCount = 0;
    std::size_t blockLength = 1;
    double rate = 0.0;
    int channelCount = 1;
    std::ptrdiff_t readFirst = 0;
    std::ptrdiff_t readEnd = 0;
    bool valid = false;
    KWeighting weighting{48000.0};
    TruePeak interpolator{48000.0};
    std::vector<Block> values;
    std::vector<float> viewScratch;
    std::vector<float> leftScratch;
    std::vector<float> rightScratch;
    std::vector<double> weightedPrefix;
};

} // namespace orizon::engine
