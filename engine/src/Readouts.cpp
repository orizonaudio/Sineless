#include "engine/Readouts.h"

#include "engine/Trigger.h"

#include <algorithm>
#include <cmath>

namespace orizon::engine {

namespace {

constexpr double kHysteresisFraction = 0.1;
constexpr double kMinimumRange = 1e-6; // Below this the signal is treated as flat.

} // namespace

Readouts computeReadouts(std::span<const float> samples, double sampleRate)
{
    Readouts readouts;
    if (samples.empty()) {
        return readouts;
    }
    double sum = 0.0;
    double sumOfSquares = 0.0;
    float minimum = samples.front();
    float maximum = samples.front();
    for (float sample : samples) {
        sum += sample;
        sumOfSquares += static_cast<double>(sample) * sample;
        minimum = std::min(minimum, sample);
        maximum = std::max(maximum, sample);
    }
    const auto count = static_cast<double>(samples.size());
    readouts.peak = std::max(std::abs(minimum), std::abs(maximum));
    readouts.rms = static_cast<float>(std::sqrt(sumOfSquares / count));

    const double range = static_cast<double>(maximum) - minimum;
    if (range < kMinimumRange) {
        return readouts;
    }
    const TriggerParameters crossing{.level = sum / count,
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
    if (edgeCount >= 2) {
        readouts.frequencyHz = (edgeCount - 1) * sampleRate / (lastEdge - firstEdge);
    }
    return readouts;
}

} // namespace orizon::engine
