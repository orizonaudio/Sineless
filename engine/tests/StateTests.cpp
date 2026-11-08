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
    EXPECT_EQ(deserializeState(serializeState(settings)).settings.value(setting->id), value);
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
    const Settings settings = deserializeState(readTestFile("state-v1.json")).settings;
    EXPECT_EQ(settings.value(SettingId::kTriggerSpan), 0.5);
    EXPECT_EQ(settings.value(SettingId::kVerticalScale), -12.0);
    EXPECT_EQ(settings.value(SettingId::kChannelViews), 0b1100);
    EXPECT_EQ(settings.enumValue<ViewLayout>(SettingId::kViewLayout), ViewLayout::kStacked);
    EXPECT_EQ(settings.enumValue<TriggerMode>(SettingId::kTriggerMode), TriggerMode::kNormal);
    EXPECT_EQ(settings.enumValue<ChannelView>(SettingId::kTriggerSource), ChannelView::kSide);
    EXPECT_EQ(settings.enumValue<TriggerSlope>(SettingId::kTriggerSlope), TriggerSlope::kFalling);
    EXPECT_EQ(settings.value(SettingId::kTriggerLevel), -0.25);
    EXPECT_EQ(settings.value(SettingId::kTriggerHysteresis), 0.05);
    EXPECT_EQ(settings.value(SettingId::kTriggerHoldOff), 0.1);
    // A version 1 state without a time base predates tempo sync and keeps its triggered seconds;
    // other settings added later take their defaults.
    EXPECT_EQ(settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase),
              protocol::TimeBase::kTrigger);
    EXPECT_EQ(settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan),
              protocol::BeatSpan::kOneBar);
}

// Every golden state, whatever its version.
constexpr std::array kEveryGoldenStateFile{
    "state-v1.json",
    "state-v1-beats.json",
    "state-v2.json",
    "state-v2-0.5.json",
    "state-v2-0.5-new-instance.json",
    "state-v3.json",
    "state-v4.json",
    "state-v5.json",
    "state-v5-trigger-position.json",
    "state-v5-editor-size.json",
};

// A project saved before Auto-freeze existed opens with it on, as a new instance does (FR-11):
// a missing setting takes its default, with no migration to keep the old behaviour.
TEST(State, AStateWithoutAutoFreezeOpensWithItOn)
{
    int without = 0;
    for (const char* file : kEveryGoldenStateFile) {
        const std::string text = readTestFile(file);
        if (nlohmann::json::parse(text).at("settings").contains("autoFreeze")) {
            continue;
        }
        ++without;
        EXPECT_EQ(
            deserializeState(text).settings.enumValue<protocol::Toggle>(SettingId::kAutoFreeze),
            protocol::Toggle::kOn)
            << file;
    }
    EXPECT_GT(without, 0) << "no golden state predates Auto-freeze";
}

// A project saved before the trigger Position was a setting reopens with the trigger point where
// it was then, 10% from the left.
TEST(State, AStateWithoutATriggerPositionOpensAtTheOldPlace)
{
    int without = 0;
    for (const char* file : kEveryGoldenStateFile) {
        const std::string text = readTestFile(file);
        if (nlohmann::json::parse(text).at("settings").contains("triggerPosition")) {
            continue;
        }
        ++without;
        EXPECT_EQ(deserializeState(text).settings.value(SettingId::kTriggerPosition), 0.1) << file;
    }
    EXPECT_GT(without, 0) << "no golden state predates the trigger Position";
}

// Version 1 states from 0.1.0 already carry a time base: one saved in Beats reopens in Beats.
// The file is what 0.1.0's serializeState wrote for a Beats project.
TEST(State, LoadsAVersion1GoldenFileSavedInBeats)
{
    const Settings settings = deserializeState(readTestFile("state-v1-beats.json")).settings;
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
    const Settings settings = deserializeState(readTestFile("state-v2.json")).settings;
    // Saved in the Free trigger mode at 50 ms: it opens in Free at Free's shortest span, with the
    // span kept for Trigger and the trigger mode at its default.
    EXPECT_EQ(settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase),
              protocol::TimeBase::kFree);
    EXPECT_EQ(settings.value(SettingId::kFreeSpan), protocol::kShortestFreeSpanSeconds);
    EXPECT_EQ(settings.value(SettingId::kTriggerSpan), 0.05);
    EXPECT_EQ(settings.enumValue<TriggerMode>(SettingId::kTriggerMode), TriggerMode::kAuto);
    EXPECT_EQ(settings.value(SettingId::kVerticalScale), 3.0);
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

