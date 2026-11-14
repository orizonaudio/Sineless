#pragma once

#include "engine/Engine.h"
#include "protocol/Protocol.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace orizon::engine {

// One instance of a group, as the multi-track view sees it.
struct GroupMember
{
    std::shared_ptr<Engine> engine; // Kept alive while the member is held.
    TrackInfo track;
    std::uint64_t serial = 0; // Creation order within the process, from 1.
    // Its number among the group's instances on its track (sameTrack), from 1 in creation order;
    // 0 for a track's only instance there, which needs none.
    std::uint32_t instance = 0;
};

// Whether two instances are on one host track, as far as the host tells: the same track position
// where it gives both one (a track renamed between two reports is still that track); the same
// name, not empty, where it gives neither a position (an Audio Unit host gives only the name, so
// two tracks named alike count as one there). Hosts do not say where in a track's chain an
// instance sits.
[[nodiscard]] bool sameTrack(const TrackInfo& first, const TrackInfo& second);

// The Sineless instances of this process, so that one instance's editor can show the others.
// It holds weak references: an instance that ends disappears even if it was not removed, and a
// member already handed out stays valid until its holder lets go. Each build of the plugin has
// its own registry, so instances of different versions never see each other.
//
// Non-audio threads only.
class InstanceRegistry
{
public:
    // The registry of this plugin module.
    static InstanceRegistry& shared();

    void add(const std::shared_ptr<Engine>& engine);
    void remove(const Engine& engine);

    // The live instances whose Group setting is `group`: tracks with a host position first, in
    // that order, then the others in creation order; each with its number among the instances on
    // its track.
    [[nodiscard]] std::vector<GroupMember> members(protocol::Group group) const;

private:
    struct Entry
    {
        std::weak_ptr<Engine> engine;
        std::uint64_t serial = 0;
    };

    mutable std::mutex mutex;
    std::vector<Entry> entries;
    std::uint64_t nextSerial = 1;
};

} // namespace orizon::engine
