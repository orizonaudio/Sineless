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

} // namespace orizon::editor