// Its time span, 0.25 s in Seconds, is `triggerSpan` since version 3: MigratesEachKindOfProject
// checks it. Saved in Compare, it opens in Multi, with Compare on since version 4:
// OpensAProjectSavedInCompareInMultiWithCompareOn checks the switch.
constexpr std::array kVersion2Release05Values{
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
    GoldenValue{.id = SettingId::kTimeBase, .value = asValue(protocol::TimeBase::kTrigger)},
    GoldenValue{.id = SettingId::kBeatSpan, .value = asValue(protocol::BeatSpan::kSixteenBars)},
    GoldenValue{.id = SettingId::kDisplayMode, .value = asValue(protocol::DisplayMode::kMulti)},
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
    // Off by default then, on since: the one golden that holds it away from today's default.
    GoldenValue{.id = SettingId::kAutoFreeze, .value = asValue(protocol::Toggle::kOff)},
};

// What state version 3 saves, in state-v3.json: every setting away from its default, the two spans
// and the Free time base included. Saved in Compare, it opens in Multi (and with Compare on).
constexpr std::array kVersion3Values{
    GoldenValue{.id = SettingId::kTriggerSpan, .value = 0.25},
    GoldenValue{.id = SettingId::kFreeSpan, .value = 2.0},
    GoldenValue{.id = SettingId::kVerticalScale, .value = -9.0},
    GoldenValue{.id = SettingId::kChannelViews,
                .value = flag(ChannelView::kRight) + flag(ChannelView::kSide)},
    GoldenValue{.id = SettingId::kViewLayout, .value = asValue(ViewLayout::kOverlaid)},
    GoldenValue{.id = SettingId::kTriggerMode, .value = asValue(TriggerMode::kSingle)},
    GoldenValue{.id = SettingId::kTriggerSource, .value = asValue(ChannelView::kSide)},
    GoldenValue{.id = SettingId::kReadoutSource, .value = asValue(ChannelView::kRight)},
    GoldenValue{.id = SettingId::kTriggerSlope, .value = asValue(TriggerSlope::kFalling)},
    GoldenValue{.id = SettingId::kTriggerLevel, .value = -0.25},
    GoldenValue{.id = SettingId::kTriggerHysteresis, .value = 0.05},
    GoldenValue{.id = SettingId::kTriggerHoldOff, .value = 0.125},
    GoldenValue{.id = SettingId::kTimeBase, .value = asValue(protocol::TimeBase::kFree)},
    GoldenValue{.id = SettingId::kBeatSpan, .value = asValue(protocol::BeatSpan::kSixteenBars)},
    GoldenValue{.id = SettingId::kDisplayMode, .value = asValue(protocol::DisplayMode::kMulti)},
    GoldenValue{.id = SettingId::kGroup, .value = asValue(protocol::Group::kC)},
    GoldenValue{.id = SettingId::kHoverReadout, .value = asValue(protocol::Toggle::kOn)},
    GoldenValue{.id = SettingId::kConcertPitch, .value = 432.0},
};

