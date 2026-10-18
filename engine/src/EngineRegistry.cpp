#include "engine/EngineRegistry.h"

#include <mutex>
#include <random>
#include <unordered_map>

namespace orizon::engine {

namespace {

struct Registry
{
    std::mutex mutex;
    std::unordered_map<std::int64_t, std::weak_ptr<Engine>> engines;
    std::mt19937_64 random{std::random_device{}()};
};

Registry& registry()
{
    static Registry instance;
    return instance;
}

} // namespace

std::int64_t EngineRegistry::add(const std::shared_ptr<Engine>& engine)
{
    Registry& state = registry();
    const std::scoped_lock lock(state.mutex);
    std::int64_t token = 0;
    do {
        token = static_cast<std::int64_t>(state.random());
    } while (token == 0 || state.engines.contains(token));
    state.engines.emplace(token, engine);
    return token;
}

void EngineRegistry::remove(std::int64_t token)
{
    Registry& state = registry();
    const std::scoped_lock lock(state.mutex);
    state.engines.erase(token);
}

std::shared_ptr<Engine> EngineRegistry::find(std::int64_t token)
{
    Registry& state = registry();
    const std::scoped_lock lock(state.mutex);
    const auto found = state.engines.find(token);
    return found == state.engines.end() ? nullptr : found->second.lock();
}

} // namespace orizon::engine
