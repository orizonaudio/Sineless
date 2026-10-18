#include "engine/EngineRegistry.h"

#include <gtest/gtest.h>

namespace orizon::engine {
namespace {

// A plugin format's glue finds an instance's engine by its token while the instance lives, and
// nothing once it is removed: an editor opened after its processor went away shows
// "unavailable" instead of reaching a dead engine.
TEST(EngineRegistry, ForgetsRemovedTokens)
{
    auto engine = std::make_shared<Engine>();
    const std::int64_t token = EngineRegistry::add(engine);
    EXPECT_EQ(EngineRegistry::find(token), engine);
    EngineRegistry::remove(token);
    EXPECT_EQ(EngineRegistry::find(token), nullptr);
}

} // namespace
} // namespace orizon::engine