// What state version 4 saves, in state-v4.json: every setting away from its default, Compare on
// in Multi included.
constexpr std::array kVersion4Values{
    GoldenValue{.id = SettingId::kTriggerSpan, .value = 0.25},
    GoldenValue{.id = SettingId::kFreeSpan, .value = 2.0},
    GoldenValue{.id = SettingId::kVerticalScale, .value = -9.0},
    GoldenValue{.id = SettingId::kChannelViews,
                .value = flag(ChannelView::kRight) + flag(ChannelView::kSide)},
    GoldenValue{.id = SettingId::kViewLayout, .value = asValue(ViewLayout::kOverlaid)},
    GoldenValue{.id = SettingId::kTriggerMode, .value = asValue(TriggerMode::kSingle)},
    GoldenValue{.id = SettingId::kTriggerSource, .value = asValue(ChannelView::kSide)},
    GoldenValue{.id = SettingId::kReadoutSource, .value = asValue(ChannelView::kRight)},
    GoldenValue{.id = SettingId::kTriggerSlope, .value = asValue(TriggerSlope::kFalling)},
    GoldenValue{.id = SettingId::kTriggerLevel, .value = -0.25},
    GoldenValue{.id = SettingId::kTriggerHysteresis, .value = 0.05},
    GoldenValue{.id = SettingId::kTriggerHoldOff, .value = 0.125},
    GoldenValue{.id = SettingId::kTimeBase, .value = asValue(protocol::TimeBase::kFree)},
    GoldenValue{.id = SettingId::kBeatSpan, .value = asValue(protocol::BeatSpan::kSixteenBars)},
    GoldenValue{.id = SettingId::kDisplayMode, .value = asValue(protocol::DisplayMode::kMulti)},
    GoldenValue{.id = SettingId::kGroup, .value = asValue(protocol::Group::kC)},
    GoldenValue{.id = SettingId::kCompare, .value = asValue(protocol::Toggle::kOn)},
    GoldenValue{.id = SettingId::kHoverReadout, .value = asValue(protocol::Toggle::kOn)},
    GoldenValue{.id = SettingId::kConcertPitch, .value = 432.0},
};

// What state version 5 saves, in state-v5.json: every setting away from its default, with a beat
// span that only version 5 names.
constexpr std::array kVersion5Values{
    GoldenValue{.id = SettingId::kTriggerSpan, .value = 0.25},
    GoldenValue{.id = SettingId::kAutoFreeze, .value = asValue(protocol::Toggle::kOff)},
    GoldenValue{.id = SettingId::kFreeSpan, .value = 2.0},
    GoldenValue{.id = SettingId::kVerticalScale, .value = -9.0},
    GoldenValue{.id = SettingId::kChannelViews,
                .value = flag(ChannelView::kRight) + flag(ChannelView::kSide)},
    GoldenValue{.id = SettingId::kViewLayout, .value = asValue(ViewLayout::kOverlaid)},
    GoldenValue{.id = SettingId::kTriggerMode, .value = asValue(TriggerMode::kSingle)},
    GoldenValue{.id = SettingId::kTriggerSource, .value = asValue(ChannelView::kSide)},
    GoldenValue{.id = SettingId::kReadoutSource, .value = asValue(ChannelView::kRight)},
    GoldenValue{.id = SettingId::kTriggerSlope, .value = asValue(TriggerSlope::kFalling)},
    GoldenValue{.id = SettingId::kTriggerLevel, .value = -0.25},
    GoldenValue{.id = SettingId::kTriggerHysteresis, .value = 0.05},
    GoldenValue{.id = SettingId::kTriggerHoldOff, .value = 0.125},
    GoldenValue{.id = SettingId::kTimeBase, .value = asValue(protocol::TimeBase::kFree)},
    GoldenValue{.id = SettingId::kBeatSpan, .value = asValue(protocol::BeatSpan::kThirdBar)},
    GoldenValue{.id = SettingId::kDisplayMode, .value = asValue(protocol::DisplayMode::kMulti)},
    GoldenValue{.id = SettingId::kGroup, .value = asValue(protocol::Group::kC)},
    GoldenValue{.id = SettingId::kCompare, .value = asValue(protocol::Toggle::kOn)},
    GoldenValue{.id = SettingId::kHoverReadout, .value = asValue(protocol::Toggle::kOn)},
    GoldenValue{.id = SettingId::kMeter, .value = asValue(protocol::Toggle::kOn)},
    GoldenValue{.id = SettingId::kConcertPitch, .value = 432.0},
    GoldenValue{.id = SettingId::kGridTypeBeats, .value = asValue(protocol::GridType::kNone)},
    GoldenValue{.id = SettingId::kGridTypeFree, .value = asValue(protocol::GridType::kNone)},
    GoldenValue{.id = SettingId::kGridTypeTrigger, .value = asValue(protocol::GridType::kNone)},
    GoldenValue{.id = SettingId::kGridTimeBeats, .value = asValue(protocol::GridDensity::kFine)},
    GoldenValue{.id = SettingId::kGridTimeFree, .value = asValue(protocol::GridDensity::kCoarse)},
    GoldenValue{.id = SettingId::kGridTimeTrigger, .value = asValue(protocol::GridDensity::kFine)},
    GoldenValue{.id = SettingId::kGridLevelBeats, .value = asValue(protocol::GridDensity::kCoarse)},
    GoldenValue{.id = SettingId::kGridLevelFree, .value = asValue(protocol::GridDensity::kFine)},
    GoldenValue{.id = SettingId::kGridLevelTrigger,
                .value = asValue(protocol::GridDensity::kCoarse)},
    GoldenValue{.id = SettingId::kBeatDivision,
                .value = asValue(protocol::BeatDivision::kTriplets)},
    GoldenValue{.id = SettingId::kLaneChannel, .value = asValue(protocol::ChannelView::kSide)},
};

