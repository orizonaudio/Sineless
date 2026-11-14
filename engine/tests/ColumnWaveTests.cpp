// Min/max columns drawn from the band-limited wave around their extreme samples (DESIGN.md, AD-32):
// a steady tone keeps its height in every column at every zoom, a column never reads below its
// samples nor above the wave, and a column that is updated in place or rolls in equals one reduced
// afresh. The tests' own model of the columns' filter (12 taps a phase, Kaiser β 6) gives the wave
// and the step's inherent error, never the code under test.
#include "engine/Reducer.h"
#include "support/SampleRates.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::FrameMode;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// --- The tests' model of the columns' filter -------------------------------------------------

constexpr int kModelReach = 6;
constexpr std::size_t kModelTaps = 2 * static_cast<std::size_t>(kModelReach);
constexpr double kModelBeta = 6.0;

double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 60; ++k) {
        term *= (x * x / 4.0) / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
    }
    return sum;
}

// The weights of the point `fraction` of a sample after sample n, for samples n - 5 … n + 6, with
// unity gain at DC.
std::array<double, kModelTaps> weightsAt(double fraction)
{
    std::array<double, kModelTaps> weights{};
    double sum = 0.0;
    for (int k = 0; k < 2 * kModelReach; ++k) {
        const double t = fraction - static_cast<double>(k - kModelReach + 1);
        const double ratio = t / kModelReach;
        const double sinc =
            t == 0.0 ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
        const double window =
            std::abs(ratio) < 1.0
                ? besselI0(kModelBeta * std::sqrt(1.0 - ratio * ratio)) / besselI0(kModelBeta)
                : 0.0;
        weights.at(static_cast<std::size_t>(k)) = sinc * window;
        sum += sinc * window;
    }
    for (double& weight : weights) {
        weight /= sum;
    }
    return weights;
}

// The model's wave at `fraction` after sample n of `x`, or NaN where its neighbours run out.
double waveAt(std::span<const double> x, std::ptrdiff_t n, double fraction)
{
    if (fraction == 0.0) {
        return n >= 0 && n < std::ssize(x) ? x[static_cast<std::size_t>(n)] : std::nan("");
    }
    if (n - kModelReach + 1 < 0 || n + kModelReach >= std::ssize(x)) {
        return std::nan("");
    }
    // The fractions used are quarters and sixteenths: their weights are computed once.
    static const std::vector<std::array<double, kModelTaps>> kSixteenths = [] {
        std::vector<std::array<double, kModelTaps>> table;
        table.reserve(16);
        for (int k = 0; k < 16; ++k) {
            table.push_back(weightsAt(k / 16.0));
        }
        return table;
    }();
    const double sixteenths = fraction * 16.0;
    const auto& weights = sixteenths == std::floor(sixteenths)
                              ? kSixteenths.at(static_cast<std::size_t>(sixteenths))
                              : weightsAt(fraction);
    double sum = 0.0;
    for (int k = 0; k < 2 * kModelReach; ++k) {
        sum += weights.at(static_cast<std::size_t>(k)) *
               x[static_cast<std::size_t>(n - kModelReach + 1 + k)];
    }
    return sum;
}

// The largest of the model's wave, 16 points a sample, over samples [from, to] (fractional ends).
double denseMax(std::span<const double> x, double from, double to)
{
    double largest = -std::numeric_limits<double>::infinity();
    for (auto n = static_cast<std::ptrdiff_t>(std::floor(from)); static_cast<double>(n) <= to;
         ++n) {
        for (int k = 0; k < 16; ++k) {
            const double t = static_cast<double>(n) + k / 16.0;
            if (t < from || t > to) {
                continue;
            }
            const double value = waveAt(x, n, k / 16.0);
            if (std::isfinite(value)) {
                largest = std::max(largest, value);
            }
        }
    }
    return largest;
}

