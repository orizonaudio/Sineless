#include "engine/Fft.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <numbers>
#include <utility>

namespace orizon::engine {

Fft::Fft(std::size_t size) : points(size), reversed(size)
{
    assert(std::has_single_bit(size) && size >= 2 && size <= kLargestSize);
    cosines.reserve(size - 1);
    sines.reserve(size - 1);
    for (std::size_t length = 2; length <= size; length *= 2) {
        for (std::size_t j = 0; j < length / 2; ++j) {
            const double angle =
                2.0 * std::numbers::pi * static_cast<double>(j) / static_cast<double>(length);
            cosines.push_back(static_cast<float>(std::cos(angle)));
            sines.push_back(static_cast<float>(-std::sin(angle)));
        }
    }
    const int bits = std::countr_zero(size);
    for (std::size_t index = 0; index < size; ++index) {
        std::uint32_t mirrored = 0;
        for (int bit = 0; bit < bits; ++bit) {
            mirrored |= static_cast<std::uint32_t>((index >> static_cast<unsigned>(bit)) & 1U)
                        << static_cast<unsigned>(bits - 1 - bit);
        }
        reversed[index] = mirrored;
    }
}

// Radix 2, decimation in time: the values in bit-reversed order, then one pass per stage, each
// combining pairs of half-length transforms. `direction` is 1 forward and -1 inverse, which
// conjugates the twiddle factors.
void Fft::transform(std::span<float> real, std::span<float> imaginary, float direction) const
{
    assert(real.size() == points && imaginary.size() == points);
    float* const re = real.data();
    float* const im = imaginary.data();
    for (std::size_t index = 0; index < points; ++index) {
        const std::size_t other = reversed[index];
        if (index < other) {
            std::swap(re[index], re[other]);
            std::swap(im[index], im[other]);
        }
    }
    std::size_t table = 0;
    for (std::size_t length = 2; length <= points; length *= 2) {
        const std::size_t half = length / 2;
        const float* const cosine = cosines.data() + table;
        const float* const sine = sines.data() + table;
        for (std::size_t first = 0; first < points; first += length) {
            float* const lowRe = re + first;
            float* const lowIm = im + first;
            float* const highRe = lowRe + half;
            float* const highIm = lowIm + half;
            for (std::size_t j = 0; j < half; ++j) {
                const float c = cosine[j];
                const float s = direction * sine[j];
                const float turnedRe = highRe[j] * c - highIm[j] * s;
                const float turnedIm = highRe[j] * s + highIm[j] * c;
                highRe[j] = lowRe[j] - turnedRe;
                highIm[j] = lowIm[j] - turnedIm;
                lowRe[j] += turnedRe;
                lowIm[j] += turnedIm;
            }
        }
        table += half;
    }
}

} // namespace orizon::engine
