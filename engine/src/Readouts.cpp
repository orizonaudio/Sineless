#include "engine/Readouts.h"

#include "engine/BlockStats.h"
#include "engine/PreciseFloatingPoint.h"

#include <cmath>
#include <cstddef>
#include <span>

namespace orizon::engine {

Readouts computeReadouts(std::span<const float> samples)
{
    Readouts readouts;
    if (samples.empty()) {
        return readouts;
    }
    const BlockStats::Block levels = measureBlock(samples);
    readouts.invalid = levels.invalid;
    const std::size_t finite = samples.size() - levels.invalid;
    if (finite == 0) {
        return readouts;
    }
    readouts.peak = levels.peak;
    readouts.rms = static_cast<float>(std::sqrt(levels.sumSquares / static_cast<double>(finite)));
    return readouts;
}

} // namespace orizon::engine
