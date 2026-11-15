#include "engine/InstanceRegistry.h"

#include <atomic>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::DisplayMode;
using protocol::SettingId;

std::shared_ptr<Engine> engineOn(TrackInfo track = {})
{
    auto engine = std::make_shared<Engine>();
    engine->setTrackInfo(std::move(track));
    return engine;
}

std::vector<Engine*> enginesOf(const std::vector<Member>& members)
{
    std::vector<Engine*> engines;
    engines.reserve(members.size());
    for (const Member& member : members) {
        engines.push_back(member.engine.get());
    }
    return engines;
}

// Every instance in the process can be shown in any other's Multi, whatever its own display mode
// and settings: nothing an instance is set to keeps it off the list.
TEST(InstanceRegistry, ListsEveryInstanceWhateverItsSettings)
{
    InstanceRegistry registry;
    const auto first = engineOn();
    const auto inMulti = engineOn();
    inMulti->settings().set(SettingId::kDisplayMode, static_cast<double>(DisplayMode::kMulti));
    const auto second = engineOn();
    registry.add(first);
    registry.add(inMulti);
    registry.add(second);
    EXPECT_EQ(enginesOf(registry.members()),
              (std::vector<Engine*>{first.get(), inMulti.get(), second.get()}));
}

TEST(InstanceRegistry, OrdersByTrackPositionThenCreation)
{
    InstanceRegistry registry;
    const auto unplacedFirst = engineOn();
    const auto third = engineOn({.name = "Bass", .colour = std::nullopt, .index = 3});
    const auto unplacedSecond = engineOn();
    const auto first = engineOn({.name = "Kick", .colour = 0xff0000, .index = 1});
    for (const auto& engine : {unplacedFirst, third, unplacedSecond, first}) {
        registry.add(engine);
    }
    const auto members = registry.members();
    EXPECT_EQ(enginesOf(members),
              (std::vector<Engine*>{first.get(), third.get(), unplacedFirst.get(),
                                    unplacedSecond.get()}));
    EXPECT_EQ(members[0].track.name, "Kick");
    EXPECT_EQ(members[0].track.colour, 0xff0000u);
    EXPECT_LT(members[2].serial, members[3].serial);
}

std::vector<std::uint32_t> instancesOf(const std::vector<Member>& members)
{
    std::vector<std::uint32_t> instances;
    instances.reserve(members.size());
    for (const Member& member : members) {
        instances.push_back(member.instance);
    }
    return instances;
}

// Two instances around an effect on one track read as one track twice: each gets its number among
// the track's instances, in the order they were created, so their lanes can be told apart. Where
// the host gives track positions (VST3 hosts may), the position says which track.
TEST(InstanceRegistry, NumbersInstancesOnOneTrackByCreation)
{
    InstanceRegistry registry;
    const auto before = engineOn({.name = "Bass", .colour = 0x3060e0, .index = 3});
    const auto kick = engineOn({.name = "Kick", .colour = 0xff0000, .index = 1});
    const auto after = engineOn({.name = "Bass", .colour = 0x3060e0, .index = 3});
    for (const auto& engine : {before, kick, after}) {
        registry.add(engine);
    }
    const auto members = registry.members();
    ASSERT_EQ(enginesOf(members), (std::vector<Engine*>{kick.get(), before.get(), after.get()}));
    EXPECT_EQ(instancesOf(members), (std::vector<std::uint32_t>{0, 1, 2}))
        << "a track's only instance has no number";
}

// Where the host gives no position (an Audio Unit host gives the name alone), the same name says
// it; tracks the host has not named are never one track.
TEST(InstanceRegistry, NumbersByNameWhereTheHostGivesNoPosition)
{
    InstanceRegistry registry;
    const auto before = engineOn({.name = "Bass", .colour = std::nullopt, .index = std::nullopt});
    const auto unnamed = engineOn();
    const auto after = engineOn({.name = "Bass", .colour = std::nullopt, .index = std::nullopt});
    const auto alsoUnnamed = engineOn();
    for (const auto& engine : {before, unnamed, after, alsoUnnamed}) {
        registry.add(engine);
    }
    EXPECT_EQ(instancesOf(registry.members()), (std::vector<std::uint32_t>{1, 0, 2, 0}));
}

