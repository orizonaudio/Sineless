# Third-party C++ dependencies, pinned.
include(FetchContent)

# The C++ runtime is linked statically on Windows, so that users need no Visual C++
# redistributable. The VST3 SDK adds its own runtime flag; both must agree.
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(SMTG_USE_STATIC_CRT ON CACHE BOOL "" FORCE)
endif()

# VST3 SDK: only the submodules needed for a plugin without VSTGUI.
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG v3.8.1_build_84
    GIT_SHALLOW TRUE
    GIT_SUBMODULES base cmake pluginterfaces public.sdk
    SYSTEM
)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    SYSTEM
)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    SYSTEM
)

FetchContent_MakeAvailable(vst3sdk googletest nlohmann_json)

# WebView2 SDK for the Windows editor: headers and the static loader, so that no WebView2Loader.dll
# ships next to the plugin. The NuGet package is a zip under another extension.
if(WIN32)
    FetchContent_Declare(webview2
        URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4258.31/microsoft.web.webview2.1.0.4258.31.nupkg
        URL_HASH SHA256=56F7F4B8BF9AEE4B8EFEFBBDD4F67D5F74EBD1B100ED0806DA71BF76AF481AA9
        DOWNLOAD_NAME microsoft.web.webview2.zip
    )
    FetchContent_MakeAvailable(webview2)
    if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
        set(webview2Arch arm64)
    else()
        set(webview2Arch x64)
    endif()
    add_library(webview2 STATIC IMPORTED)
    set_target_properties(webview2 PROPERTIES
        IMPORTED_LOCATION ${webview2_SOURCE_DIR}/build/native/${webview2Arch}/WebView2LoaderStatic.lib
        INTERFACE_INCLUDE_DIRECTORIES ${webview2_SOURCE_DIR}/build/native/include
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${webview2_SOURCE_DIR}/build/native/include
    )
endif()
smtg_enable_vst3_sdk()
include(GoogleTest)