// The step as the model takes it, for a column's highest sample `n` with every neighbour held: the
// wave at 4 points a sample from n - 1 to n + 1, and the vertex of the parabola at each local
// maximum. Its error on a tone is the step's own, which no implementation can beat.
double modelPeak(std::span<const double> x, std::ptrdiff_t n)
{
    std::array<double, 11> points{}; // n - 1 - 1/4 … n + 1 + 1/4.
    for (int q = 0; q < 11; ++q) {
        const int quarter = q + 3; // Quarters after n - 2.
        points.at(static_cast<std::size_t>(q)) =
            waveAt(x, n - 2 + quarter / 4, (quarter % 4) / 4.0);
    }
    double peak = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 1; i <= 9; ++i) {
        const double a = points.at(i - 1);
        const double b = points.at(i);
        const double c = points.at(i + 1);
        peak = std::max(peak, b);
        if (b >= a && b >= c && a + c < 2.0 * b) {
            peak = std::max(peak, b + (c - a) * (c - a) / (8.0 * (2.0 * b - a - c)));
        }
    }
    return peak;
}

std::vector<double> sine(std::size_t count, double cycles, double phase, double amplitude)
{
    std::vector<double> x(count);
    for (std::size_t k = 0; k < count; ++k) {
        x[k] = amplitude *
               std::sin((2.0 * std::numbers::pi * cycles * static_cast<double>(k)) + phase);
    }
    return x;
}

double decibels(double ratio)
{
    return 20.0 * std::log10(ratio);
}

// The step's largest error, in dB of the amplitude, on a steady tone of `cycles` a sample, at
// every phase (each crest anywhere between two samples): computed from the model, with a margin
// for float. About 0.002 dB at 10 kHz at 44.1 kHz, 0.015 at a third of the rate, 0.025 at 0.34.
double toneTolerance(double cycles)
{
    double worst = 0.0;
    for (int step = 0; step < 256; ++step) {
        const double phase = 2.0 * std::numbers::pi * step / 256.0;
        const auto x = sine(64, cycles, phase, 1.0);
        for (std::ptrdiff_t n = 16; n < 48; ++n) {
            // A local maximum of the samples: the crest is within a sample of it.
            if (x[static_cast<std::size_t>(n)] >= x[static_cast<std::size_t>(n - 1)] &&
                x[static_cast<std::size_t>(n)] >= x[static_cast<std::size_t>(n + 1)]) {
                worst = std::max(worst, std::abs(decibels(modelPeak(x, n))));
            }
        }
    }
    return worst + 0.002;
}

// --- Helpers ---------------------------------------------------------------------------------

std::vector<float> toFloat(std::span<const double> x)
{
    std::vector<float> out(x.size());
    std::ranges::transform(x, out.begin(), [](double v) { return static_cast<float>(v); });
    return out;
}

std::vector<double> toDouble(std::span<const float> x)
{
    std::vector<double> out(x.size());
    std::ranges::transform(x, out.begin(), [](float v) { return static_cast<double>(v); });
    return out;
}

// Column c of `columns` over `shown` samples covers [begin(c), begin(c + 1)).
std::size_t columnBegin(std::size_t column, std::size_t shown, std::size_t columns)
{
    return static_cast<std::size_t>(std::uint64_t{column} * shown / columns);
}

// --- Steady tones ----------------------------------------------------------------------------

struct ToneCase
{
    double rate = 44100.0;
    double frequency = 10000.0; // Hz, or a share of the rate below 1.
};

double cyclesPerSample(const ToneCase& c)
{
    return c.frequency < 1.0 ? c.frequency : c.frequency / c.rate;
}

class SteadyTone : public ::testing::TestWithParam<ToneCase>
{
};

