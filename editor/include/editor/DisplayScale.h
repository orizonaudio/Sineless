#pragma once

namespace orizon::editor {

// How many of the host's window units make one CSS pixel of the page. Hosts size the editor in
// their window units: points on macOS, so always 1; on Windows, physical pixels in a DPI-aware
// window (1.5 at 150 % display scaling) or 96-dpi units in a DPI-unaware one, which Windows
// stretches (1). `parent` is the host's window; null asks the calling thread's DPI context,
// for the size the host asks for before it attaches the editor.
[[nodiscard]] double displayScale(void* parent);

} // namespace orizon::editor
