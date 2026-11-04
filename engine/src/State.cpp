#include "engine/State.h"

#include "engine/SettingJson.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>

namespace orizon::engine {

namespace {

using Migration = void (*)(nlohmann::json& settings);

// Version 2 made Beats the default time base. A version 1 state without one predates tempo sync,
// so it keeps the seconds time base it was made with.
void migrateToVersion2(nlohmann::json& settings)
{
    if (!settings.contains("timeBase")) {
        settings["timeBase"] = "seconds";
    }
}

// Version 3 made Free a time base beside Beats and Trigger, each of Trigger and Free with a span of
// its own. A Seconds project opens showing what it showed: in Free where its window ran free (the
// Free trigger mode, or a span above 1 s), else in Trigger. Its span goes to the time base whose
// range holds it, and to Free's too, clamped to Free's range, where the window ran free. A span
// that is missing or was invalid counts as the 1 s the old engine fell back to. The Free trigger
// mode becomes Auto, the default. Fields of the wrong type are read as missing, so nothing throws.
void migrateToVersion3(nlohmann::json& settings)
{
    const auto stringAt = [&](const char* key) -> std::string {
        const auto found = settings.find(key);
        return found != settings.end() && found->is_string() ? found->get<std::string>()
                                                             : std::string();
    };
    constexpr double kOldShortest = 0.0005;
    constexpr double kOldLongest = 4.0;
    constexpr double kOldDefault = 1.0;
    double span = kOldDefault;
    if (const auto found = settings.find("timeSpan");
        found != settings.end() && found->is_number()) {
        const double value = found->get<double>();
        if (value >= kOldShortest && value <= kOldLongest) {
            span = value;
        }
    }
    const bool freeMode = stringAt("triggerMode") == "free";
    const bool ranFree = freeMode || span > protocol::kLongestTriggerSpanSeconds;
    // A span the state already holds stays: it was saved by version 3, under another version.
    if (span <= protocol::kLongestTriggerSpanSeconds && !settings.contains("triggerSpan")) {
        settings["triggerSpan"] = span;
    }
    if (ranFree && !settings.contains("freeSpan")) {
        settings["freeSpan"] = std::clamp(span, protocol::kShortestFreeSpanSeconds, kOldLongest);
    }
    if (stringAt("timeBase") == "seconds") {
        settings["timeBase"] = ranFree ? "free" : "trigger";
    }
    if (freeMode) {
        settings["triggerMode"] = "auto";
    }
    settings.erase("timeSpan");
}

// kMigrations[n - 1] upgrades the settings object of state version n to version n + 1.
// Add an entry, and a golden state file, whenever kStateVersion is raised. A release that adds
// settings or values without raising it gets a golden state file too (state-v2-0.5.json).
constexpr std::array<Migration, kStateVersion - 1> kMigrations{&migrateToVersion2,
                                                               &migrateToVersion3};

} // namespace

std::string serializeState(const Settings& settings)
{
    nlohmann::json values = nlohmann::json::object();
    for (const auto& setting : protocol::kSettings) {
        values[std::string(setting.name)] = settingToJson(setting.id, settings.value(setting.id));
    }
    const nlohmann::json state = {{"version", kStateVersion}, {"settings", values}};
    return state.dump(2);
}

Settings deserializeState(std::string_view text) noexcept
{
    Settings settings;
    try {
        nlohmann::json state = nlohmann::json::parse(text, nullptr, false);
        if (!state.is_object() || !state.contains("settings") || !state["settings"].is_object()) {
            return settings;
        }
        int version = 1;
        if (state.contains("version") && state["version"].is_number_integer()) {
            version = state["version"].get<int>();
        }
        nlohmann::json& values = state["settings"];
        for (int from = std::max(version, 1); from < kStateVersion; ++from) {
            kMigrations.at(static_cast<std::size_t>(from - 1))(values);
        }
        for (const auto& setting : protocol::kSettings) {
            const auto found = values.find(std::string(setting.name));
            if (found == values.end()) {
                continue;
            }
            if (const auto value = settingFromJson(setting.id, *found)) {
                settings.set(setting.id, *value);
            }
        }
    } catch (...) {
        return Settings{};
    }
    return settings;
}

} // namespace orizon::engine
