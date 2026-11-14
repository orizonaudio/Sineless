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
    // Instances on one track follow each other in creation order (the sort above), so each one's
    // number is one more than the instances of its track before it.
    for (std::size_t index = 0; index < found.size(); ++index) {
        const auto onItsTrack = [&](const GroupMember& other) {
            return sameTrack(found[index].track, other.track);
        };
        const auto before = static_cast<std::ptrdiff_t>(index);
        const auto earlier = std::count_if(found.begin(), found.begin() + before, onItsTrack);
        const bool alone =
            earlier == 0 && std::none_of(found.begin() + before + 1, found.end(), onItsTrack);
        found[index].instance = alone ? 0 : static_cast<std::uint32_t>(earlier + 1);
    }
    return found;
}

bool sameTrack(const TrackInfo& first, const TrackInfo& second)
{
    if (first.index.has_value() || second.index.has_value()) {
        return first.index == second.index;
    }
    return !first.name.empty() && first.name == second.name;
}

} // namespace orizon::engine
