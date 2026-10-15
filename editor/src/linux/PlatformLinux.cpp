// Linux has no editor: Sineless ships for macOS and Windows only, and is built on Linux for the
// checks alone (the sanitizers over the plugin and the editor's shared code). The editor says it
// supports no Linux window type, so no host opens it; these are what it would call there.

#include "editor/DisplayScale.h"
#include "editor/UiDispatcher.h"
#include "editor/WebView.h"

namespace orizon::editor {

double displayScale(void* /*parent*/)
{
    return 1.0;
}

std::unique_ptr<UiDispatcher> UiDispatcher::create()
{
    return nullptr;
}

// The parameters are WebView::create's, by value as on macOS and Windows, where the view keeps
// them.
// NOLINTBEGIN(performance-unnecessary-value-param)
std::unique_ptr<WebView> WebView::create(void* /*parent*/, int /*width*/, int /*height*/,
                                         MessageHandler /*onMessage*/,
                                         FrameRequestHandler /*onFrameRequest*/)
{
    return nullptr;
}
// NOLINTEND(performance-unnecessary-value-param)

} // namespace orizon::editor
