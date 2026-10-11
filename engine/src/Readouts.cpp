#include "engine/Readouts.h"

#include "engine/BlockStats.h"
#include "engine/Trigger.h"
#include "protocol/Protocol.h"

#include <algorithm>
#include <cmath>

namespace orizon::engine {

namespace {

constexpr double kHysteresisFraction = 0.1;
// A peak-to-peak range at the silence floor: quieter signals are treated as flat, with no pitch.
const double kMinimumRange = std::pow(10.0, protocol::kSilenceFloorDb / 20.0);

} // namespace

std::size_t frequencyWindow(std::size_t count, double sampleRate)
{
    const double window = std::max(kFrequencyWindowSeconds * sampleRate, 0.0);
    return std::min(count, static_cast<std::size_t>(std::llround(window)));
}

std::optional<double> estimateFrequency(std::span<const float> samples, double sampleRate)
{
    if (samples.empty()) {
        return std::nullopt;
    }
    double sum = 0.0;
    float minimum = samples.front();
    float maximum = samples.front();
    for (const float sample : samples) {
        sum += sample;
        minimum = std::min(minimum, sample);
        maximum = std::max(maximum, sample);
    }
    const double range = static_cast<double>(maximum) - minimum;
    if (range < kMinimumRange) {
        return std::nullopt;
    }
    const TriggerParameters crossing{.level = sum / static_cast<double>(samples.size()),
                                     .hysteresis = kHysteresisFraction * range};
    int edgeCount = 0;
    double firstEdge = 0.0;
    double lastEdge = 0.0;
    forEachEdge(samples, 0, crossing, [&](double edge) {
        if (edgeCount++ == 0) {
            firstEdge = edge;
        }
        lastEdge = edge;
    });
    if (edgeCount < 2) {
        return std::nullopt;
    }
    return (edgeCount - 1) * sampleRate / (lastEdge - firstEdge);
}

Readouts computeReadouts(std::span<const float> samples, double sampleRate)
{
    Readouts readouts;
    if (samples.empty()) {
        return readouts;
    }
    const BlockStats::Block levels = measureBlock(samples);
    readouts.peak = levels.peak;
    readouts.rms =
        static_cast<float>(std::sqrt(levels.sumSquares / static_cast<double>(samples.size())));
    readouts.frequencyHz =
        estimateFrequency(samples.last(frequencyWindow(samples.size(), sampleRate)), sampleRate);
    return readouts;
}

} // namespace orizon::engine
