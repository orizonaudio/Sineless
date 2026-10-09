#include "editor/WebView.h"

namespace orizon::editor {

// The Windows webview (WebView2) is not built yet. The editor accepts no Windows window type, so
// hosts never ask for one; this keeps the module complete on Windows meanwhile.
std::unique_ptr<WebView> WebView::create(void* /*parent*/, int /*width*/, int /*height*/,
                                         MessageHandler /*onMessage*/,
                                         FrameRequestHandler /*onFrameRequest*/)
{
    return nullptr;
}

} // namespace orizon::editor
