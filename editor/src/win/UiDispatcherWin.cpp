#include "editor/UiDispatcher.h"

namespace orizon::editor {

// See WebViewWin.cpp: the Windows editor arrives with WebView2.
std::unique_ptr<UiDispatcher> UiDispatcher::create()
{
    return nullptr;
}

} // namespace orizon::editor
