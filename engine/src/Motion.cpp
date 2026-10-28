#include "engine/Motion.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace orizon::engine {

Motion measureMotion(std::span<const EdgeDraw> draws, double sampleRate)
{
    Motion motion;
    std::vector<double> errors;
    errors.reserve(draws.size());
    for (std::size_t index = 0; index < draws.size(); ++index) {
        const EdgeDraw& draw = draws[index];
        motion.lagMax = std::max(motion.lagMax, (draw.newest - draw.edge) / sampleRate * 1000.0);
        motion.ahead = motion.ahead || draw.edge > draw.newest;
        if (index == 0) {
            continue;
        }
        const EdgeDraw& previous = draws[index - 1];
        motion.backwards = motion.backwards || draw.edge < previous.edge;
        const double moved = (draw.edge - previous.edge) / sampleRate;
        errors.push_back(std::abs(moved - (draw.seconds - previous.seconds)) * 1000.0);
    }
    if (!errors.empty()) {
        std::ranges::sort(errors);
        motion.advanceErrorMax = errors.back();
        motion.advanceErrorP99 = errors[std::min(errors.size() - 1, errors.size() * 99 / 100)];
    }
    return motion;
}

} // namespace orizon::engine
