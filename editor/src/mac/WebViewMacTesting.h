#pragma once

namespace orizon::editor {

class WebView;

// For tests: ends the view's page process, as a crash or macOS under memory pressure would. False
// if WebKit offers no way to.
bool terminateContentProcessForTests(WebView& view);

// For tests: the NSCursor the view shows for the page, as the page last reported it.
const void* cursorForTests(WebView& view);

} // namespace orizon::editor
