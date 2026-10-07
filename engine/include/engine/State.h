#pragma once

#include "engine/Settings.h"

#include <string>
#include <string_view>

namespace orizon::engine {

// Saved plugin state: {"version": n, "settings": {...}}.
inline constexpr int kStateVersion = 2;

[[nodiscard]] std::string serializeState(const Settings& settings);

// Tolerant load: migrates older versions, keeps the default for any missing or invalid field,
// ignores unknown fields, and returns all defaults for unreadable input. Never throws.
[[nodiscard]] Settings deserializeState(std::string_view text) noexcept;

} // namespace orizon::engine