// The version 5 state with the trigger Position, which 0.8 added: saved by the build that adds it,
// as the next release saves it.
constexpr std::array kVersion5TriggerPositionValues{
    GoldenValue{.id = SettingId::kTriggerPosition, .value = 0.35},
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
    GoldenState{.file = "state-v3.json", .values = kVersion3Values},
    GoldenState{.file = "state-v4.json", .values = kVersion4Values},
    GoldenState{.file = "state-v5.json", .values = kVersion5Values},
    GoldenState{.file = "state-v5-trigger-position.json", .values = kVersion5TriggerPositionValues},
    // The same settings, with the editor's size beside them (written by hand, as 8.4 has it).
    GoldenState{.file = "state-v5-editor-size.json", .values = kVersion5TriggerPositionValues},
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
    GoldenValueNames{.file = "value-names-0.8.json", .version = 3},
    GoldenValueNames{.file = "value-names-0.9.json", .version = 5},
};

// Released names a migration maps to another value, rather than names the protocol still reads:
// "seconds" became Trigger or Free, the Free trigger mode Auto (version 3):
// MigratesEachKindOfProject checks what each becomes. The Compare display mode became Multi with
// Compare on (version 4): OpensEachReleasedDisplayModeAsSaved checks it. The beat spans counted in
// beats became the bar spans they equal in 4/4, or 1/8 bar (version 5): SavedBeatSpan checks them.
struct MigratedName
{
    std::string_view setting;
    std::string_view name;
};

constexpr std::array kMigratedNames{
    MigratedName{.setting = "timeBase", .name = "seconds"},
    MigratedName{.setting = "triggerMode", .name = "free"},
    MigratedName{.setting = "displayMode", .name = "compare"},
    MigratedName{.setting = "beatSpan", .name = "sixteenthBeat"},
    MigratedName{.setting = "beatSpan", .name = "eighthBeat"},
    MigratedName{.setting = "beatSpan", .name = "quarterBeat"},
    MigratedName{.setting = "beatSpan", .name = "halfBeat"},
    MigratedName{.setting = "beatSpan", .name = "oneBeat"},
    MigratedName{.setting = "beatSpan", .name = "twoBeats"},
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
        EXPECT_EQ(deserializeState(text).settings.value(setting.id), expected->value)
            << golden.file;
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
        EXPECT_EQ(deserializeState(state.dump()).settings.value(setting->id), expected)
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
                const bool migrated = std::ranges::any_of(kMigratedNames, [&](const auto& m) {
                    return m.setting == key && m.name == text;
                });
                EXPECT_TRUE(migrated || std::ranges::find(setting->valueNames, text) !=
                                            setting->valueNames.end())
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

// A beat span saved before version 5, under its released name, and the span it opens as: a span
// in beats as the bar span it equals in 4/4 (two beats are half a bar, one beat a quarter), those
// shorter than 1/8 bar as 1/8 bar; a span in bars as itself.
struct SavedSpan
{
    std::string_view name;
    protocol::BeatSpan span{};
};

constexpr std::array kSavedSpans{
    SavedSpan{.name = "sixteenthBeat", .span = protocol::BeatSpan::kEighthBar},
    SavedSpan{.name = "eighthBeat", .span = protocol::BeatSpan::kEighthBar},
    SavedSpan{.name = "quarterBeat", .span = protocol::BeatSpan::kEighthBar},
    SavedSpan{.name = "halfBeat", .span = protocol::BeatSpan::kEighthBar},
    SavedSpan{.name = "oneBeat", .span = protocol::BeatSpan::kQuarterBar},
    SavedSpan{.name = "twoBeats", .span = protocol::BeatSpan::kHalfBar},
    SavedSpan{.name = "oneBar", .span = protocol::BeatSpan::kOneBar},
    SavedSpan{.name = "twoBars", .span = protocol::BeatSpan::kTwoBars},
    SavedSpan{.name = "fourBars", .span = protocol::BeatSpan::kFourBars},
    SavedSpan{.name = "eightBars", .span = protocol::BeatSpan::kEightBars},
    SavedSpan{.name = "sixteenBars", .span = protocol::BeatSpan::kSixteenBars},
};

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const SavedSpan& c, std::ostream* out)
{
    *out << c.name;
}

