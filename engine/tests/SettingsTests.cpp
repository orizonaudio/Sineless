#include "engine/SettingJson.h"
#include "engine/Settings.h"

#include <cmath>
#include <gtest/gtest.h>
#include <limits>

namespace orizon::engine {
namespace {

using protocol::ChannelView;
using protocol::SettingId;
using protocol::TriggerMode;

TEST(Settings, StartWithTheSchemaDefaults)
{
    const Settings settings;
    for (const auto& setting : protocol::kSettings) {
        EXPECT_EQ(settings.value(setting.id), setting.defaultValue) << setting.name;
    }
    EXPECT_EQ(settings.enumValue<TriggerMode>(SettingId::kTriggerMode), TriggerMode::kAuto);
}

TEST(Settings, AcceptValidValues)
{
    Settings settings;
    EXPECT_TRUE(settings.set(SettingId::kTimeSpan, 4.0));
    EXPECT_TRUE(settings.set(SettingId::kTriggerMode, 3.0));
    EXPECT_TRUE(settings.set(SettingId::kChannelViews, 0b1000));
    EXPECT_EQ(settings.value(SettingId::kTimeSpan), 4.0);
    EXPECT_EQ(settings.enumValue<TriggerMode>(SettingId::kTriggerMode), TriggerMode::kSingle);
}

TEST(Settings, RejectInvalidValuesAndKeepThePreviousOne)
{
    Settings settings;
    const double before = settings.value(SettingId::kTimeSpan);
    EXPECT_FALSE(settings.set(SettingId::kTimeSpan, 4.5)) << "above maximum";
    EXPECT_FALSE(settings.set(SettingId::kTimeSpan, 0.0001)) << "below minimum";
    EXPECT_FALSE(settings.set(SettingId::kTimeSpan, std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(settings.set(SettingId::kTimeSpan, std::numeric_limits<double>::infinity()));
    EXPECT_EQ(settings.value(SettingId::kTimeSpan), before);

    EXPECT_FALSE(settings.set(SettingId::kTriggerMode, 4.0)) << "no such mode";
    EXPECT_FALSE(settings.set(SettingId::kTriggerMode, 1.5)) << "not a whole number";
    EXPECT_FALSE(settings.set(SettingId::kChannelViews, 0.0)) << "empty set";
    EXPECT_FALSE(settings.set(SettingId::kChannelViews, 16.0)) << "unknown view";
}

TEST(SettingJson, UsesNamesForEnumsAndFlags)
{
    EXPECT_EQ(settingToJson(SettingId::kTriggerMode, 1.0), "normal");
    EXPECT_EQ(settingToJson(SettingId::kChannelViews, 0b0101),
              nlohmann::json::array({"left", "mid"}));
    EXPECT_EQ(settingToJson(SettingId::kTriggerLevel, -0.5), -0.5);
}

TEST(SettingJson, RoundTripsEverySetting)
{
    const Settings settings;
    for (const auto& setting : protocol::kSettings) {
        const auto json = settingToJson(setting.id, settings.value(setting.id));
        EXPECT_EQ(settingFromJson(setting.id, json), settings.value(setting.id)) << setting.name;
    }
}

TEST(SettingJson, RejectsWrongShapesAndUnknownNames)
{
    EXPECT_FALSE(settingFromJson(SettingId::kTriggerMode, "sometimes"));
    EXPECT_FALSE(settingFromJson(SettingId::kTriggerMode, 1));
    EXPECT_FALSE(settingFromJson(SettingId::kChannelViews, "left"));
    EXPECT_FALSE(settingFromJson(SettingId::kChannelViews, nlohmann::json::array({"left", 2})));
    EXPECT_FALSE(settingFromJson(SettingId::kTimeSpan, "0.02"));
}

TEST(SettingsStore, AppliesValidChangesOnly)
{
    SettingsStore store;
    EXPECT_TRUE(store.set(SettingId::kTriggerLevel, 0.5));
    EXPECT_FALSE(store.set(SettingId::kTriggerLevel, 2.0));
    EXPECT_EQ(store.get().value(SettingId::kTriggerLevel), 0.5);

    Settings replacement;
    replacement.set(SettingId::kViewLayout, 1.0);
    store.replace(replacement);
    EXPECT_EQ(store.get(), replacement);
}

} // namespace
} // namespace orizon::engine
