#pragma once

#include "engine/Fft.h"

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
    void start(std::span<const float> a, std::span<const float> b, std::size_t maxOffset,
               std::size_t block);
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
        kDone,
    };
    // A match being measured: the second track shifted by `offset`, its energy per block of the
    // first as shifted (`placed` once known), where both then sound, and the sums there.
    struct Measure
    {
        std::ptrdiff_t offset = 0;
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
    void beginMeasure(std::ptrdiff_t offset);
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
