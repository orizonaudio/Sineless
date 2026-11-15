#pragma once

// Tests of the multi-track view register a few instances and look at one of them: its Multi shows
// only the tracks it is asked to (a new instance shows its own alone), so they ask for all.

#include "engine/Engine.h"
#include "engine/InstanceRegistry.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace orizon::engine {

// Has `own`'s Multi show every other instance of `registry`, as when the user ticks each.
inline void showEveryTrack(const InstanceRegistry& registry, Engine& own)
{
    std::vector<std::uint64_t> others;
    for (const auto& member : registry.members()) {
        if (member.engine.get() != &own) {
            others.push_back(member.serial);
        }
    }
    own.setShown(std::move(others));
}

} // namespace orizon::engine
