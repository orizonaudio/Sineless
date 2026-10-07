#include "engine/State.h"

#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace orizon::engine {
namespace {

using protocol::ChannelView;
using protocol::SettingId;
using protocol::TriggerMode;
using protocol::TriggerSlope;
using protocol::ViewLayout;

std::string readTestFile(const std::string& name)
{
    std::ifstream file(std::string(ORIZON_TEST_DATA_DIR) + "/" + name);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

double defaultOf(SettingId id)
{
    return Settings().value(id);
}

TEST(State, RoundTripsEverySetting)
{
    Settings settings;
    settings.set(SettingId::kTimeSpan, 1.5);
    settings.set(SettingId::kChannelViews, 0b1111);
    settings.set(SettingId::kTriggerSlope, 1.0);
    EXPECT_EQ(deserializeState(serializeState(settings)), settings);
}

TEST(State, WritesTheVersionEnvelope)
{
    const auto json = nlohmann::json::parse(serializeState(Settings()));
    EXPECT_EQ(json["version"], kStateVersion);
    EXPECT_EQ(json["settings"]["triggerMode"], "auto");
}

// One golden file per released state version proves that old projects still load.
TEST(State, LoadsTheVersion1GoldenFile)
{
    const Settings settings = deserializeState(readTestFile("state-v1.json"));
    EXPECT_EQ(settings.value(SettingId::kTimeSpan), 0.5);
    EXPECT_EQ(settings.value(SettingId::kVerticalScale), -12.0);
    EXPECT_EQ(settings.value(SettingId::kChannelViews), 0b1100);
    EXPECT_EQ(settings.enumValue<ViewLayout>(SettingId::kViewLayout), ViewLayout::kStacked);
    EXPECT_EQ(settings.enumValue<TriggerMode>(SettingId::kTriggerMode), TriggerMode::kNormal);
    EXPECT_EQ(settings.enumValue<ChannelView>(SettingId::kTriggerSource), ChannelView::kSide);
    EXPECT_EQ(settings.enumValue<TriggerSlope>(SettingId::kTriggerSlope), TriggerSlope::kFalling);
    EXPECT_EQ(settings.value(SettingId::kTriggerLevel), -0.25);
    EXPECT_EQ(settings.value(SettingId::kTriggerHysteresis), 0.05);
    EXPECT_EQ(settings.value(SettingId::kTriggerHoldOff), 0.1);
    // A version 1 state without a time base predates tempo sync and keeps seconds; other
    // settings added later take their defaults.
    EXPECT_EQ(settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase),
              protocol::TimeBase::kSeconds);
    EXPECT_EQ(settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan),
              protocol::BeatSpan::kOneBar);
}

TEST(State, LoadsTheVersion2GoldenFile)
{
    const Settings settings = deserializeState(readTestFile("state-v2.json"));
    EXPECT_EQ(settings.value(SettingId::kTimeSpan), 0.05);
    EXPECT_EQ(settings.value(SettingId::kVerticalScale), 3.0);
    EXPECT_EQ(settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase),
              protocol::TimeBase::kSeconds);
    EXPECT_EQ(settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan),
              protocol::BeatSpan::kTwoBars);
}

TEST(State, DefaultsToABeatsSweepOfOneBar)
{
    const Settings settings = deserializeState(R"({"version": 2, "settings": {}})");
    EXPECT_EQ(settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase),
              protocol::TimeBase::kBeats);
    EXPECT_EQ(settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan),
              protocol::BeatSpan::kOneBar);
}

TEST(State, KeepsDefaultsForMissingAndInvalidFieldsOnly)
{
    const Settings settings = deserializeState(R"({
        "version": 1,
        "settings": {
            "timeSpan": 99,
            "triggerMode": "sometimes",
            "channelViews": [],
            "triggerLevel": 0.5
        }
    })");
    EXPECT_EQ(settings.value(SettingId::kTimeSpan), defaultOf(SettingId::kTimeSpan));
    EXPECT_EQ(settings.value(SettingId::kTriggerMode), defaultOf(SettingId::kTriggerMode));
    EXPECT_EQ(settings.value(SettingId::kChannelViews), defaultOf(SettingId::kChannelViews));
    EXPECT_EQ(settings.value(SettingId::kTriggerLevel), 0.5);
}

TEST(State, IgnoresUnknownFieldsAndLoadsNewerVersions)
{
    const Settings settings = deserializeState(R"({
        "version": 7,
        "settings": {"triggerLevel": 0.5, "futureSetting": true},
        "futureSection": {}
    })");
    EXPECT_EQ(settings.value(SettingId::kTriggerLevel), 0.5);
}

TEST(State, FallsBackToDefaultsForUnreadableInput)
{
    for (const char* input : {"", "not json", "[1, 2]", R"({"version": 1})",
                              R"({"settings": "nope"})", R"({"settings": {"timeSpan": 0.5)"}) {
        EXPECT_EQ(deserializeState(input), Settings()) << input;
    }
}

} // namespace
} // namespace orizon::engine
