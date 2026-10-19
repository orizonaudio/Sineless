#include "engine/SettingJson.h"
#include "engine/State.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <ostream>
#include <span>
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

// What Sineless 0.5.x saved in state-v2-0.5.json: every setting away from its default then, using
// the settings and values added since 0.2.0 where there are some. Written with the protocol's
// constants rather than the saved names, so a renamed key or value fails to load it.
struct GoldenValue
{
    SettingId id{};
    double value = 0.0;
};

constexpr double flag(ChannelView view)
{
    return static_cast<double>(1U << static_cast<unsigned>(view));
}

constexpr double asValue(auto enumValue)
{
    return static_cast<double>(enumValue);
}

constexpr std::array kVersion2Release05Values{
    GoldenValue{.id = SettingId::kTimeSpan, .value = 0.25},
    GoldenValue{.id = SettingId::kVerticalScale, .value = -9.0},
    GoldenValue{.id = SettingId::kChannelViews,
                .value = flag(ChannelView::kRight) + flag(ChannelView::kSide)},
    // viewLayout: stacked in the file, which is the default now: the new instance's file holds the
    // layout away from it.
    GoldenValue{.id = SettingId::kTriggerMode, .value = asValue(TriggerMode::kSingle)},
    GoldenValue{.id = SettingId::kTriggerSource, .value = asValue(ChannelView::kSide)},
    GoldenValue{.id = SettingId::kTriggerSlope, .value = asValue(TriggerSlope::kFalling)},
    GoldenValue{.id = SettingId::kTriggerLevel, .value = -0.25},
    GoldenValue{.id = SettingId::kTriggerHysteresis, .value = 0.05},
    GoldenValue{.id = SettingId::kTriggerHoldOff, .value = 0.125},
    GoldenValue{.id = SettingId::kTimeBase, .value = asValue(protocol::TimeBase::kSeconds)},
    GoldenValue{.id = SettingId::kBeatSpan, .value = asValue(protocol::BeatSpan::kSixteenBars)},
    GoldenValue{.id = SettingId::kAutoFreeze, .value = asValue(protocol::Toggle::kOn)},
    GoldenValue{.id = SettingId::kDisplayMode, .value = asValue(protocol::DisplayMode::kCompare)},
    GoldenValue{.id = SettingId::kGroup, .value = asValue(protocol::Group::kC)},
    GoldenValue{.id = SettingId::kHoverReadout, .value = asValue(protocol::Toggle::kOn)},
    GoldenValue{.id = SettingId::kConcertPitch, .value = 432.0},
};

// What Sineless 0.5.x saved in state-v2-0.5-new-instance.json: a new instance, every setting at
// the default of its time, as serializeState of v0.5.2 wrote it. It holds the layout overlaid,
// 0.5.x's default, which projects saved by an untouched instance keep although new instances are
// stacked since.
constexpr std::array kVersion2Release05NewInstanceValues{
    GoldenValue{.id = SettingId::kViewLayout, .value = asValue(ViewLayout::kOverlaid)},
};

// Golden states saved by released builds that added settings without a state version raise,
// each with the values it holds. A release that adds a setting adds its own file here; the
// files already here stay as their release saved them.
struct GoldenState
{
    const char* file = nullptr;
    std::span<const GoldenValue> values;
};

constexpr std::array kGoldenStates{
    GoldenState{.file = "state-v2-0.5.json", .values = kVersion2Release05Values},
    GoldenState{.file = "state-v2-0.5-new-instance.json",
                .values = kVersion2Release05NewInstanceValues},
};

// The saved name of every enum and flags value, as each release wrote it. One state can hold
// only one value of a setting, so the other released names are pinned here.
// Each released list of value names, with the state version its release saved: a state is built at
// that version, so that its names go through every migration since, as a project of that release
// does.
struct GoldenValueNames
{
    const char* file = nullptr;
    int version = 0;
};

constexpr std::array kGoldenValueNames{
    GoldenValueNames{.file = "value-names-0.5.json", .version = 2},
};

struct SavedSetting
{
    const protocol::SettingDescriptor* setting = nullptr;
};

std::vector<SavedSetting> everySetting()
{
    std::vector<SavedSetting> cases;
    cases.reserve(protocol::kSettings.size());
    for (const protocol::SettingDescriptor& setting : protocol::kSettings) {
        cases.push_back({.setting = &setting});
    }
    return cases;
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const SavedSetting& c, std::ostream* out)
{
    *out << c.setting->name;
}

class SavedProject : public ::testing::TestWithParam<SavedSetting>
{
};

