#include "engine/InstanceRegistry.h"

#include <algorithm>
#include <tuple>

namespace orizon::engine {

InstanceRegistry& InstanceRegistry::shared()
{
    static InstanceRegistry registry;
    return registry;
}

void InstanceRegistry::add(const std::shared_ptr<Engine>& engine)
{
    const std::scoped_lock lock(mutex);
    std::erase_if(entries, [](const Entry& entry) { return entry.engine.expired(); });
    entries.push_back({.engine = engine, .serial = nextSerial++});
}

void InstanceRegistry::remove(const Engine& engine)
{
    const std::scoped_lock lock(mutex);
    std::erase_if(entries, [&](const Entry& entry) {
        const auto held = entry.engine.lock();
        return !held || held.get() == &engine;
    });
}

std::vector<GroupMember> InstanceRegistry::members(protocol::Group group) const
{
    std::vector<GroupMember> found;
    {
        const std::scoped_lock lock(mutex);
        for (const Entry& entry : entries) {
            if (auto engine = entry.engine.lock()) {
                found.push_back({.engine = std::move(engine), .track = {}, .serial = entry.serial});
            }
        }
    }
    // Settings and track info have their own locks; they are read outside the registry's.
    std::erase_if(found, [&](const GroupMember& member) {
        return member.engine->settings().get().enumValue<protocol::Group>(
                   protocol::SettingId::kGroup) != group;
    });
    for (GroupMember& member : found) {
        member.track = member.engine->trackInfo();
    }
    std::ranges::sort(found, [](const GroupMember& left, const GroupMember& right) {
        const auto key = [](const GroupMember& member) {
            return std::tuple(!member.track.index.has_value(), member.track.index.value_or(0),
                              member.serial);
        };
        return key(left) < key(right);
    });
    return found;
}

} // namespace orizon::engine