// A steady tone up to 0.35 of the rate is drawn at its amplitude in every column holding a crest,
// within the step's own error, at every number of samples a column (1.5 to 4096), at every
// starting phase (EBU Tech 3341's, and the one putting crests midway between samples), at half
// and twice full scale; no column reads below its samples or above the wave; and the tone negated
// gives exactly the negated columns. Today's columns, of samples alone, read up to 2.4 dB low at
// 10 kHz at 44.1 kHz and 3 dB low at a quarter of the rate whatever the zoom.
TEST_P(SteadyTone, KeepsItsHeightInEveryColumn)
{
    const ToneCase c = GetParam();
    const double cycles = cyclesPerSample(c);
    const double tolerance = toneTolerance(cycles);
    // The wave at 16 points a sample reads a crest up to cos(π f / 16) low; the step may read it
    // as high as its own error above.
    const double denseSlack =
        std::pow(10.0, (tolerance - decibels(std::cos(std::numbers::pi * cycles / 16.0))) / 20.0) -
        1.0;
    constexpr std::size_t kColumns = 40;
    for (const double perColumn : {1.5, 2.0, 3.0, 5.0, 8.0, 16.0, 32.0, 64.0, 256.0, 4096.0}) {
        const auto shown = static_cast<std::size_t>(perColumn * kColumns);
        for (const double phaseDegrees : {0.0, 22.5, 45.0, 60.0, 67.5, 90.0 - (180.0 * cycles)}) {
            for (const double amplitude : {0.5, 2.0}) {
                SCOPED_TRACE(std::to_string(perColumn) + " samples a column, phase " +
                             std::to_string(phaseDegrees) + ", amplitude " +
                             std::to_string(amplitude));
                const double phase = phaseDegrees * std::numbers::pi / 180.0;
                const auto x = sine(shown + 2 * kColumnReach, cycles, phase, amplitude);
                const auto samples = toFloat(x);
                const auto held = toDouble(samples);
                std::vector<float> columns;
                ASSERT_EQ(reduceToColumns(samples, kColumnReach, kColumnReach, kColumns, columns),
                          FrameMode::kMinMax);
                for (std::size_t column = 0; column < kColumns; ++column) {
                    const std::size_t b = kColumnReach + columnBegin(column, shown, kColumns);
                    const std::size_t e = kColumnReach + columnBegin(column + 1, shown, kColumns);
                    const double top = columns[column * 2 + 1];
                    const double bottom = columns[column * 2];
                    const double sampleTop =
                        std::ranges::max(std::span<const double>(held).subspan(b, e - b));
                    const double sampleBottom =
                        std::ranges::min(std::span<const double>(held).subspan(b, e - b));
                    ASSERT_GE(top, sampleTop) << "column " << column << ": below its samples";
                    ASSERT_LE(bottom, sampleBottom) << "column " << column;
                    const double from = static_cast<double>(b) - 1.0 - 0.125;
                    const double to = static_cast<double>(e) + 0.125;
                    if (perColumn <= 64.0) { // Wider, the wave's peak is the samples' within it.
                        ASSERT_LE(top, denseMax(held, from, to) + (amplitude * denseSlack))
                            << "column " << column << ": above the wave";
                    }
                    // A crest (or trough) in [b, e): the tone's sine reaches ±1 at
                    // k = (π/2 + 2πm - phase) / (2π cycles).
                    const auto holds = [&](double quarter) {
                        const double start = (quarter * std::numbers::pi / 2.0 - phase) /
                                             (2.0 * std::numbers::pi * cycles);
                        const double period = 1.0 / cycles;
                        double t =
                            start + std::ceil((static_cast<double>(b) - start) / period) * period;
                        return t < static_cast<double>(e) - 1e-6;
                    };
                    if (holds(1.0)) {
                        EXPECT_NEAR(decibels(top / amplitude), 0.0, tolerance)
                            << "column " << column << " holds a crest";
                    }
                    if (holds(3.0)) {
                        EXPECT_NEAR(decibels(-bottom / amplitude), 0.0, tolerance)
                            << "column " << column << " holds a trough";
                    }
                }
                // Negated, exactly the negated columns.
                std::vector<float> negated(samples.size());
                std::ranges::transform(samples, negated.begin(), [](float v) { return -v; });
                std::vector<float> mirrored;
                reduceToColumns(negated, kColumnReach, kColumnReach, kColumns, mirrored);
                for (std::size_t column = 0; column < kColumns; ++column) {
                    ASSERT_EQ(mirrored[column * 2], -columns[column * 2 + 1]) << column;
                    ASSERT_EQ(mirrored[column * 2 + 1], -columns[column * 2]) << column;
                }
            }
        }
    }
}

