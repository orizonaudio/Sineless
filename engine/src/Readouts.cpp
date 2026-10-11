#include "engine/Readouts.h"

#include "engine/BlockStats.h"
#include "engine/Trigger.h"
#include "protocol/Protocol.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace orizon::engine {

namespace {

// Most cycles must be close to the median one for the signal to count as repeating.
constexpr double kSteadyShare = 0.6;

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
    double squares = 0.0;
    float minimum = samples.front();
    float maximum = samples.front();
    for (const float sample : samples) {
        sum += sample;
        squares += static_cast<double>(sample) * sample;
        minimum = std::min(minimum, sample);
        maximum = std::max(maximum, sample);
    }
    const double range = static_cast<double>(maximum) - minimum;
    // At or below the analysis gate there is no pitch to find.
    const double rms = std::sqrt(squares / static_cast<double>(samples.size()));
    if (!(rms > std::pow(10.0, protocol::kAnalysisGateDbfs / 20.0)) || range <= 0.0) {
        return std::nullopt;
    }
    const TriggerParameters crossing{.level = sum / static_cast<double>(samples.size()),
                                     .hysteresis = protocol::kCrossingHysteresis * range};
    std::vector<double> periods;
    double lastEdge = -1.0;
    forEachEdge(samples, 0, crossing, [&](double edge) {
        if (lastEdge >= 0.0) {
            periods.push_back(edge - lastEdge);
        }
        lastEdge = edge;
    });
    if (periods.empty()) {
        return std::nullopt;
    }
    // Steady repetition: most periods within kPeriodTolerance of the median, which leaves out
    // the gaps between notes or hits. The frequency is from those periods alone.
    std::vector<double> sorted = periods;
    const auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2);
    std::nth_element(sorted.begin(), middle, sorted.end());
    const double median = *middle;
    double steadyTotal = 0.0;
    std::size_t steady = 0;
    for (const double period : periods) {
        if (std::abs(period - median) <= protocol::kPeriodTolerance * median) {
            steadyTotal += period;
            ++steady;
        }
    }
    if (static_cast<double>(steady) < kSteadyShare * static_cast<double>(periods.size())) {
        return std::nullopt; // Noise, or several pitches at once.
    }
    return sampleRate * static_cast<double>(steady) / steadyTotal;
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
