#pragma once

#include "engine/BlockClock.h"
#include "engine/Motion.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <string_view>
#include <vector>

namespace orizon::engine::testing {

// How a simulated host delivers its blocks.
enum class Arrivals : std::uint8_t
{
    kSteady,   // On time, within the jitter.
    kJittered, // Within a quarter of a period.
    kSplit,    // Each block split in two at a random point, both parts arriving together.
    kBurst,    // Two blocks together every other period.
};

inline std::string_view arrivalsName(Arrivals arrivals)
{
    switch (arrivals) {
    case Arrivals::kSteady:
        return "Steady";
    case Arrivals::kJittered:
        return "Jittered";
    case Arrivals::kSplit:
        return "Split";
    case Arrivals::kBurst:
        return "Burst";
    }
    return "";
}

struct HostPattern
{
    std::uint64_t block = 512;
    double rate = 48000.0;
    Arrivals arrivals = Arrivals::kSteady;
    double drawHertz = 60.0;
    double jitterSeconds = 0.001; // Steady, split and burst arrivals, and draws.
    double seconds = 20.0;
    std::uint32_t seed = 1;

    [[nodiscard]] double periodSeconds() const { return static_cast<double>(block) / rate; }
    // The longest time between two deliveries.
    [[nodiscard]] double largestGapSeconds() const
    {
        const double period = periodSeconds();
        switch (arrivals) {
        case Arrivals::kJittered:
            return 1.5 * period;
        case Arrivals::kBurst:
            return 2.0 * period + 2.0 * jitterSeconds;
        default:
            return period + 2.0 * jitterSeconds;
        }
    }
};

// Plays `pattern` against a clock starting at 1 s: the analysis thread looks at the newest stamp
// every 2 ms and at every draw, and `edgeAt` gives the edge a draw shows. Returns the draws.
inline std::vector<EdgeDraw>
simulateHost(const HostPattern& pattern,
             const std::function<void(const std::optional<BlockStampValue>&)>& observe,
             const std::function<double(std::uint64_t nanos, std::uint64_t newest)>& edgeAt)
{
    constexpr double kStart = 1.0;
    std::mt19937 random(pattern.seed);
    const auto uniform = [&](double half) {
        return std::uniform_real_distribution<double>(-half, half)(random);
    };
    // The deliveries: (time, index one past the block's audio).
    std::vector<std::pair<double, std::uint64_t>> deliveries;
    const double period = pattern.periodSeconds();
    const auto blocks = static_cast<std::uint64_t>(pattern.seconds / period);
    for (std::uint64_t block = 1; block <= blocks; ++block) {
        const double due = kStart + static_cast<double>(block) * period;
        double time = due + uniform(pattern.jitterSeconds);
        switch (pattern.arrivals) {
        case Arrivals::kJittered:
            time = due + uniform(period / 4.0);
            break;
        case Arrivals::kBurst:
            if (block % 2 == 1) {
                time = due + period + uniform(pattern.jitterSeconds);
            }
            break;
        case Arrivals::kSplit: {
            const auto cut =
                std::uniform_int_distribution<std::uint64_t>(1, pattern.block - 1)(random);
            deliveries.emplace_back(time, (block - 1) * pattern.block + cut);
            break;
        }
        case Arrivals::kSteady:
            break;
        }
        deliveries.emplace_back(time, block * pattern.block);
    }
    for (std::size_t index = 1; index < deliveries.size(); ++index) {
        deliveries[index].first = std::max(deliveries[index].first, deliveries[index - 1].first);
    }

    std::vector<EdgeDraw> draws;
    std::size_t delivered = 0;
    std::optional<BlockStampValue> stamp;
    const auto nanosOf = [](double seconds) { return static_cast<std::uint64_t>(seconds * 1e9); };
    const auto catchUp = [&](double time) {
        while (delivered < deliveries.size() && deliveries[delivered].first <= time) {
            stamp = BlockStampValue{.index = deliveries[delivered].second,
                                    .nanos = nanosOf(deliveries[delivered].first),
                                    .paced = true};
            ++delivered;
        }
    };
    double nextPoll = kStart;
    double nextDraw = kStart + 1.0 / pattern.drawHertz;
    const double end = kStart + pattern.seconds - 0.5;
    while (nextDraw < end) {
        const double drawTime = nextDraw + uniform(pattern.jitterSeconds);
        while (nextPoll < drawTime) {
            catchUp(nextPoll);
            observe(stamp);
            nextPoll += 0.002;
        }
        catchUp(drawTime);
        observe(stamp);
        const std::uint64_t newest = stamp ? stamp->index : 0;
        draws.push_back({.seconds = drawTime,
                         .edge = edgeAt(nanosOf(drawTime), newest),
                         .newest = static_cast<double>(newest)});
        nextDraw += 1.0 / pattern.drawHertz;
    }
    return draws;
}

} // namespace orizon::engine::testing
