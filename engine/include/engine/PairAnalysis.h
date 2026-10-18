#pragma once

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
// Everything is measured only where both sound, in blocks of `block` samples as overlapGain
// decides, so a track that plays briefly in a long stretch is judged where it plays. The search
// runs coarse then fine over the whole stretch, and offsets whose match is within 2% of
// the best prefer the smaller shift, so that a polarity
// flip wins over a half-period shift of a steady tone. A best match whose correlation is below 0.3
// counts as unrelated.
[[nodiscard]] PairAnalysis analysePair(std::span<const float> a, std::span<const float> b,
                                       std::size_t maxOffset, std::size_t block);

// analysePair spread over calls, so that a long stretch's search does not stall one frame: each
// step does about `budget` multiply-adds of it (at least one offset's), and the search visits
// the same offsets in the same order as analysePair, so the result is the same.
class PairSearch
{
public:
    // Starts a search of `a` and `b` (equal length) as analysePair would, ending any search in
    // progress. It keeps its own copies, so they may change afterwards. Prepares them here.
    void start(std::span<const float> a, std::span<const float> b, std::size_t maxOffset,
               std::size_t block);
    // Searches on for about `budget` multiply-adds. True once the result is ready.
    bool step(std::size_t budget);
    // The result, once a step has returned true.
    [[nodiscard]] const PairAnalysis& result() const { return found; }
    // The multiply-adds of the offsets searched so far, over every search.
    [[nodiscard]] std::uint64_t work() const { return done; }

private:
    enum class Stage : std::uint8_t
    {
        kIdle,
        kCoarse,
        kFine,
        kDone,
    };
    // The offsets in [from, to], from the smallest shift out, and the strongest match so far.
    struct Scan
    {
        std::ptrdiff_t from = 0;
        std::ptrdiff_t to = 0;
        std::ptrdiff_t nearest = 0;
        std::ptrdiff_t step = 0; // 0: `nearest` itself is next.
        int side = 0;            // Of nearest - step, then nearest + step.
        std::ptrdiff_t bestOffset = 0;
        double bestValue = 0.0;
    };
    static void beginScan(Scan& scan, std::ptrdiff_t from, std::ptrdiff_t to);
    // Scans on in `a` and `b`, normalised by `scale`, for about `budget` multiply-adds. True
    // once every offset was visited.
    bool advance(std::span<const float> a, std::span<const float> b, double scale,
                 std::size_t& budget);
    void finish();

    Stage stage = Stage::kIdle;
    std::vector<float> maskedA;
    std::vector<float> maskedB;
    std::vector<float> coarseA;
    std::vector<float> coarseB;
    std::ptrdiff_t reach = 0;
    double norm = 0.0;
    double coarseNorm = 0.0;
    Scan scan;
    PairAnalysis found;
    std::uint64_t done = 0;
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