class SavedBeatSpan : public ::testing::TestWithParam<SavedSpan>
{
};

// A project saved with a beat span before version 5 (0.5 and 0.8 saved every one of these names)
// reopens at the nearest span there is, not at the default.
TEST_P(SavedBeatSpan, OpensAsTheNearestSpan)
{
    const SavedSpan& c = GetParam();
    for (const int version : {2, 3, 4}) {
        const nlohmann::json state = {{"version", version},
                                      {"settings", {{"beatSpan", std::string(c.name)}}}};
        EXPECT_EQ(deserializeState(state.dump())
                      .settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan),
                  c.span)
            << "version " << version;
    }
}

INSTANTIATE_TEST_SUITE_P(State, SavedBeatSpan, ::testing::ValuesIn(kSavedSpans),
                         [](const ::testing::TestParamInfo<SavedSpan>& info) {
                             return std::string(info.param.name);
                         });

TEST(State, DefaultsToABeatsSweepOfOneBar)
{
    const Settings settings = deserializeState(R"({"version": 2, "settings": {}})").settings;
    EXPECT_EQ(settings.enumValue<protocol::TimeBase>(SettingId::kTimeBase),
              protocol::TimeBase::kBeats);
    EXPECT_EQ(settings.enumValue<protocol::BeatSpan>(SettingId::kBeatSpan),
              protocol::BeatSpan::kOneBar);
    EXPECT_EQ(settings.enumValue<protocol::Toggle>(SettingId::kAutoFreeze), protocol::Toggle::kOn);
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
    })")
                                  .settings;
    EXPECT_EQ(settings.value(SettingId::kTriggerSpan), defaultOf(SettingId::kTriggerSpan));
    EXPECT_EQ(settings.value(SettingId::kFreeSpan), defaultOf(SettingId::kFreeSpan));
    EXPECT_EQ(settings.value(SettingId::kTriggerMode), defaultOf(SettingId::kTriggerMode));
    EXPECT_EQ(settings.value(SettingId::kChannelViews), defaultOf(SettingId::kChannelViews));
    EXPECT_EQ(settings.value(SettingId::kTriggerLevel), 0.5);
}

// A project saved before Free was a time base (state version 2), and what it opens as: in Free
// where its window ran free (the Free trigger mode, or a span above 1 s), else in Trigger, each
// span where its range holds the saved one, and its default where none did. A missing or invalid
// span counts as the old default, 1 s.
struct Migration
{
    const char* name = nullptr;
    const char* saved = nullptr; // The settings object of a version 2 state.
    protocol::TimeBase timeBase{};
    double triggerSpan = 0.0;
    double freeSpan = 0.0;
    TriggerMode triggerMode{};
};

using protocol::TimeBase;

