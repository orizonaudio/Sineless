#pragma once

namespace orizon::editor {

class PointerControl;
class WebView;

// For tests: ends the view's page process, as a crash or macOS under memory pressure would. False
// if WebKit offers no way to.
bool terminateContentProcessForTests(WebView& view);

// For tests: the NSCursor the view shows for the page, as the page last reported it.
const void* cursorForTests(WebView& view);

// For tests: the NSCursor the view shows for a CSS cursor (nil when left to WebKit), and the
// diagonal resize cursor drawn for macOS before 15.
const void* cursorForCssForTests(const char* css);
const void* drawnDiagonalResizeCursorForTests();

// For tests: the pointer that views created from now on hold, instead of the real one (nullptr).
void setPointerControlForTests(PointerControl* control);

// For tests: the CFBundlePackageType that views created from now on take their process's main
// bundle to have, as Logic's out-of-process Audio Unit service has "XPC!" (nullptr: the real one).
void setBundleTypeForTests(const char* type);

// For tests: whether a process whose main bundle has this CFBundlePackageType holds the pointer.
bool holdsPointerInBundleOfTypeForTests(const char* type);

// For tests: the check for a release no mouse-up reported, as the view's timer runs it.
void pollPointerHoldForTests(WebView& view);

} // namespace orizon::editor