// A project saved by a released build (0.3.0 to 0.5.x so far) reopens with each setting as it
// was saved, not reset to its default. A setting no golden state holds fails until one saved by
// the release that adds it is listed in kGoldenStates.
TEST_P(SavedProject, ReopensWithEachSettingAsSaved)
{
    const protocol::SettingDescriptor& setting = *GetParam().setting;
    int heldBy = 0;
    for (const GoldenState& golden : kGoldenStates) {
        const auto expected = std::ranges::find(golden.values, setting.id, &GoldenValue::id);
        if (expected == golden.values.end()) {
            continue;
        }
        ++heldBy;
        const std::string text = readTestFile(golden.file);
        const auto saved = nlohmann::json::parse(text).at("settings");
        ASSERT_TRUE(saved.contains(std::string(setting.name)))
            << setting.name << " is not in " << golden.file;
        ASSERT_NE(expected->value, defaultOf(setting.id))
            << "a default value would load even if its saved form were no longer read";
        EXPECT_EQ(deserializeState(text).value(setting.id), expected->value) << golden.file;
    }
    EXPECT_GT(heldBy, 0) << setting.name
                         << " is in no golden state: add one saved by the release that adds it";
}

INSTANTIATE_TEST_SUITE_P(State, SavedProject, ::testing::ValuesIn(everySetting()),
                         [](const ::testing::TestParamInfo<SavedSetting>& info) {
                             return std::string(info.param.setting->name);
                         });

// Every value of every enum and flags setting, from the protocol's table.
struct NamedValue
{
    const protocol::SettingDescriptor* setting = nullptr;
    std::size_t index = 0;
};

std::vector<NamedValue> everyNamedValue()
{
    std::vector<NamedValue> cases;
    for (const protocol::SettingDescriptor& setting : protocol::kSettings) {
        for (std::size_t index = 0; index < setting.valueNames.size(); ++index) {
            cases.push_back({.setting = &setting, .index = index});
        }
    }
    return cases;
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const NamedValue& c, std::ostream* out)
{
    *out << c.setting->name << "." << c.setting->valueNames[c.index];
}

class SavedValueName : public ::testing::TestWithParam<NamedValue>
{
};

// A project saved with any value of a mode, group, span, trigger or channel reopens with that
// value, under the name its release saved it with. Saved projects hold names, so the value is
// looked up by name and the protocol may reorder its values. A value no released name list holds
// fails until the list of the release that adds it is in kGoldenValueNames.
TEST_P(SavedValueName, ReopensAsTheValueSaved)
{
    const auto [setting, index] = GetParam();
    const bool isFlags = setting->kind == protocol::SettingKind::kFlags;
    const double expected = isFlags ? static_cast<double>(1U << index) : static_cast<double>(index);
    const std::string key(setting->name);
    const std::string name(setting->valueNames[index]);
    int heldBy = 0;
    for (const GoldenValueNames& golden : kGoldenValueNames) {
        const auto released = nlohmann::json::parse(readTestFile(golden.file));
        const auto names = released.find(key);
        if (names == released.end() ||
            std::find(names->begin(), names->end(), name) == names->end()) {
            continue;
        }
        ++heldBy;
        const nlohmann::json saved = isFlags ? nlohmann::json::array({name}) : nlohmann::json(name);
        const nlohmann::json state = {{"version", golden.version}, {"settings", {{key, saved}}}};
        EXPECT_EQ(deserializeState(state.dump()).value(setting->id), expected)
            << golden.file << ": " << name;
    }
    EXPECT_GT(heldBy, 0) << "no released name list holds this value: add the list of the "
                            "release that adds it";
}

// A name a release saved is still one the protocol reads: renaming or dropping a value would
// reset it to the default in every project saved with it.
TEST(State, KeepsEveryReleasedValueName)
{
    for (const GoldenValueNames& golden : kGoldenValueNames) {
        const auto released = nlohmann::json::parse(readTestFile(golden.file));
        for (const auto& [key, names] : released.items()) {
            const auto setting = std::ranges::find(protocol::kSettings, std::string_view(key),
                                                   &protocol::SettingDescriptor::name);
            ASSERT_NE(setting, protocol::kSettings.end()) << golden.file << ": " << key;
            for (const auto& name : names) {
                const std::string_view text = name.get_ref<const std::string&>();
                EXPECT_NE(std::ranges::find(setting->valueNames, text), setting->valueNames.end())
                    << golden.file << ": " << key << "." << name;
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(State, SavedValueName, ::testing::ValuesIn(everyNamedValue()),
                         [](const ::testing::TestParamInfo<NamedValue>& info) {
                             return std::string(info.param.setting->name) + "_" +
                                    std::string(info.param.setting->valueNames[info.param.index]);
                         });

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