// The cases, made when the tests are listed: the defaults are read from a Settings.
std::vector<Migration> migrations()
{
    return {
        Migration{.name = "SecondsAutoAtOneSecond",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "auto", "timeSpan": 1.0})",
                  .timeBase = TimeBase::kTrigger,
                  .triggerSpan = 1.0,
                  .freeSpan = defaultOf(SettingId::kFreeSpan),
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "SecondsAutoAtAQuarterSecond",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "auto", "timeSpan": 0.25})",
                  .timeBase = TimeBase::kTrigger,
                  .triggerSpan = 0.25,
                  .freeSpan = defaultOf(SettingId::kFreeSpan),
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "SecondsAutoJustAboveOneSecond",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "auto", "timeSpan": 1.0001})",
                  .timeBase = TimeBase::kFree,
                  .triggerSpan = defaultOf(SettingId::kTriggerSpan),
                  .freeSpan = 1.0001,
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "SecondsNormalAtFourSeconds",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "normal", "timeSpan": 4.0})",
                  .timeBase = TimeBase::kFree,
                  .triggerSpan = defaultOf(SettingId::kTriggerSpan),
                  .freeSpan = 4.0,
                  .triggerMode = TriggerMode::kNormal},
        Migration{.name = "SecondsFreeAtOneSecond",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "free", "timeSpan": 1.0})",
                  .timeBase = TimeBase::kFree,
                  .triggerSpan = 1.0,
                  .freeSpan = 1.0,
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "SecondsFreeBelowFreesShortest",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "free", "timeSpan": 0.05})",
                  .timeBase = TimeBase::kFree,
                  .triggerSpan = 0.05,
                  .freeSpan = protocol::kShortestFreeSpanSeconds,
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "SecondsSingleAtAQuarterSecond",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "single", "timeSpan": 0.25})",
                  .timeBase = TimeBase::kTrigger,
                  .triggerSpan = 0.25,
                  .freeSpan = defaultOf(SettingId::kFreeSpan),
                  .triggerMode = TriggerMode::kSingle},
        Migration{.name = "BeatsFreeAtTwoSeconds",
                  .saved = R"({"timeBase": "beats", "triggerMode": "free", "timeSpan": 2.0})",
                  .timeBase = TimeBase::kBeats,
                  .triggerSpan = defaultOf(SettingId::kTriggerSpan),
                  .freeSpan = 2.0,
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "BeatsAutoAtHalfASecond",
                  .saved = R"({"timeBase": "beats", "triggerMode": "auto", "timeSpan": 0.5})",
                  .timeBase = TimeBase::kBeats,
                  .triggerSpan = 0.5,
                  .freeSpan = defaultOf(SettingId::kFreeSpan),
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "BeatsAutoAtThreeSeconds",
                  .saved = R"({"timeBase": "beats", "triggerMode": "auto", "timeSpan": 3.0})",
                  .timeBase = TimeBase::kBeats,
                  .triggerSpan = defaultOf(SettingId::kTriggerSpan),
                  .freeSpan = 3.0,
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "SecondsAutoAtTheOldShortest",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "auto", "timeSpan": 0.0005})",
                  .timeBase = TimeBase::kTrigger,
                  .triggerSpan = 0.0005,
                  .freeSpan = defaultOf(SettingId::kFreeSpan),
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "SecondsAutoAtTheOldLongest",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "auto", "timeSpan": 4.0})",
                  .timeBase = TimeBase::kFree,
                  .triggerSpan = defaultOf(SettingId::kTriggerSpan),
                  .freeSpan = 4.0,
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "SecondsAutoWithAnInvalidSpan",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "auto", "timeSpan": 99})",
                  .timeBase = TimeBase::kTrigger,
                  .triggerSpan = 1.0,
                  .freeSpan = defaultOf(SettingId::kFreeSpan),
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "SecondsAutoWithASpanThatIsNotANumber",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "auto", "timeSpan": "x"})",
                  .timeBase = TimeBase::kTrigger,
                  .triggerSpan = 1.0,
                  .freeSpan = defaultOf(SettingId::kFreeSpan),
                  .triggerMode = TriggerMode::kAuto},
        Migration{.name = "SecondsFreeWithAnInvalidSpan",
                  .saved = R"({"timeBase": "seconds", "triggerMode": "free", "timeSpan": 99})",
                  .timeBase = TimeBase::kFree,
                  .triggerSpan = 1.0,
                  .freeSpan = 1.0,
                  .triggerMode = TriggerMode::kAuto},
    };
}

class MigratesEachKindOfProject : public ::testing::TestWithParam<Migration>
{
};

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const Migration& c, std::ostream* out)
{
    *out << c.name;
}