// The numbers are of the instances there are now: when one of a track's two goes away, the other
// is the track's only instance, and plain again.
TEST(InstanceRegistry, NumbersAgainWhenAnInstanceGoesAway)
{
    InstanceRegistry registry;
    const auto before = engineOn({.name = "Bass", .colour = std::nullopt, .index = 3});
    const auto after = engineOn({.name = "Bass", .colour = std::nullopt, .index = 3});
    registry.add(before);
    registry.add(after);
    ASSERT_EQ(instancesOf(registry.members()), (std::vector<std::uint32_t>{1, 2}));
    registry.remove(*before);
    EXPECT_EQ(instancesOf(registry.members()), (std::vector<std::uint32_t>{0}));
}

struct TrackPair
{
    const char* name;
    TrackInfo first;
    TrackInfo second;
    bool same;
};

class SameTrack : public ::testing::TestWithParam<TrackPair>
{
};

// Which instances count as on one host track, from what the host tells of each.
TEST_P(SameTrack, GoesByThePositionOrElseTheName)
{
    const TrackPair& pair = GetParam();
    EXPECT_EQ(sameTrack(pair.first, pair.second), pair.same);
    EXPECT_EQ(sameTrack(pair.second, pair.first), pair.same);
}

TrackInfo on(const char* name, std::optional<int> position = std::nullopt)
{
    return {.name = name, .colour = std::nullopt, .index = position};
}

TrackPair pairOf(const char* name, TrackInfo first, TrackInfo second, bool same)
{
    return {.name = name, .first = std::move(first), .second = std::move(second), .same = same};
}

INSTANTIATE_TEST_SUITE_P(
    Pairs, SameTrack,
    ::testing::Values(pairOf("OnePositionUnderOneName", on("Bass", 3), on("Bass", 3), true),
                      // A track renamed between the host's two reports is still that track.
                      pairOf("OnePositionUnderTwoNames", on("Bass", 3), on("Bass DI", 3), true),
                      pairOf("TwoPositionsUnderOneName", on("Bass", 3), on("Bass", 4), false),
                      pairOf("APositionAndNoneUnderOneName", on("Bass", 3), on("Bass"), false),
                      pairOf("NoPositionsAndOneName", on("Bass"), on("Bass"), true),
                      pairOf("NoPositionsAndTwoNames", on("Bass"), on("Kick"), false),
                      pairOf("NoPositionsAndNoNames", on(""), on(""), false)),
    [](const ::testing::TestParamInfo<TrackPair>& info) { return std::string(info.param.name); });

TEST(InstanceRegistry, ForgetsRemovedAndEndedInstances)
{
    InstanceRegistry registry;
    const auto kept = engineOn();
    const auto removed = engineOn();
    auto ended = engineOn();
    registry.add(kept);
    registry.add(removed);
    registry.add(ended);
    registry.remove(*removed);
    ended.reset(); // Never removed: the weak reference lapses on its own.
    EXPECT_EQ(enginesOf(registry.members()), (std::vector<Engine*>{kept.get()}));
}

TEST(InstanceRegistry, KeepsAHeldMemberAliveAfterItsInstanceEnds)
{
    InstanceRegistry registry;
    auto engine = engineOn();
    const std::weak_ptr<Engine> watch = engine;
    registry.add(engine);
    const auto members = registry.members();
    registry.remove(*engine);
    engine.reset();
    EXPECT_FALSE(watch.expired()) << "the reader still holds the engine";
    EXPECT_TRUE(registry.members().empty());
}

TEST(InstanceRegistry, SupportsConcurrentChangesAndReads)
{
    InstanceRegistry registry;
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done.load()) {
            for (const Member& member : registry.members()) {
                EXPECT_NE(member.engine, nullptr);
            }
        }
    });
    for (int round = 0; round < 200; ++round) {
        const auto engine =
            engineOn({.name = "Track", .colour = std::nullopt, .index = std::nullopt});
        registry.add(engine);
        engine->setTrackInfo({.name = "Renamed", .colour = std::nullopt, .index = round});
        registry.remove(*engine);
    }
    done.store(true);
    reader.join();
    EXPECT_TRUE(registry.members().empty());
}

} // namespace
} // namespace orizon::engine
