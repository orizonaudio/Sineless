#pragma once

// The material the benchmarks play: kicks, bass, hats, tones and noise, one kind per track in
// turn, the same on every run, so that two runs of one build do exactly the same work. Each
// track's signal repeats every four seconds: a whole number of cycles of every tone, and of beats
// at 60 and 120 beats a minute. It is made once and then read, so playing costs a benchmark
// almost nothing beside the work it measures.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace orizon::bench {

class Signals
{
public:
    Signals(std::size_t tracks, double rate, double tempo)
        : loop(static_cast<std::size_t>(std::llround(kLoopSeconds * rate))),
          lateStep(static_cast<std::uint64_t>(std::llround(rate / 1000.0))),
          jitterPeriod(static_cast<std::uint64_t>(std::llround(kJitterPeriodSeconds * rate)))
    {
        tables.resize(tracks);
        for (std::size_t track = 0; track < tracks; ++track) {
            tables[track].resize(loop);
            for (std::size_t sample = 0; sample < loop; ++sample) {
                tables[track][sample] = valueOf(track, sample, rate, tempo);
            }
        }
    }

    // Writes track `track`'s audio from host sample `first`: the left channel, and the right at
    // 0.8 of it. With `jitter`, every track but the first plays 0 to 3 ms late, changing every
    // 0.7 s, so that Compare's matches keep moving, as they do with real material.
    void fill(std::size_t track, std::uint64_t first, bool jitter, std::span<float> left,
              std::span<float> right) const
    {
        const std::vector<float>& table = tables.at(track);
        for (std::size_t i = 0; i < left.size(); ++i) {
            std::uint64_t sample = first + i;
            if (jitter && track > 0) {
                const std::uint64_t late = lateStep * ((sample / jitterPeriod + track) % 4);
                sample = sample > late ? sample - late : 0;
            }
            left[i] = table[static_cast<std::size_t>(sample % loop)];
            right[i] = 0.8f * left[i];
        }
    }

private:
    static constexpr double kLoopSeconds = 4.0;
    static constexpr double kJitterPeriodSeconds = 0.7;

    // Noise in -0.5 to 0.5, the same for the same track and sample.
    static double noise(std::size_t track, std::size_t sample)
    {
        std::uint64_t x = (static_cast<std::uint64_t>(sample) * 0x9E3779B97F4A7C15ULL) ^
                          ((static_cast<std::uint64_t>(track) + 1) * 0xBF58476D1CE4E5B9ULL);
        x ^= x >> 30U;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27U;
        return (static_cast<double>(x >> 40U) / static_cast<double>(1U << 24U)) - 0.5;
    }

    static float valueOf(std::size_t track, std::size_t sample, double rate, double tempo)
    {
        const double t = static_cast<double>(sample) / rate;
        const double beat = 60.0 / tempo;
        const double inBeat = std::fmod(t, beat);
        const double pi = std::numbers::pi;
        switch (track % 5) {
        case 0: // A kick on every beat.
            return static_cast<float>(
                0.9 * std::exp(-inBeat * 9.0) *
                std::sin(2.0 * pi * (50.0 + (100.0 * std::exp(-inBeat / 0.03))) * inBeat));
        case 1: // Bass.
            return static_cast<float>(0.45 * std::sin(2.0 * pi * 55.0 * t));
        case 2: { // Hats on every eighth.
            const double inEighth = std::fmod(t, beat / 2);
            return static_cast<float>(0.4 * std::exp(-inEighth * 40.0) * noise(track, sample));
        }
        case 3: // A tone and its third harmonic.
            return static_cast<float>((0.3 * std::sin(2.0 * pi * 220.0 * t)) +
                                      (0.1 * std::sin(2.0 * pi * 660.0 * t)));
        default: // A noise bed.
            return static_cast<float>(0.2 * noise(track, sample));
        }
    }

    std::size_t loop;
    std::uint64_t lateStep;     // 1 ms.
    std::uint64_t jitterPeriod; // 0.7 s.
    std::vector<std::vector<float>> tables;
};

} // namespace orizon::bench
