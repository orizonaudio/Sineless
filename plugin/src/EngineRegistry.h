#pragma once

#include "engine/Engine.h"

#include <cstdint>
#include <memory>

namespace orizon::plugin {

// Lets the controller find its processor's engine by token, so that no raw pointer travels
// through host messages. Tokens are random 64-bit values: a controller in a different
// process, whose registry does not hold the token, finds nothing instead of a wrong engine.
class EngineRegistry
{
public:
    [[nodiscard]] static std::int64_t add(std::shared_ptr<engine::Engine> engine);
    static void remove(std::int64_t token);
    [[nodiscard]] static std::shared_ptr<engine::Engine> find(std::int64_t token);
};

// Message the processor sends to the controller on connect, with the token as attribute.
constexpr const char* kEngineTokenMessage = "OrizonEngineToken";
constexpr const char* kEngineTokenAttribute = "token";

} // namespace orizon::plugin
