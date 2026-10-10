#pragma once

#include <string>

namespace orizon::editor {

class WebView;

// For tests: the folder WebView2 looks for its browser in, instead of the installed runtime; empty
// for the installed one. It is passed as the browserExecutableFolder argument, which WebView2
// honours in elevated processes too, where it ignores its WEBVIEW2_* environment variables.
void setBrowserFolderForTests(std::wstring folder);

// For tests: ends the view's renderer process, or its whole browser, as a crash would. False if
// the view has not started or the process could not be found.
bool terminateProcessForTests(WebView& view, bool browser);

} // namespace orizon::editor
