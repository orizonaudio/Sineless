#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace orizon::engine {

// A complex Fourier transform of a power-of-two size, in place and in float, for the Compare
// view's cross-correlation (PairAnalysis). The real and the imaginary parts are kept in two
// arrays, so that each pass runs as vector lanes. Neither direction scales: forward then inverse
// gives the signal times size(), which a correlation divides out with its own norm.
//
// Its tables are computed once for a size (the twiddle factors from double sines, as a float
// recurrence would lose accuracy with the size); a transform allocates nothing. Used on the
// analysis thread only.
class Fft
{
public:
    // The largest size: 4 million points, beyond what a search over the longest window at the
    // highest sample rate asks for.
    static constexpr std::size_t kLargestSize = std::size_t{1} << 22U;

    // `size` is a power of two, from 2 to kLargestSize.
    explicit Fft(std::size_t size);

    [[nodiscard]] std::size_t size() const { return points; }

    // Transforms the `size()` values real[i] + j·imaginary[i]: forward with e^(-j2πik/N).
    void forward(std::span<float> real, std::span<float> imaginary) const
    {
        transform(real, imaginary, 1.0f);
    }
    void inverse(std::span<float> real, std::span<float> imaginary) const
    {
        transform(real, imaginary, -1.0f);
    }

private:
    void transform(std::span<float> real, std::span<float> imaginary, float direction) const;

    std::size_t points;
    // cos and -sin of 2πj/length for j below length/2, one stage's after another: lengths 2, 4,
    // …, size, each read from first to last by its pass.
    std::vector<float> cosines;
    std::vector<float> sines;
    // Each index with its bits reversed, which orders the values for the passes.
    std::vector<std::uint32_t> reversed;
};

} // namespace orizon::engine
