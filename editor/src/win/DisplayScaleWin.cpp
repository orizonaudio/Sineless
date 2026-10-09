#include "editor/DisplayScale.h"

// clang-format off
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format on

namespace orizon::editor {

double displayScale(void* parent)
{
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    if (parent != nullptr) {
        dpi = GetDpiForWindow(static_cast<HWND>(parent));
    } else if (GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) !=
               DPI_AWARENESS_UNAWARE) {
        dpi = GetDpiForSystem(); // The likely display until a window says otherwise.
    }
    return dpi == 0 ? 1.0 : static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI;
}

} // namespace orizon::editor
