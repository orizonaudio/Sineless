#include "engine/InstanceRegistry.h"

#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::Group;
using protocol::SettingId;

std::shared_ptr<Engine> engineIn(Group group, TrackInfo track = {})
{
    auto engine = std::make_shared<Engine>();
    engine->settings().set(SettingId::kGroup, static_cast<double>(group));
    engine->setTrackInfo(std::move(track));
    return engine;
}

std::vector<Engine*> enginesOf(const std::vector<GroupMember>& members)
{
    std::vector<Engine*> engines;
    engines.reserve(members.size());
    for (const GroupMember& member : members) {
        engines.push_back(member.engine.get());
    }
    return engines;
}

TEST(InstanceRegistry, ListsTheInstancesOfAGroup)
{
    InstanceRegistry registry;
    const auto first = engineIn(Group::kA);
    const auto other = engineIn(Group::kB);
    const auto second = engineIn(Group::kA);
    registry.add(first);
    registry.add(other);
    registry.add(second);
    EXPECT_EQ(enginesOf(registry.members(Group::kA)),
              (std::vector<Engine*>{first.get(), second.get()}));
    EXPECT_EQ(enginesOf(registry.members(Group::kB)), (std::vector<Engine*>{other.get()}));
    EXPECT_TRUE(registry.members(Group::kD).empty());
}

TEST(InstanceRegistry, FollowsGroupChanges)
{
    InstanceRegistry registry;
    const auto engine = engineIn(Group::kA);
    registry.add(engine);
    engine->settings().set(SettingId::kGroup, static_cast<double>(Group::kC));
    EXPECT_TRUE(registry.members(Group::kA).empty());
    EXPECT_EQ(registry.members(Group::kC).size(), 1u);
}

TEST(InstanceRegistry, OrdersByTrackPositionThenCreation)
{
    InstanceRegistry registry;
    const auto unplacedFirst = engineIn(Group::kA);
    const auto third = engineIn(Group::kA, {.name = "Bass", .colour = std::nullopt, .index = 3});
    const auto unplacedSecond = engineIn(Group::kA);
    const auto first = engineIn(Group::kA, {.name = "Kick", .colour = 0xff0000, .index = 1});
    for (const auto& engine : {unplacedFirst, third, unplacedSecond, first}) {
        registry.add(engine);
    }
    const auto members = registry.members(Group::kA);
    EXPECT_EQ(enginesOf(members),
              (std::vector<Engine*>{first.get(), third.get(), unplacedFirst.get(),
                                    unplacedSecond.get()}));
    EXPECT_EQ(members[0].track.name, "Kick");
    EXPECT_EQ(members[0].track.colour, 0xff0000u);
    EXPECT_LT(members[2].serial, members[3].serial);
}

TEST(InstanceRegistry, ForgetsRemovedAndEndedInstances)
{
    InstanceRegistry registry;
    const auto kept = engineIn(Group::kA);
    const auto removed = engineIn(Group::kA);
    auto ended = engineIn(Group::kA);
    registry.add(kept);
    registry.add(removed);
    registry.add(ended);
    registry.remove(*removed);
    ended.reset(); // Never removed: the weak reference lapses on its own.
    EXPECT_EQ(enginesOf(registry.members(Group::kA)), (std::vector<Engine*>{kept.get()}));
}

TEST(InstanceRegistry, KeepsAHeldMemberAliveAfterItsInstanceEnds)
{
    InstanceRegistry registry;
    auto engine = engineIn(Group::kA);
    const std::weak_ptr<Engine> watch = engine;
    registry.add(engine);
    const auto members = registry.members(Group::kA);
    registry.remove(*engine);
    engine.reset();
    EXPECT_FALSE(watch.expired()) << "the reader still holds the engine";
    EXPECT_TRUE(registry.members(Group::kA).empty());
}

TEST(InstanceRegistry, SupportsConcurrentChangesAndReads)
{
    InstanceRegistry registry;
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done.load()) {
            for (const GroupMember& member : registry.members(Group::kA)) {
                EXPECT_NE(member.engine, nullptr);
            }
        }
    });
    for (int round = 0; round < 200; ++round) {
        const auto engine =
            engineIn(Group::kA, {.name = "Track", .colour = std::nullopt, .index = std::nullopt});
        registry.add(engine);
        engine->setTrackInfo({.name = "Renamed", .colour = std::nullopt, .index = round});
        engine->settings().set(SettingId::kGroup, static_cast<double>(Group::kB));
        registry.remove(*engine);
    }
    done.store(true);
    reader.join();
    EXPECT_TRUE(registry.members(Group::kA).empty());
}

} // namespace
} // namespace orizon::engine
