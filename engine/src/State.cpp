#include "engine/State.h"

#include "engine/SettingJson.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>

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

// Version 4 made Compare a switch in Multi rather than a display mode: a project saved in Compare
// opens in Multi with Compare on. Others leave it off, its default.
void migrateToVersion4(nlohmann::json& settings)
{
    const auto found = settings.find("displayMode");
    if (found != settings.end() && found->is_string() && found->get<std::string>() == "compare") {
        settings["displayMode"] = "multi";
        settings["compare"] = "on";
    }
}

// Version 5 counts every beat span in bars, from 1/8 bar to 16 bars; the spans in beats went. Each
// opens as the bar span it equals in 4/4, those shorter than 1/8 bar as 1/8 bar.
void migrateToVersion5(nlohmann::json& settings)
{
    const auto found = settings.find("beatSpan");
    if (found == settings.end() || !found->is_string()) {
        return;
    }
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kBeatSpans{{
        {"sixteenthBeat", "eighthBar"},
        {"eighthBeat", "eighthBar"},
        {"quarterBeat", "eighthBar"},
        {"halfBeat", "eighthBar"},
        {"oneBeat", "quarterBar"},
        {"twoBeats", "halfBar"},
    }};
    const std::string saved = found->get<std::string>();
    for (const auto& [from, to] : kBeatSpans) {
        if (saved == from) {
            settings["beatSpan"] = std::string(to);
        }
    }
}

// kMigrations[n - 1] upgrades the settings object of state version n to version n + 1.
// Add an entry, and a golden state file, whenever kStateVersion is raised. A release that adds
// settings or values without raising it gets a golden state file too (state-v2-0.5.json).
constexpr std::array<Migration, kStateVersion - 1> kMigrations{
    &migrateToVersion2, &migrateToVersion3, &migrateToVersion4, &migrateToVersion5};

// The editor's size in a state, or none: both a width and a height that are numbers (JSON's are
// finite: a number too large for a double makes the whole state unreadable). Their range is the
// editor's to check, where its minimum and largest sizes live.
std::optional<EditorSize> editorSizeIn(const nlohmann::json& state)
{
    const auto editor = state.find("editor");
    if (editor == state.end() || !editor->is_object()) {
        return std::nullopt;
    }
    const auto width = editor->find("width");
    const auto height = editor->find("height");
    if (width == editor->end() || height == editor->end() || !width->is_number() ||
        !height->is_number()) {
        return std::nullopt;
    }
    return EditorSize{.width = width->get<double>(), .height = height->get<double>()};
}

} // namespace

std::string serializeState(const Settings& settings, std::optional<EditorSize> editorSize)
{
    nlohmann::json values = nlohmann::json::object();
    for (const auto& setting : protocol::kSettings) {
        values[std::string(setting.name)] = settingToJson(setting.id, settings.value(setting.id));
    }
    nlohmann::json state = {{"version", kStateVersion}, {"settings", values}};
    if (editorSize) {
        state["editor"] = {{"width", editorSize->width}, {"height", editorSize->height}};
    }
    return state.dump(2);
}

SavedState deserializeState(std::string_view text) noexcept
{
    SavedState saved;
    Settings& settings = saved.settings;
    try {
        nlohmann::json state = nlohmann::json::parse(text, nullptr, false);
        if (!state.is_object() || !state.contains("settings") || !state["settings"].is_object()) {
            return saved;
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
        saved.editorSize = editorSizeIn(state);
    } catch (...) {
        return SavedState{};
    }
    return saved;
}

} // namespace orizon::engine
