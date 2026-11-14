#include "engine/Fft.h"

#include <cmath>
#include <complex>
#include <gtest/gtest.h>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace orizon::engine {
namespace {

struct Signal
{
    std::vector<float> real;
    std::vector<float> imaginary;
};

Signal noise(std::size_t size, unsigned seed)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> uniform(-0.5f, 0.5f);
    Signal signal{.real = std::vector<float>(size), .imaginary = std::vector<float>(size)};
    for (std::size_t i = 0; i < size; ++i) {
        signal.real[i] = uniform(random);
        signal.imaginary[i] = uniform(random);
    }
    return signal;
}

// The transform by its definition, summed in double: X[k] = Σ x[i]·e^(∓j2πik/N).
std::vector<std::complex<double>> direct(const Signal& signal, double direction)
{
    const std::size_t size = signal.real.size();
    std::vector<std::complex<double>> out(size);
    for (std::size_t k = 0; k < size; ++k) {
        std::complex<double> total;
        for (std::size_t i = 0; i < size; ++i) {
            const double angle = -direction * 2.0 * std::numbers::pi *
                                 static_cast<double>((i * k) % size) / static_cast<double>(size);
            total += std::complex<double>(signal.real[i], signal.imaginary[i]) *
                     std::complex<double>(std::cos(angle), std::sin(angle));
        }
        out[k] = total;
    }
    return out;
}

// How far `signal` is from `expected`, as a level against `expected`'s, in dB.
double errorDb(const Signal& signal, const std::vector<std::complex<double>>& expected)
{
    double error = 0.0;
    double reference = 0.0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        error += std::norm(std::complex<double>(signal.real[i], signal.imaginary[i]) - expected[i]);
        reference += std::norm(expected[i]);
    }
    return 10.0 * std::log10(error / reference);
}

class FftSize : public ::testing::TestWithParam<std::size_t>
{
};

// A match search reads its offsets from these sums: each must be the sum its definition gives, at
// every size a search uses, to within what float holds (a correlation needs far less).
TEST_P(FftSize, MatchesTheTransformsDefinition)
{
    const std::size_t size = GetParam();
    const Fft fft(size);
    ASSERT_EQ(fft.size(), size);
    const Signal signal = noise(size, 7);

    Signal forward = signal;
    fft.forward(forward.real, forward.imaginary);
    EXPECT_LT(errorDb(forward, direct(signal, 1.0)), -110.0);

    Signal inverse = signal;
    fft.inverse(inverse.real, inverse.imaginary);
    EXPECT_LT(errorDb(inverse, direct(signal, -1.0)), -110.0);
}

INSTANTIATE_TEST_SUITE_P(PowersOfTwo, FftSize,
                         ::testing::Values(2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U, 512U, 1024U,
                                           2048U, 4096U),
                         [](const ::testing::TestParamInfo<std::size_t>& info) {
                             return std::to_string(info.param) + "Points";
                         });

// At the sizes of a search over a long window, forward then inverse gives the signal back, times
// its size: the rounding of twenty passes each way stays far below the signal.
TEST(Fft, ReturnsTheSignalAfterForwardAndInverse)
{
    for (const std::size_t size : {std::size_t{1} << 14U, std::size_t{1} << 20U}) {
        const Fft fft(size);
        const Signal signal = noise(size, 11);
        Signal there = signal;
        fft.forward(there.real, there.imaginary);
        fft.inverse(there.real, there.imaginary);
        std::vector<std::complex<double>> expected(size);
        for (std::size_t i = 0; i < size; ++i) {
            expected[i] = static_cast<double>(size) *
                          std::complex<double>(signal.real[i], signal.imaginary[i]);
        }
        EXPECT_LT(errorDb(there, expected), -110.0) << size << " points";
    }
}

} // namespace
} // namespace orizon::engine