TEST_P(MigratesEachKindOfProject, OpensShowingWhatItShowed)
{
    const Migration& c = GetParam();
    const nlohmann::json state = {{"version", 2}, {"settings", nlohmann::json::parse(c.saved)}};
    const Settings settings = deserializeState(state.dump()).settings;
    EXPECT_EQ(settings.enumValue<TimeBase>(SettingId::kTimeBase), c.timeBase);
    EXPECT_EQ(settings.value(SettingId::kTriggerSpan), c.triggerSpan);
    EXPECT_EQ(settings.value(SettingId::kFreeSpan), c.freeSpan);
    EXPECT_EQ(settings.enumValue<TriggerMode>(SettingId::kTriggerMode), c.triggerMode);
}

INSTANTIATE_TEST_SUITE_P(State, MigratesEachKindOfProject, ::testing::ValuesIn(migrations()),
                         [](const ::testing::TestParamInfo<Migration>& info) {
                             return std::string(info.param.name);
                         });

// The 0.5 release's saved project, in Seconds at 0.25 s in Single, opens in Trigger at 0.25 s.
TEST(State, OpensTheReleasedSecondsProjectInTrigger)
{
    const Settings settings = deserializeState(readTestFile("state-v2-0.5.json")).settings;
    EXPECT_EQ(settings.enumValue<TimeBase>(SettingId::kTimeBase), TimeBase::kTrigger);
    EXPECT_EQ(settings.value(SettingId::kTriggerSpan), 0.25);
    EXPECT_EQ(settings.value(SettingId::kFreeSpan), defaultOf(SettingId::kFreeSpan));
}

// A project saved in the Compare display mode (0.5 and 0.8 both saved one) opens in Multi with
// Compare on, showing what it showed.
TEST(State, OpensAProjectSavedInCompareInMultiWithCompareOn)
{
    for (const char* file : {"state-v2-0.5.json", "state-v3.json"}) {
        const Settings settings = deserializeState(readTestFile(file)).settings;
        EXPECT_EQ(settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode),
                  protocol::DisplayMode::kMulti)
            << file;
        EXPECT_EQ(settings.enumValue<protocol::Toggle>(SettingId::kCompare), protocol::Toggle::kOn)
            << file;
    }
}

// Each display mode a release saved opens as the view it showed: Single and Multi as they were,
// with Compare off, and Compare as Multi with Compare on.
TEST(State, OpensEachReleasedDisplayModeAsSaved)
{
    struct Shown
    {
        protocol::DisplayMode mode{};
        protocol::Toggle compare{};
    };
    const auto shown = [](std::string_view name) {
        if (name == "compare") {
            return Shown{.mode = protocol::DisplayMode::kMulti, .compare = protocol::Toggle::kOn};
        }
        const auto index = std::ranges::find(protocol::kDisplayModeNames, name) -
                           protocol::kDisplayModeNames.begin();
        return Shown{.mode = static_cast<protocol::DisplayMode>(index),
                     .compare = protocol::Toggle::kOff};
    };
    for (const GoldenValueNames& golden : kGoldenValueNames) {
        const auto released = nlohmann::json::parse(readTestFile(golden.file));
        for (const auto& name : released.at("displayMode")) {
            const nlohmann::json state = {{"version", golden.version},
                                          {"settings", {{"displayMode", name}}}};
            const Settings settings = deserializeState(state.dump()).settings;
            const Shown expected = shown(name.get_ref<const std::string&>());
            EXPECT_EQ(settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode),
                      expected.mode)
                << golden.file << ": " << name;
            EXPECT_EQ(settings.enumValue<protocol::Toggle>(SettingId::kCompare), expected.compare)
                << golden.file << ": " << name;
        }
    }
}

// Compare keeps its state while the display mode is Single, so a project saved so shows Compare
// again when it goes back to Multi.
TEST(State, KeepsCompareOnInSingle)
{
    Settings saved;
    saved.set(SettingId::kCompare, asValue(protocol::Toggle::kOn));
    const Settings settings = deserializeState(serializeState(saved)).settings;
    EXPECT_EQ(settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode),
              protocol::DisplayMode::kSingle);
    EXPECT_EQ(settings.enumValue<protocol::Toggle>(SettingId::kCompare), protocol::Toggle::kOn);
}

