#pragma once

#include "engine/Settings.h"

#include <optional>
#include <string>
#include <string_view>

namespace orizon::engine {

// Saved plugin state: {"version": n, "settings": {...}, "editor": {"width", "height"}}.
inline constexpr int kStateVersion = 5;

// The editor's size as the user chose it, in CSS pixels. Saved as read; the editor checks it
// against its minimum and largest sizes when it opens.
struct EditorSize
{
    double width = 0.0;
    double height = 0.0;

    bool operator==(const EditorSize&) const = default;
};

struct SavedState
{
    Settings settings;
    std::optional<EditorSize> editorSize; // None: the editor opens at its default.
};

// `editorSize` is written beside the settings when there is one.
[[nodiscard]] std::string serializeState(const Settings& settings,
                                         std::optional<EditorSize> editorSize = std::nullopt);

// Tolerant load: migrates older versions, keeps the default for any missing or invalid field,
// ignores unknown fields, and returns all defaults for unreadable input. An editor size whose
// width and height are not both numbers is none. Never throws.
[[nodiscard]] SavedState deserializeState(std::string_view text) noexcept;

} // namespace orizon::engine
