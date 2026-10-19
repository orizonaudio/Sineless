// Fault injection for saved state: what a host hands back after a failed, short or corrupted
// save. deserializeState must never throw or crash, and must keep the default for whatever it
// cannot read (see State.h).

#include "engine/State.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace orizon::engine {
namespace {

using protocol::SettingKind;

std::string readTestFile(const std::string& name)
{
    std::ifstream file(std::string(ORIZON_TEST_DATA_DIR) + "/" + name);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

// A valid value other than the default for every setting, so that a field that fell back to its
// default is told apart from one that loaded.
Settings everyValueChanged()
{
    Settings settings;
    for (const protocol::SettingDescriptor& setting : protocol::kSettings) {
        const double value =
            setting.maximum != setting.defaultValue ? setting.maximum : setting.minimum;
        EXPECT_TRUE(settings.set(setting.id, value)) << setting.name;
        EXPECT_NE(value, setting.defaultValue) << setting.name;
    }
    return settings;
}

// The saved state with one setting's JSON value replaced by `token`, written as raw text so that
// tokens nlohmann cannot hold (1e400, NaN) reach the parser as a host would hand them over.
constexpr std::string_view kPlaceholder = "@placeholder@";

std::string withPlaceholderReplaced(const nlohmann::json& state, const std::string& token)
{
    std::string text = state.dump(2);
    const std::string quoted = "\"" + std::string(kPlaceholder) + "\"";
    text.replace(text.find(quoted), quoted.size(), token);
    return text;
}

std::string stateWithRawValue(std::string_view name, const std::string& token)
{
    nlohmann::json state = nlohmann::json::parse(serializeState(everyValueChanged()));
    state["settings"][std::string(name)] = kPlaceholder;
    return withPlaceholderReplaced(state, token);
}

// The same for the version field.
std::string stateWithRawVersion(const std::string& token)
{
    nlohmann::json state = nlohmann::json::parse(serializeState(everyValueChanged()));
    state["version"] = kPlaceholder;
    return withPlaceholderReplaced(state, token);
}

// Every setting except `id` loaded its changed value, and `id` kept its default.
void expectOnlyDefaulted(const Settings& loaded, protocol::SettingId id)
{
    const Settings changed = everyValueChanged();
    const Settings defaults;
    for (const protocol::SettingDescriptor& setting : protocol::kSettings) {
        const double expected =
            setting.id == id ? defaults.value(setting.id) : changed.value(setting.id);
        EXPECT_EQ(loaded.value(setting.id), expected) << setting.name;
    }
}

// Saved states to damage: each released version's golden file, and what this build saves.
struct StateSource
{
    const char* name = "";
    std::string text;
};

std::vector<StateSource> stateSources()
{
    return {{.name = "Version1File", .text = readTestFile("state-v1.json")},
            {.name = "Version2File", .text = readTestFile("state-v2.json")},
            {.name = "Saved", .text = serializeState(everyValueChanged())}};
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const StateSource& source, std::ostream* out)
{
    *out << source.name;
}

class DamagedState : public ::testing::TestWithParam<StateSource>
{
};

// A save cut short at any byte (a failed or short write, a read that stopped early) is
// unreadable as a whole: every setting keeps its default rather than a mix of old and new.
TEST_P(DamagedState, TruncatedAtEveryByteKeepsEveryDefault)
{
    const std::string& text = GetParam().text;
    ASSERT_FALSE(text.empty());
    const Settings full = deserializeState(text);
    ASSERT_NE(full, Settings()) << "the source must differ from the defaults to prove anything";
    // Only trailing whitespace can go without losing the closing brace.
    const std::size_t complete = text.find_last_not_of(" \t\r\n") + 1;
    for (std::size_t length = 0; length < text.size(); ++length) {
        const Settings loaded = deserializeState(std::string_view(text).substr(0, length));
        if (length < complete) {
            EXPECT_EQ(loaded, Settings()) << "cut at byte " << length;
        } else {
            EXPECT_EQ(loaded, full) << "cut at byte " << length << ", in trailing whitespace";
        }
    }
}

// One damaged byte anywhere, by a character that breaks any number or name it lands in, never
// invents a value: each setting either loads as saved or keeps its default. This does not check
// which settings the damage costs; a load that falls back to every default also passes.
TEST_P(DamagedState, ADamagedByteNeverInventsAValue)
{
    const std::string& text = GetParam().text;
    const Settings full = deserializeState(text);
    const Settings defaults;
    for (const char damage : {'x', '\0'}) {
        for (std::size_t offset = 0; offset < text.size(); ++offset) {
            std::string damaged = text;
            damaged[offset] = damage;
            const Settings loaded = deserializeState(damaged);
            for (const protocol::SettingDescriptor& setting : protocol::kSettings) {
                const double value = loaded.value(setting.id);
                EXPECT_TRUE(value == full.value(setting.id) || value == defaults.value(setting.id))
                    << setting.name << " = " << value << " with byte " << offset << " set to "
                    << static_cast<int>(damage);
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(StateFaults, DamagedState, ::testing::ValuesIn(stateSources()),
                         [](const ::testing::TestParamInfo<StateSource>& info) {
                             return std::string(info.param.name);
                         });

// Values of the wrong JSON type for a kind of setting, or of the right type naming nothing.
std::vector<std::string> wrongValuesFor(SettingKind kind)
{
    std::vector<std::string> values = {"null", "true", "false", "{}", R"({"value": 1})"};
    switch (kind) {
    case SettingKind::kNumber:
        values.insert(values.end(), {R"("0.5")", R"("")", "[]", "[0.5]"});
        break;
    case SettingKind::kEnum:
        values.insert(values.end(),
                      {"0", "1", "0.5", R"("")", R"("no such value")", "[]", "[0]", R"(["x"])"});
        break;
    case SettingKind::kFlags:
        values.insert(values.end(), {"0", "1", R"("")", R"("left")", "[]", "[0]", "[null]",
                                     R"([["left"]])", R"(["left", 1])", R"(["no such view"])"});
        break;
    }
    return values;
}

// Numbers no setting can hold: at the ends of double's range, beyond 64-bit integers, and NaN
// and infinities written as strings.
constexpr std::array<std::string_view, 8> kUnusableNumbers = {"1e308",
                                                              "-1e308",
                                                              "18446744073709551615",
                                                              "18446744073709551616",
                                                              "-9223372036854775809",
                                                              R"("NaN")",
                                                              R"("Infinity")",
                                                              R"("-Infinity")"};

// Numbers that are not JSON (NaN, infinities) or that do not parse (beyond double's range).
constexpr std::array<std::string_view, 9> kNonJsonNumbers = {
    "NaN", "nan", "-nan", "Infinity", "-Infinity", "inf", "1e400", "-1e400", "1e999999999"};

struct FieldCase
{
    const protocol::SettingDescriptor* setting = nullptr;
    std::string token;
};

std::vector<FieldCase> badFieldCases()
{
    std::vector<FieldCase> cases;
    for (const protocol::SettingDescriptor& setting : protocol::kSettings) {
        for (const std::string& token : wrongValuesFor(setting.kind)) {
            cases.push_back({.setting = &setting, .token = token});
        }
        if (setting.kind == SettingKind::kNumber) {
            for (const std::string_view token : kUnusableNumbers) {
                cases.push_back({.setting = &setting, .token = std::string(token)});
            }
        }
    }
    return cases;
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const FieldCase& c, std::ostream* out)
{
    *out << c.setting->name << ": " << c.token;
}

class BadField : public ::testing::TestWithParam<FieldCase>
{
};

TEST_P(BadField, KeepsTheDefaultForThatSettingOnly)
{
    const auto& [setting, token] = GetParam();
    expectOnlyDefaulted(deserializeState(stateWithRawValue(setting->name, token)), setting->id);
}

INSTANTIATE_TEST_SUITE_P(StateFaults, BadField, ::testing::ValuesIn(badFieldCases()),
                         [](const ::testing::TestParamInfo<FieldCase>& info) {
                             return std::string(info.param.setting->name) + "_" +
                                    std::to_string(info.index);
                         });

// A state holding a number that is not JSON, in any field or as its version, is unreadable as a
// whole.
TEST(StateFaults, NonJsonNumbersMakeTheWholeStateUnreadable)
{
    for (const std::string_view number : kNonJsonNumbers) {
        const std::string token(number);
        for (const protocol::SettingDescriptor& setting : protocol::kSettings) {
            EXPECT_EQ(deserializeState(stateWithRawValue(setting.name, token)), Settings())
                << setting.name << ": " << token;
        }
        EXPECT_EQ(deserializeState(stateWithRawVersion(token)), Settings()) << "version " << token;
    }
}

// Whatever the version field holds, the settings it carries load: every one is present, so no
// migration has anything to fill in.
TEST(StateFaults, ReadsTheSettingsWhateverTheVersionSays)
{
    for (const char* version : {"null", "true", R"("2")", "[]", "{}", "2.0", "2.5", "-1", "0", "3",
                                "2147483647", "2147483648", "4294967298", "9223372036854775807",
                                "18446744073709551615", "-9223372036854775809", "1e308"}) {
        EXPECT_EQ(deserializeState(stateWithRawVersion(version)), everyValueChanged())
            << "version " << version;
    }
}

TEST(StateFaults, FallsBackToDefaultsForEmptyAndUndecodableInput)
{
    using namespace std::string_view_literals;
    for (const std::string_view input :
         {""sv, " "sv, "\n\n"sv, "\0"sv, "\xEF\xBB\xBF"sv, "null"sv, "0"sv, R"("")"sv, "{"sv, "}"sv,
          R"({"settings": null})"sv, R"({"settings": []})"sv,
          R"({"version": 2, "settings": {"viewLayout": "overlaid"}} trailing)"sv,
          "{\"version\": 2, \"settings\": {\"viewLayout\": \"\xFF\"}}"sv,
          R"({"version": 2, "settings": {"viewLayout": "\ud800"}})"sv}) {
        EXPECT_EQ(deserializeState(input), Settings()) << testing::PrintToString(input);
    }
    // The parser stops at a NUL, as at the end of a C string: a host that hands back the text with
    // its terminator loses nothing.
    const Settings terminated =
        deserializeState("{\"version\": 2, \"settings\": {\"viewLayout\": \"overlaid\"}}\0"sv);
    EXPECT_EQ(terminated.enumValue<protocol::ViewLayout>(protocol::SettingId::kViewLayout),
              protocol::ViewLayout::kOverlaid);
}

// A state nested far deeper than any we write, in a field and around the whole state.
TEST(StateFaults, SurvivesDeepNesting)
{
    constexpr std::size_t kDepth = 100000;
    const std::string open(kDepth, '[');
    const std::string close(kDepth, ']');
    const Settings field = deserializeState(R"({"version": 2, "settings": {"channelViews": )" +
                                            open + close + R"(, "triggerLevel": 0.5}})");
    EXPECT_EQ(field.value(protocol::SettingId::kChannelViews),
              Settings().value(protocol::SettingId::kChannelViews));
    EXPECT_EQ(field.value(protocol::SettingId::kTriggerLevel), 0.5);
    EXPECT_EQ(deserializeState(open + serializeState(everyValueChanged()) + close), Settings());
    EXPECT_EQ(deserializeState(open), Settings()) << "unclosed";
}

} // namespace
} // namespace orizon::engine
