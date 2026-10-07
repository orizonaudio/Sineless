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

// kMigrations[n - 1] upgrades the settings object of state version n to version n + 1.
// Add an entry, and a golden state file, whenever kStateVersion is raised.
constexpr std::array<Migration, kStateVersion - 1> kMigrations{&migrateToVersion2};

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