std::vector<ToneCase> toneCases()
{
    std::vector<ToneCase> cases;
    for (const double rate : kSampleRates) {
        for (const double frequency :
             {1000.0, 5000.0, 10000.0, 15000.0, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125}) {
            if (frequency >= 1.0 && frequency / rate > 0.35) {
                continue;
            }
            cases.push_back({.rate = rate, .frequency = frequency});
        }
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(Tones, SteadyTone, ::testing::ValuesIn(toneCases()),
                         [](const ::testing::TestParamInfo<ToneCase>& info) {
                             const ToneCase& c = info.param;
                             const std::string tone =
                                 c.frequency < 1.0
                                     ? "fs_over_" + std::to_string(std::lround(1.0 / c.frequency))
                                     : std::to_string(std::lround(c.frequency)) + "Hz";
                             return tone + "_at_" + std::to_string(std::lround(c.rate));
                         });

// Tones above 0.35 of the rate, where the short filter sags, still never read below their samples
// nor above the wave, and never lower than today's columns.
TEST(ColumnWave, ReadsHighTonesNoLowerThanTheirSamples)
{
    constexpr std::size_t kColumns = 40;
    for (const double cycles : {0.4, 0.45, 0.49}) {
        for (const double perColumn : {1.5, 2.0, 5.0, 64.0}) {
            const auto shown = static_cast<std::size_t>(perColumn * kColumns);
            const auto x = sine(shown + 2 * kColumnReach, cycles, 0.3, 1.0);
            const auto samples = toFloat(x);
            const auto held = toDouble(samples);
            std::vector<float> columns;
            ASSERT_EQ(reduceToColumns(samples, kColumnReach, kColumnReach, kColumns, columns),
                      FrameMode::kMinMax);
            for (std::size_t column = 0; column < kColumns; ++column) {
                const std::size_t b = kColumnReach + columnBegin(column, shown, kColumns);
                const std::size_t e = kColumnReach + columnBegin(column + 1, shown, kColumns);
                const double sampleTop =
                    std::ranges::max(std::span<const double>(held).subspan(b, e - b));
                EXPECT_GE(columns[column * 2 + 1], sampleTop) << cycles << " " << column;
                EXPECT_LE(columns[column * 2 + 1], denseMax(held, static_cast<double>(b) - 1.125,
                                                            static_cast<double>(e) + 0.125) +
                                                       0.02)
                    << cycles << " " << column;
            }
        }
    }
}

// --- The extreme samples' index --------------------------------------------------------------

// The first index of the highest and lowest finite sample, found in the same vectorised pass as
// the values: at lengths up to a few vector widths and beyond, with ties across lanes and on ±0,
// NaN in each lane position, and a column of infinities; a column of nothing but NaN has none.
TEST(ColumnWave, FindsTheFirstHighestAndLowestSample)
{
    std::mt19937 random(7); // NOLINT(bugprone-random-generator-seed): reproducible
    std::uniform_int_distribution<int> level(-3, 3);
    for (std::size_t length = 1; length <= 40; ++length) {
        for (int trial = 0; trial < 50; ++trial) {
            std::vector<float> samples(length);
            for (float& sample : samples) {
                const int drawn = level(random);
                sample = drawn == 3 ? kNaN : drawn == -3 ? -0.0f : static_cast<float>(drawn);
            }
            std::size_t highest = std::numeric_limits<std::size_t>::max();
            std::size_t lowest = highest;
            for (std::size_t i = 0; i < length; ++i) {
                if (std::isnan(samples[i])) {
                    continue;
                }
                if (highest == std::numeric_limits<std::size_t>::max() ||
                    samples[i] > samples[highest]) {
                    highest = i;
                }
                if (lowest == std::numeric_limits<std::size_t>::max() ||
                    samples[i] < samples[lowest]) {
                    lowest = i;
                }
            }
            const Extremes found = extremesOf(samples);
            if (highest == std::numeric_limits<std::size_t>::max()) {
                EXPECT_TRUE(std::isnan(found.maximum) && std::isnan(found.minimum));
                EXPECT_EQ(found.highest, Extremes::kNone);
                EXPECT_EQ(found.lowest, Extremes::kNone);
                continue;
            }
            EXPECT_EQ(found.highest, highest) << length << " " << trial;
            EXPECT_EQ(found.lowest, lowest) << length << " " << trial;
            EXPECT_EQ(found.maximum, samples[highest]);
            EXPECT_EQ(found.minimum, samples[lowest]);
        }
    }
    const std::vector<float> infinite(19, std::numeric_limits<float>::infinity());
    const Extremes found = extremesOf(infinite);
    EXPECT_EQ(found.maximum, std::numeric_limits<float>::infinity());
    EXPECT_EQ(found.highest, 0U);
    EXPECT_EQ(found.lowest, 0U);
}

// --- Straight gaps, edges, steps and noise ---------------------------------------------------

// A straight column's extremes: its own samples', widened by a sample either side of its extreme
// samples where that sample is a local extreme of the samples (a column shows the wave's peaks
// up to a gap outside its own samples; a slope or a flat run adds nothing).
std::pair<float, float> straightExtremes(std::span<const float> x, std::size_t b, std::size_t e)
{
    std::size_t high = b;
    while (high < e && std::isnan(x[high])) {
        ++high;
    }
    std::size_t low = high;
    for (std::size_t i = high; i < e; ++i) {
        high = x[i] > x[high] ? i : high;
        low = x[i] < x[low] ? i : low;
    }
    const auto at = [&](std::size_t i) { return i < x.size() ? x[i] : kNaN; };
    const auto passes = [](float value, float neighbour, bool above) {
        return std::isnan(neighbour) || (above ? value >= neighbour : value <= neighbour);
    };
    float top = x[high];
    float bottom = x[low];
    for (const std::size_t i : {high - 1, high + 1}) {
        if (std::isfinite(at(i)) && passes(at(i), at(i - 1), true) &&
            passes(at(i), at(i + 1), true)) {
            top = std::max(top, at(i));
        }
    }
    for (const std::size_t i : {low - 1, low + 1}) {
        if (std::isfinite(at(i)) && passes(at(i), at(i - 1), false) &&
            passes(at(i), at(i + 1), false)) {
            bottom = std::min(bottom, at(i));
        }
    }
    return {bottom, top};
}

// Where its neighbours run out, a column is drawn straight, as the raw view is: with no
// neighbours held around a short span, a column shows exactly its samples and the local extremes
// beside them; held neighbours swept from none to the column filter's reach and
// one more turn its gaps to the wave one by one, never below the samples nor above the wave, and
// at the tone's height once all are held. Negated, the same.
TEST(ColumnWave, DrawsStraightWhereNeighboursRunOut)
{
    constexpr double kCycles = 10000.0 / 44100.0;
    for (std::size_t neighbours = 0; neighbours <= kColumnReach + 1; ++neighbours) {
        for (int position = 0; position < 32; ++position) {
            // Three samples, a column each... of one shown sample: an extreme with `neighbours`
            // held each side, its crest swept across it.
            const double phase = 2.0 * std::numbers::pi * position / 32.0;
            constexpr std::size_t kShown = 6;
            const auto x = sine(kShown + 2 * neighbours, kCycles, phase, 1.0);
            const auto samples = toFloat(x);
            const auto held = toDouble(samples);
            std::vector<float> columns;
            ASSERT_EQ(reduceToColumns(samples, neighbours, neighbours, 3, columns),
                      FrameMode::kMinMax);
            for (std::size_t column = 0; column < 3; ++column) {
                const std::size_t b = neighbours + (column * 2);
                const std::size_t e = b + 2;
                const double sampleTop =
                    std::ranges::max(std::span<const double>(held).subspan(b, e - b));
                EXPECT_GE(columns[column * 2 + 1], sampleTop);
                EXPECT_LE(columns[column * 2 + 1], denseMax(held, static_cast<double>(b) - 1.125,
                                                            static_cast<double>(e) + 0.125) +
                                                       0.01);
                if (neighbours == 0 && column == 1) {
                    // Every gap around the middle column's extremes reads past the span: straight.
                    // (Its extremes are at most a sample from the span's ends.)
                    const auto [bottom, top] = straightExtremes(samples, b, e);
                    EXPECT_EQ(columns[column * 2 + 1], top);
                    EXPECT_EQ(columns[column * 2], bottom);
                }
            }
        }
    }
}

// Beside a sample that is not a number, or across a seam, the gaps are straight: a column there
// shows exactly its samples and the local extremes beside them, with no vertex.
TEST(ColumnWave, DrawsStraightBesideABrokenSampleAndAcrossASeam)
{
    const auto samples = toFloat(sine(400, 0.11, 0.4, 1.0));
    std::vector<float> broken = samples;
    for (std::size_t k = 0; k < broken.size(); k += 9) {
        broken[k] = kNaN; // Every column's extreme lies within 6 samples of a NaN.
    }
    std::vector<float> columns;
    ASSERT_EQ(reduceToColumns(broken, 0, 0, 100, columns), FrameMode::kMinMax);
    for (std::size_t column = 0; column < 100; ++column) {
        const auto [bottom, top] = straightExtremes(broken, column * 4, column * 4 + 4);
        EXPECT_EQ(columns[column * 2 + 1], top) << column;
        EXPECT_EQ(columns[column * 2], bottom) << column;
    }
    // Two joins within one column's reach, a few samples apart: no wave across either.
    for (std::size_t column = 2; column < 98; column += 7) {
        std::vector<float> one;
        std::vector<float> two;
        const std::size_t first = column * 4;
        ColumnReduction reduction;
        const auto read = [&](std::size_t from, std::span<float> into) {
            std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(from), into.size(),
                        into.begin());
            return true;
        };
        reduction.reset(samples.size(), 100);
        const std::array<std::size_t, 2> joins{first - 3, first + 3};
        ASSERT_TRUE(reduction.update(0, samples.size(), read, joins));
        // Each join alone leaves the other side's gaps to the wave; together, every gap of the
        // column that reads across either is straight: the column reads no more than the
        // straight extremes of its samples and their local-extreme neighbours.
        const auto [bottom, top] = straightExtremes(samples, first, first + 4);
        EXPECT_EQ(reduction.values()[column * 2 + 1], top) << column;
        EXPECT_EQ(reduction.values()[column * 2], bottom) << column;
    }
    // A seam inside a column: every gap around its extremes reads across it.
    for (std::size_t column = 2; column < 98; column += 7) {
        std::vector<float> seamed;
        reduceToColumns(samples, 0, 0, 100, seamed, column * 4 + 2);
        const auto [bottom, top] = straightExtremes(samples, column * 4, column * 4 + 4);
        EXPECT_EQ(seamed[column * 2 + 1], top) << column;
        EXPECT_EQ(seamed[column * 2], bottom) << column;
    }
}

// On a step the columns show the wave a converter outputs, above the samples (a square of peak 1
// reaches about 1.27); on noise, a dense mix, a square and a clipped sine, no column reads below
// its samples, nor above the wave by more than the step's vertex can on such content.
TEST(ColumnWave, ReadsStepsAndNoiseBetweenTheirSamplesAndTheWave)
{
    std::mt19937 random(3); // NOLINT(bugprone-random-generator-seed): reproducible
    std::normal_distribution<double> gaussian(0.0, 0.3);
    std::vector<std::pair<std::string, std::vector<double>>> signals;
    std::vector<double> noise(4000);
    std::ranges::generate(noise, [&] { return gaussian(random); });
    signals.emplace_back("noise", noise);
    std::vector<double> mix(4000);
    for (std::size_t k = 0; k < mix.size(); ++k) {
        const auto t = static_cast<double>(k);
        mix[k] = 0.3 * std::sin(0.013 * t) + 0.2 * std::sin(0.71 * t + 1.0) +
                 0.15 * std::sin(1.93 * t) + 0.1 * gaussian(random);
    }
    signals.emplace_back("mix", mix);
    std::vector<double> square(4000);
    for (std::size_t k = 0; k < square.size(); ++k) {
        square[k] = (k / 37) % 2 == 0 ? 1.0 : -1.0;
    }
    signals.emplace_back("square", square);
    std::vector<double> clipped(4000);
    for (std::size_t k = 0; k < clipped.size(); ++k) {
        clipped[k] = std::clamp(1.5 * std::sin(0.05 * static_cast<double>(k)), -1.0, 1.0);
    }
    signals.emplace_back("clipped", clipped);
    for (const auto& [name, x] : signals) {
        const auto samples = toFloat(x);
        const auto held = toDouble(samples);
        for (const std::size_t columns :
             {std::size_t{2600}, std::size_t{1300}, std::size_t{500}, std::size_t{100}}) {
            SCOPED_TRACE(name + ", " + std::to_string(columns) + " columns");
            const std::size_t shown = samples.size() - 2 * kColumnReach;
            std::vector<float> out;
            ASSERT_EQ(reduceToColumns(samples, kColumnReach, kColumnReach, columns, out),
                      FrameMode::kMinMax);
            for (std::size_t column = 0; column < columns; ++column) {
                const std::size_t b = kColumnReach + columnBegin(column, shown, columns);
                const std::size_t e = kColumnReach + columnBegin(column + 1, shown, columns);
                const double sampleTop =
                    std::ranges::max(std::span<const double>(held).subspan(b, e - b));
                const double sampleBottom =
                    std::ranges::min(std::span<const double>(held).subspan(b, e - b));
                ASSERT_GE(out[column * 2 + 1], sampleTop) << column;
                ASSERT_LE(out[column * 2], sampleBottom) << column;
                const double wave =
                    denseMax(held, static_cast<double>(b) - 1.125, static_cast<double>(e) + 0.125);
                // The vertex can rise above the wave's 16 points a sample by a little, a share of
                // the level around it (about 0.16 dB on noise, at the column's own peak).
                double local = 0.0;
                for (std::size_t k = b - 2; k < e + 2; ++k) {
                    local = std::max(local, std::abs(held[k]));
                }
                EXPECT_LE(out[column * 2 + 1], wave + (0.02 * local)) << column;
            }
            if (name == "square") {
                EXPECT_GT(*std::ranges::max_element(out), 1.1f) << "the wave's overshoot";
            }
        }
        // Negated, exactly the negated columns.
        std::vector<float> negated(samples.size());
        std::ranges::transform(samples, negated.begin(), [](float v) { return -v; });
        std::vector<float> out;
        std::vector<float> mirrored;
        reduceToColumns(samples, kColumnReach, kColumnReach, 700, out);
        reduceToColumns(negated, kColumnReach, kColumnReach, 700, mirrored);
        for (std::size_t column = 0; column < 700; ++column) {
            ASSERT_EQ(mirrored[column * 2], -out[column * 2 + 1]) << name << column;
            ASSERT_EQ(mirrored[column * 2 + 1], -out[column * 2]) << name << column;
        }
    }
}

// --- Incremental equals fresh ----------------------------------------------------------------

// Where two sets of columns first differ, as "column: got, expected", or empty where they match.
std::string firstDifference(std::span<const float> got, std::span<const float> expected)
{
    if (got.size() != expected.size()) {
        return "sizes " + std::to_string(got.size()) + ", " + std::to_string(expected.size());
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i] != expected[i] && !(std::isnan(got[i]) && std::isnan(expected[i]))) {
            return "value " + std::to_string(i) + ": " + std::to_string(got[i]) + ", " +
                   std::to_string(expected[i]);
        }
    }
    return {};
}

