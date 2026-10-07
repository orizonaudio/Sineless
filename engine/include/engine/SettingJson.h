#pragma once

#include "protocol/Protocol.h"

#include <nlohmann/json.hpp>
#include <optional>

namespace orizon::engine {

// JSON form of a setting value, shared by saved state and control messages: numbers as
// numbers, enum values by name, flags as an array of names in value order.
[[nodiscard]] nlohmann::json settingToJson(protocol::SettingId id, double value);

// The inverse of settingToJson. Returns nothing if the JSON has the wrong shape or names an
// unknown value; range checks are left to Settings::isValid.
[[nodiscard]] std::optional<double> settingFromJson(protocol::SettingId id,
                                                    const nlohmann::json& json);

} // namespace orizon::engine