// A display mode of the wrong type, or a version 4 state without Compare, loads the defaults for
// both: nothing throws, and Compare is off.
TEST(State, LoadsCompareOffWhenTheStateDoesNotSayOtherwise)
{
    for (const char* text : {R"({"version": 3, "settings": {"displayMode": 2}})",
                             R"({"version": 3, "settings": {"displayMode": ["compare"]}})",
                             R"({"version": 4, "settings": {"displayMode": "multi"}})"}) {
        const Settings settings = deserializeState(text).settings;
        EXPECT_EQ(settings.enumValue<protocol::Toggle>(SettingId::kCompare), protocol::Toggle::kOff)
            << text;
    }
    EXPECT_EQ(deserializeState(R"({"version": 4, "settings": {"displayMode": "multi"}})")
                  .settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode),
              protocol::DisplayMode::kMulti);
}

// A version 3 state holds no time span; one that does ignores it, as any unknown field.
TEST(State, IgnoresATimeSpanInAVersion3State)
{
    const Settings settings =
        deserializeState(R"({"version": 3, "settings": {"timeBase": "trigger", "timeSpan": 0.25}})")
            .settings;
    EXPECT_EQ(settings.value(SettingId::kTriggerSpan), defaultOf(SettingId::kTriggerSpan));
}

TEST(State, IgnoresUnknownFieldsAndLoadsNewerVersions)
{
    const Settings settings = deserializeState(R"({
        "version": 7,
        "settings": {"triggerLevel": 0.5, "futureSetting": true},
        "futureSection": {}
    })")
                                  .settings;
    EXPECT_EQ(settings.value(SettingId::kTriggerLevel), 0.5);
}

// The editor's size the user chose reopens with the project, beside the settings.
TEST(State, ReopensAtTheEditorSizeSaved)
{
    Settings settings;
    settings.set(SettingId::kTriggerLevel, 0.5);
    const SavedState saved =
        deserializeState(serializeState(settings, EditorSize{.width = 1234.5, .height = 678.25}));
    EXPECT_EQ(saved.settings, settings);
    EXPECT_EQ(saved.editorSize, (EditorSize{.width = 1234.5, .height = 678.25}));

    const SavedState golden = deserializeState(readTestFile("state-v5-editor-size.json"));
    EXPECT_EQ(golden.editorSize, (EditorSize{.width = 1200, .height = 500}));
}

// A project saved before the size was saved, or by an instance whose size was never changed,
// opens at the default size.
TEST(State, AStateWithoutAnEditorSizeOpensAtTheDefault)
{
    for (const char* file : kEveryGoldenStateFile) {
        const std::string text = readTestFile(file);
        if (nlohmann::json::parse(text).contains("editor")) {
            continue;
        }
        EXPECT_FALSE(deserializeState(text).editorSize) << file;
    }
    EXPECT_FALSE(deserializeState(serializeState(Settings())).editorSize);
    EXPECT_FALSE(nlohmann::json::parse(serializeState(Settings())).contains("editor"));
}

// An editor size that is not one opens at the default, and never costs a setting.
TEST(State, AnInvalidEditorSizeOpensAtTheDefaultAndKeepsTheSettings)
{
    for (const char* editor :
         {R"("big")", "null", "[1200, 500]", R"({"width": 1200})", R"({"height": 500})",
          R"({"width": "1200", "height": 500})", R"({"width": 1200, "height": null})"}) {
        const SavedState saved = deserializeState(
            std::string(R"({"version": 5, "settings": {"triggerLevel": 0.5}, "editor": )") +
            editor + "}");
        EXPECT_FALSE(saved.editorSize) << editor;
        EXPECT_EQ(saved.settings.value(SettingId::kTriggerLevel), 0.5) << editor;
    }
}

// A size out of the editor's range is still a size: the editor raises or lowers it as it opens.
TEST(State, KeepsAnEditorSizeOutOfRangeForTheEditorToBound)
{
    const SavedState saved = deserializeState(
        R"({"version": 5, "settings": {}, "editor": {"width": -5, "height": 1e9}})");
    EXPECT_EQ(saved.editorSize, (EditorSize{.width = -5, .height = 1e9}));
}

TEST(State, FallsBackToDefaultsForUnreadableInput)
{
    for (const char* input : {"", "not json", "[1, 2]", R"({"version": 1})",
                              R"({"settings": "nope"})", R"({"settings": {"timeSpan": 0.5)"}) {
        EXPECT_EQ(deserializeState(input).settings, Settings()) << input;
    }
}

} // namespace
} // namespace orizon::engine
