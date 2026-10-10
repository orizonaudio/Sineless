#pragma once

// Include after windows.h.

namespace orizon::editor {

// The module (this plugin's DLL) that registers window classes, so that each loaded copy of the
// plugin has its own classes.
inline HINSTANCE thisModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModule), &module);
    return module;
}

// Keeps this DLL loaded until the process exits. WebView2 completes calls into it
// asynchronously, possibly after the editor has closed; a host that unloaded the plugin in
// between would have them run code that is gone.
inline void pinThisModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&thisModule), &module);
}

} // namespace orizon::editor