// A sweep written in blocks, the playhead's seam at each block's end and one block wrapping past
// the image's end, updated in place equals a fresh reduction of the same image and seam: the
// changed range widens by the columns' reach, so a column whose neighbours changed is reduced
// again. 10 kHz at 44.1 kHz and a third of the rate, at one and a half and two samples a column,
// the phase swept so a crest falls within a fraction of a sample of a column's boundary.
TEST(ColumnWave, UpdatesASweepInPlaceAsAFreshReductionDoes)
{
    for (const double cycles : {10000.0 / 44100.0, 1.0 / 3.0}) {
        for (const double perColumn : {1.5, 2.0, 7.0}) {
            for (int position = 0; position < 16; ++position) {
                constexpr std::size_t kColumns = 300;
                const auto count = static_cast<std::size_t>(perColumn * kColumns);
                std::vector<float> image(count, 0.0f);
                const auto read = [&](std::size_t from, std::span<float> into) {
                    std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(from), into.size(),
                                into.begin());
                    return true;
                };
                ColumnReduction live;
                live.reset(count, kColumns);
                ASSERT_TRUE(live.update(0, count, read));
                const double phase = 2.0 * std::numbers::pi * position / 256.0;
                std::size_t written = 0;
                std::size_t seam = kNoSeam;
                for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{256},
                                                std::size_t{4096}, std::size_t{513}}) {
                    for (std::size_t k = 0; k < block; ++k) {
                        const std::size_t at = (written + k) % count;
                        image[at] = static_cast<float>(std::sin(
                            (2.0 * std::numbers::pi * cycles * static_cast<double>(written + k)) +
                            phase));
                    }
                    const std::size_t from = written % count;
                    written += block;
                    seam = written % count;
                    if (block >= count) { // Round the whole image and more: all of it changed.
                        ASSERT_TRUE(live.update(0, count, read, seam));
                    } else if (from + block <= count) {
                        ASSERT_TRUE(live.update(from, from + block, read, seam));
                    } else {
                        ASSERT_TRUE(live.update(from, count, read, seam));
                        ASSERT_TRUE(live.update(0, (from + block) % count, read, seam));
                    }
                    ColumnReduction fresh;
                    fresh.reset(count, kColumns);
                    ASSERT_TRUE(fresh.update(0, count, read, seam));
                    ASSERT_EQ(firstDifference(live.values(), fresh.values()), "")
                        << cycles << " " << perColumn << " " << position << " after " << written;
                }
            }
        }
    }
}

