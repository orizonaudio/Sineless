#pragma once

#include "engine/Fft.h"
#include "engine/Reconstruction.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace orizon::engine {

// How two tracks sum, from the same stretch of each.
struct PairAnalysis
{
    // False when the two never sound together above the analysis gate: nothing was measured, and
    // the rest holds its defaults.
    bool measured = false;
    double correlation = 0.0; // -1 to +1, at no offset.
    // Whether `b` matches `a` clearly at some shift. Unrelated tracks (a kick and a hi-hat) still
    // have a strongest match, but a weak one; it suggests no fix.
    bool related = false;
    // The shift, in samples, that best lines `b` up with `a`: positive when `b` lags. With
    // `inverted`, `b` lines up best with its polarity flipped. 0 and false when not related.
    std::ptrdiff_t offset = 0;
    bool inverted = false;
    // From a search made to align the tracks (alignPair), when related: the shift to a fraction of
    // a sample, whether or not it is clearly better than none (twenty samples on a 50 Hz note
    // match within 2% of none and still decide how deep the tracks null). 0 otherwise.
    double delay = 0.0;
};

// Analyses `a` and `b` (equal length), searching offsets up to `maxOffset` samples either way.
// The correlation at no offset is measured only where both sound, in blocks of `block` samples as
// overlapGain decides, so a track that plays briefly in a long stretch is judged where it plays.
// The offset is searched at the tracks' full rate, over every offset within the reach, each track
// gated on its own (its blocks above the analysis gate and within 60 dB of its loudest): offsets
// whose match is within 2% of the best prefer the smaller shift, so that a polarity flip wins over
// a half-period shift of a steady tone, and the offset is the top of the peak so chosen. The match
// there is then measured where both sound once shifted; one whose correlation is below 0.3 counts
// as unrelated, and one not clearly better than none suggests no shift.
[[nodiscard]] PairAnalysis analysePair(std::span<const float> a, std::span<const float> b,
                                       std::size_t maxOffset, std::size_t block);

// The same for aligning `b` with `a` before taking their difference, which a whole-sample fix
// does not need: the peak is picked on the correlation between its lags (four points a lag), so
// that a delay between two samples is found on bright material too; the delay is refined there;
// then the exact match is measured at the whole lag nearest it and at that lag's neighbours,
// moving to one that matches better until neither does (the search's float sums are a sample or
// more off where the correlation is nearly flat, as on bass); and the delay between samples is
// kept only if `b` read there through Align's filter matches better still. A plain latency thus
// comes out as its whole number of samples. No level is matched: a level that changed is part of
// what differs.
[[nodiscard]] PairAnalysis alignPair(std::span<const float> a, std::span<const float> b,
                                     std::size_t maxOffset, std::size_t block);

