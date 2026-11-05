#pragma once

#include <string_view>

namespace orizon::editor {

// UI thread. Hands an https URL to the system, which opens it in the user's default browser, never
// in the editor; returns once the system has it. A failure is not reported: there is nothing a user
// could do about it, and the press then shows nothing. Implemented beside each platform's web view.
void openInBrowser(std::string_view url);

} // namespace orizon::editor
