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
};

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
    // that order, then the others in creation order.
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