// analysePair spread over calls, so that a long stretch's search does not stall one frame: each
// step does about `budget` multiply-adds of it, at least one block of a pass over the stretch or
// one transform, and whatever the budget the result is the same. The correlation over the reach
// is taken with Fourier transforms (Fft), a block of the stretch at a time.
class PairSearch
{
public:
    // Starts a search of `a` and `b` (equal length) as analysePair would, ending any search in
    // progress. It keeps its own copies, so they may change afterwards. It allocates what the
    // search needs; the steps allocate nothing more but the small lists of blocks.
    // `align`: as alignPair, else as analysePair.
    void start(std::span<const float> a, std::span<const float> b, std::size_t maxOffset,
               std::size_t block, bool align = false);
    // Searches on for about `budget` multiply-adds. True once the result is ready.
    bool step(std::size_t budget);
    // The result, once a step has returned true.
    [[nodiscard]] const PairAnalysis& result() const { return found; }
    // The work done so far, in multiply-adds (a transform's counted as what it takes of them),
    // over every search.
    [[nodiscard]] std::uint64_t work() const { return done; }

private:
    enum class Stage : std::uint8_t
    {
        kIdle,
        kEnergies,  // Each track's energy per block: where each sounds, and both.
        kNone,      // The match at no offset.
        kCorrelate, // The cross-spectrum, a block of the stretch at a time.
        kLags,      // The correlation at every offset, and the peak it points to.
        kMatch,     // The match at that offset.
        kBetween,   // Aligning: the correlation between its lags, and the delay it points to.
        kSettle,    // ...the exact match at the whole lags about it.
        kFraction,  // ...and at the delay between samples.
        kDone,
    };
    // The correlation is read between its lags four points a lag to pick its peak (with a filter
    // kBetweenReach lags each side), then thirty-two a lag to place the peak's top (with one
    // kLagMargin each side, right to a few thousandths of a sample on bright material where a
    // short one errs by a few hundredths).
    static constexpr std::size_t kBetween = 4;
    static constexpr std::size_t kBetweenReach = 64;
    static constexpr std::size_t kRefine = 32;
    static constexpr std::size_t kLagMargin = 256;
    // A match being measured: the second track shifted by `offset`, its energy per block of the
    // first as shifted (`placed` once known), where both then sound, and the sums there.
    struct Measure
    {
        std::ptrdiff_t offset = 0;
        // A shift between samples: the filter that reads the wave that far after sample `offset`.
        std::vector<float> taps{};
        std::vector<double> shifted{};
        std::vector<bool> together{};
        bool placed = false;
        double energyA = 0.0;
        double energyB = 0.0;
        double dot = 0.0;
    };
    void spend(std::size_t& budget, std::size_t cost);
    [[nodiscard]] bool affords(std::size_t budget) const;
    // Each goes on for about `budget` multiply-adds; true once done.
    bool measureEnergies(std::size_t& budget);
    bool measure(std::size_t& budget);
    void beginMeasure(std::ptrdiff_t offset, double fraction = 0.0);
    void shiftedBlock(std::size_t first, std::size_t size, std::span<float> into);
    // The match measured: its correlation, 0 where the tracks do not meet.
    [[nodiscard]] double strength() const;
    bool interpolateLags(std::size_t& budget);
    [[nodiscard]] double estimateDelay();
    void settle();
    void finishAligned(double delay, double value, double energyB);
    [[nodiscard]] std::vector<bool> togetherOf(const std::vector<double>& shifted) const;
    void correlateBlock(std::size_t& budget);
    void finish(std::ptrdiff_t offset, double value);

    Stage stage = Stage::kIdle;
    std::vector<float> trackA;
    std::vector<float> trackB;
    std::size_t blockLength = 0;
    std::size_t cursor = 0; // The next block of a pass, or the next sample to correlate.
    std::vector<double> energiesA;
    std::vector<double> energiesB;
    std::vector<bool> audibleA; // The blocks of each track the search reads.
    std::vector<bool> audibleB;
    std::vector<float> keptA; // One block of each, as measured.
    std::vector<float> keptB;
    Measure measured;
    bool aligning = false;
    // The whole lag that matches best so far, with its sums, and which neighbours are known to
    // match less well.
    struct Settled
    {
        std::ptrdiff_t lag = 0;
        bool any = false;
        double strength = 0.0;
        double dot = 0.0;
        double energyB = 0.0;
        int direction = 0;
        bool triedBefore = false;
        bool triedAfter = false;
    };
    Settled settled;
    double estimateSign = 1.0;     // Of the correlation at the estimate: the polarity it found.
    double estimate = 0.0;         // The delay the correlation points to, before settling.
    std::ptrdiff_t computed = 0;   // The lags the correlation is taken for: the reach and a margin.
    std::vector<float> padded;     // The correlation, kLagMargin lags beyond the reach each side.
    std::vector<float> fine;       // ...and kBetween points a lag within it.
    std::vector<float> neighbours; // A block of the second track with a filter's reach around it.
    Reconstruction between{kBetweenReach};
    Reconstruction refine{kLagMargin};
    Reconstruction delayFilter;
    std::ptrdiff_t reach = 0;
    std::size_t stride = 0; // Samples of the first track a transform takes.
    Fft fft{2};             // The smallest there is, until start() makes the one a search takes.
    std::vector<float> real;
    std::vector<float> imaginary;
    std::vector<float> crossReal;
    std::vector<float> crossImaginary;
    PairAnalysis found;
    std::uint64_t done = 0;
    bool progressed = false; // This step has done some work already.
};