// Rolling, the columns kept equal a fresh reduction of the same window; the newest columns, whose
// trailing neighbours have not arrived, are reduced again until they have, and then equal it too.
// From the source's first sample, where no lead is held, the first columns are final at once.
TEST(ColumnWave, RollsAsAFreshReductionDoes)
{
    std::vector<float> signal(30000);
    for (std::size_t k = 0; k < signal.size(); ++k) {
        signal[k] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * (10000.0 / 44100.0) * static_cast<double>(k)));
    }
    for (const std::uint64_t perColumn : {2U, 3U, 96U}) {
        constexpr std::size_t kColumns = 200;
        const ColumnGrid grid{.spanSamples = kColumns * perColumn, .columns = kColumns};
        std::uint64_t available = 0;
        std::size_t reads = 0;
        const auto read = [&](std::uint64_t first, std::span<float> into) {
            if (first + into.size() > available) {
                return false;
            }
            reads += into.size();
            std::copy_n(signal.begin() + static_cast<std::ptrdiff_t>(first), into.size(),
                        into.begin());
            return true;
        };
        RollColumns roll;
        for (std::uint64_t end = grid.spanSamples + 5; end + 200 < signal.size(); end += 131) {
            available = end;
            const std::uint64_t firstColumn = (end - grid.spanSamples) / perColumn;
            const auto values = roll.update(0, firstColumn, grid, read);
            RollColumns fresh;
            const auto expected = fresh.update(0, firstColumn, grid, read);
            ASSERT_EQ(firstDifference(values, expected), "")
                << perColumn << " samples a column, audio to " << end;
        }
        // From the source's start: the first column is read once, never again.
        RollColumns fromStart;
        available = 5000;
        reads = 0;
        fromStart.update(0, 0, grid, read);
        const std::size_t first = reads;
        reads = 0;
        fromStart.update(0, 0, grid, read);
        EXPECT_LT(reads, first) << "the columns from the start are kept";
    }
}

} // namespace
} // namespace orizon::engine
