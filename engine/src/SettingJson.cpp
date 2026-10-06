#include "engine/SettingJson.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace orizon::engine {

using protocol::SettingId;
using protocol::SettingKind;

namespace {

const protocol::SettingDescriptor& descriptor(SettingId id)
{
    return protocol::kSettings.at(static_cast<std::size_t>(id));
}

std::optional<std::size_t> indexOfName(const protocol::SettingDescriptor& setting,
                                       const nlohmann::json& json)
{
    if (!json.is_string()) {
        return std::nullopt;
    }
    const auto& name = json.get_ref<const std::string&>();
    const auto found = std::ranges::find(setting.valueNames, name);
    if (found == setting.valueNames.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - setting.valueNames.begin());
}

} // namespace

nlohmann::json settingToJson(SettingId id, double value)
{
    const auto& setting = descriptor(id);
    switch (setting.kind) {
    case SettingKind::kNumber:
        return value;
    case SettingKind::kEnum:
        return std::string(setting.valueNames[static_cast<std::size_t>(value)]);
    case SettingKind::kFlags: {
        auto names = nlohmann::json::array();
        const auto mask = static_cast<std::uint32_t>(value);
        for (std::size_t bit = 0; bit < setting.valueNames.size(); ++bit) {
            if ((mask & (1U << bit)) != 0) {
                names.push_back(std::string(setting.valueNames[bit]));
            }
        }
        return names;
    }
    }
    return nullptr;
}

std::optional<double> settingFromJson(SettingId id, const nlohmann::json& json)
{
    const auto& setting = descriptor(id);
    switch (setting.kind) {
    case SettingKind::kNumber:
        if (!json.is_number()) {
            return std::nullopt;
        }
        return json.get<double>();
    case SettingKind::kEnum:
        if (const auto index = indexOfName(setting, json)) {
            return static_cast<double>(*index);
        }
        return std::nullopt;
    case SettingKind::kFlags: {
        if (!json.is_array()) {
            return std::nullopt;
        }
        std::uint32_t mask = 0;
        for (const auto& element : json) {
            const auto index = indexOfName(setting, element);
            if (!index) {
                return std::nullopt;
            }
            mask |= 1U << *index;
        }
        return static_cast<double>(mask);
    }
    }
    return std::nullopt;
}

} // namespace orizon::engine
