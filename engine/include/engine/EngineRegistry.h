#pragma once

#include "engine/Engine.h"

#include <cstdint>
#include <memory>

namespace orizon::engine {

// Lets a plugin format's glue find an instance's engine by token, so that no raw pointer travels
// through host messages or properties (the VST3 controller finds its processor's engine this way,
// AD-10). Tokens are random 64-bit values: code in a different process, whose registry does not
// hold the token, finds nothing instead of a wrong engine.
class EngineRegistry
{
public:
    [[nodiscard]] static std::int64_t add(const std::shared_ptr<Engine>& engine);
    static void remove(std::int64_t token);
    [[nodiscard]] static std::shared_ptr<Engine> find(std::int64_t token);
};

} // namespace orizon::engine
