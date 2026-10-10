#pragma once

#include <string>

namespace orizon::editor {

// For tests: the folder WebView2 looks for its browser in, instead of the installed runtime; empty
// for the installed one. It is passed as the browserExecutableFolder argument, which WebView2
// honours in elevated processes too, where it ignores its WEBVIEW2_* environment variables.
void setBrowserFolderForTests(std::wstring folder);

} // namespace orizon::editor