// The energy of `values`: the sum of their squares.
[[nodiscard]] double energyOf(std::span<const float> values);

// How loud a sum of tracks is against the same tracks unrelated, in dB: the sum's energy against
// `partsEnergy`, the tracks' energies added. -infinity when the sum cancels exactly (a measurement:
// the sum is silent), NaN when the tracks are silent: nothing to measure.
[[nodiscard]] double summingGain(double sumEnergy, double partsEnergy);

// The summing gain of `tracks` (equal length) where at least two of them sound together, so that
// stretches where one plays alone do not water it down: over blocks of `block` samples in which
// the second-loudest track is within 20 dB of the loudest, leaving out blocks more than 60 dB
// below the loudest block or at or below the analysis gate. 0 for fewer than two tracks (the sum
// is the track); NaN when they never sound together: nothing was measured.
[[nodiscard]] double overlapGain(std::span<const std::span<const float>> tracks, std::size_t block);

// The same gain from energies kept per block of `block` samples: `tracks` holds each track's
// energy (sum of squares) per block, and `sums` the energy of the tracks' sum per block, over the
// same blocks.
[[nodiscard]] double overlapGainOfBlocks(std::span<const std::span<const double>> tracks,
                                         std::span<const double> sums, std::size_t block);

// How deep a difference nulls, in dB: the level of `sums`, the tracks' sum per block (a difference
// when the other tracks are negated in it), against that of track `reference`, over the blocks
// where the reference and at least one other track sound together (the rule overlapGain judges
// by, between the reference and the loudest other). -infinity when the sum is silent there (it
// cancels exactly); NaN for fewer than two tracks and when they never sound together: nothing to
// measure.
[[nodiscard]] double nullDepthOfBlocks(std::span<const std::span<const double>> tracks,
                                       std::span<const double> sums, std::size_t block,
                                       std::size_t reference);

// overlapGain measured in steps, a pass over one track at a time, so that the gain of many long
// tracks can be spread over frames. The tracks must stay as they are until it ends.
class GainMeasure
{
public:
    // Starts measuring `tracks` as overlapGain would, ending any measurement in progress.
    void start(std::span<const std::span<const float>> tracks, std::size_t block);
    // Measures on over about `budget` samples, at least one pass. True once the gain is ready.
    bool step(std::size_t budget);
    // The gain, once a step has returned true.
    [[nodiscard]] double result() const { return gain; }
    // The null depth of the tracks' sum against track `reference` (nullDepthOfBlocks), once a
    // step has returned true: the tracks given being this track and the others negated, their sum
    // is the difference.
    [[nodiscard]] double depth(std::size_t reference) const;
    // The samples passed over so far, over every measurement.
    [[nodiscard]] std::uint64_t work() const { return done; }

private:
    enum class Stage : std::uint8_t
    {
        kFused,    // Each track's energy per block and its part of the sum, then the sum's.
        kTotals,   // With a track not finite: each added into the sum, noting where not.
        kEnergies, // Then each track's energy per block, then the sum's, where all are finite.
        kDone,
    };
    std::vector<std::span<const float>> tracks;
    std::size_t block = 0;
    std::size_t length = 0;
    std::vector<double> totals;
    std::vector<std::uint8_t> broken;
    std::vector<std::vector<double>> energies;
    std::vector<double> sums;
    Stage stage = Stage::kDone;
    std::size_t next = 0; // The track the stage passes over next; the sum after the last.
    double gain = 0.0;
    std::uint64_t done = 0;
};

} // namespace orizon::engine
