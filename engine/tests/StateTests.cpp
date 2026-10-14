#include "engine/SettingJson.h"
#include "engine/State.h"

#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

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

// Every value worth trying for a setting, from the protocol's table: a number's minimum, maximum
// and default; every value of an enum; each flag alone and all of them.
struct SettingCase
{
    const protocol::SettingDescriptor* setting = nullptr;
    double value = 0.0;
};

std::vector<SettingCase> everySettingValue()
{
    std::vector<SettingCase> cases;
    for (const protocol::SettingDescriptor& setting : protocol::kSettings) {
        std::vector<double> values;
        switch (setting.kind) {
        case protocol::SettingKind::kNumber:
            values = {setting.minimum, setting.maximum, setting.defaultValue};
            break;
        case protocol::SettingKind::kEnum:
            for (auto value = static_cast<int>(setting.minimum);
                 value <= static_cast<int>(setting.maximum); ++value) {
                values.push_back(value);
            }
            break;
        case protocol::SettingKind::kFlags:
            for (std::size_t bit = 0; bit < setting.valueNames.size(); ++bit) {
                values.push_back(static_cast<double>(1U << bit));
            }
            values.push_back(setting.maximum);
            break;
        }
        for (const double value : values) {
            cases.push_back({.setting = &setting, .value = value});
        }
    }
    return cases;
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const SettingCase& c, std::ostream* out)
{
    *out << c.setting->name << " = " << c.value;
}

class EverySetting : public ::testing::TestWithParam<SettingCase>
{
};

// Each survives a project's save and load, and the page's JSON form, unchanged.
TEST_P(EverySetting, RoundTripsThroughStateAndJson)
{
    const auto [setting, value] = GetParam();
    Settings settings;
    ASSERT_TRUE(settings.set(setting->id, value)) << "the table's own value is refused";
    EXPECT_EQ(deserializeState(serializeState(settings)).value(setting->id), value);
    const auto parsed = settingFromJson(setting->id, settingToJson(setting->id, value));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, value);
}

INSTANTIATE_TEST_SUITE_P(State, EverySetting, ::testing::ValuesIn(everySettingValue()),
                         [](const ::testing::TestParamInfo<SettingCase>& info) {
                             return std::string(info.param.setting->name) + "_" +
                                    std::to_string(info.index);
                         });

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
    EXPECT_EQ(settings.enumValue<protocol::Toggle>(SettingId::kAutoFreeze), protocol::Toggle::kOff);
}

// Version 1 states from 0.1.0 already carry a time base: one saved in Beats reopens in Beats.
// The file is what 0.1.0's serializeState wrote for a Beats project.
TEST(State, LoadsAVersion1GoldenFileSavedInBeats)
{
    const Settings settings = deserializeState(readTestFile("state-v1-beats.json"));
    EXPECT_EQ(settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase),
              protocol::TimeBase::kBeats);
    EXPECT_EQ(settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan),
              protocol::BeatSpan::kTwoBars);
    EXPECT_EQ(settings.value(SettingId::kChannelViews), 0b0101);
    EXPECT_EQ(settings.enumValue<ViewLayout>(SettingId::kViewLayout), ViewLayout::kStacked);
    EXPECT_EQ(settings.enumValue<TriggerSlope>(SettingId::kTriggerSlope), TriggerSlope::kFalling);
    EXPECT_EQ(settings.value(SettingId::kVerticalScale), -6.0);
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
    EXPECT_EQ(settings.enumValue<protocol::Toggle>(SettingId::kAutoFreeze), protocol::Toggle::kOn);
    // Settings added after this state was saved take their defaults.
    EXPECT_EQ(settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode),
              protocol::DisplayMode::kSingle);
    EXPECT_EQ(settings.enumValue<protocol::Group>(SettingId::kGroup), protocol::Group::kA);
}

TEST(State, DefaultsToABeatsSweepOfOneBar)
{
    const Settings settings = deserializeState(R"({"version": 2, "settings": {}})");
    EXPECT_EQ(settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase),
              protocol::TimeBase::kBeats);
    EXPECT_EQ(settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan),
              protocol::BeatSpan::kOneBar);
    EXPECT_EQ(settings.enumValue<protocol::Toggle>(SettingId::kAutoFreeze), protocol::Toggle::kOff);
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
