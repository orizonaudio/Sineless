#pragma once

// Deterministic test signals: the same samples on every platform and compiler, for the signal
// corpus and the recorded sessions.

#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <utility>

namespace orizon::engine {

// A track's audio: the sample at index `n`, playing at `rate`.
using Signal = std::function<float(std::uint64_t n, std::uint32_t rate)>;

// The phase is from integers, so it is exact however long the signal plays and the sine library
// always sees a small argument.
inline Signal sine(std::uint32_t hertz, double amplitude)
{
    return [=](std::uint64_t n, std::uint32_t rate) {
        const auto cycle = static_cast<double>((n * hertz) % rate);
        return static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * cycle / static_cast<double>(rate)));
    };
}

inline Signal dc(double level)
{
    return [=](std::uint64_t, std::uint32_t) { return static_cast<float>(level); };
}

inline Signal silence()
{
    return dc(0.0);
}

// White noise from a hash of the sample index: the same on every platform, uniform in
// [-amplitude, amplitude).
inline Signal noise(std::uint64_t seed, double amplitude = 1.0)
{
    return [=](std::uint64_t n, std::uint32_t) {
        std::uint64_t x = n * 0x9E3779B97F4A7C15ULL + seed * 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 30U;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27U;
        const double unit = static_cast<double>(x >> 40U) / static_cast<double>(1U << 24U);
        return static_cast<float>(amplitude * (2.0 * unit - 1.0));
    };
}

inline Signal scaled(Signal signal, double gain)
{
    return [source = std::move(signal), gain](std::uint64_t n, std::uint32_t rate) {
        return static_cast<float>(gain * source(n, rate));
    };
}

inline Signal inverted(Signal signal)
{
    return scaled(std::move(signal), -1.0);
}

// `signal` arriving `samples` later (silent before it starts).
inline Signal delayed(Signal signal, std::uint64_t samples)
{
    return [source = std::move(signal), samples](std::uint64_t n, std::uint32_t rate) {
        return n < samples ? 0.0f : source(n - samples, rate);
    };
}

// `signal` with `value` (NaN, an infinity) in place of every `every`-th sample.
inline Signal spiked(Signal signal, float value, std::uint64_t every)
{
    return [source = std::move(signal), value, every](std::uint64_t n, std::uint32_t rate) {
        return n % every == every / 2 ? value : source(n, rate);
    };
}

// One instance's input: its left and right channels, or the left alone on a mono bus.
struct Track
{
    Signal left;
    Signal right;
    int channels = 2;
};

inline Track stereo(const Signal& left, const Signal& right)
{
    Track track;
    track.left = left;
    track.right = right;
    return track;
}

inline Track stereo(const Signal& both)
{
    return stereo(both, both);
}

inline Track mono(const Signal& signal)
{
    Track track;
    track.left = signal;
    track.right = signal;
    track.channels = 1;
    return track;
}

} // namespace orizon::engine
