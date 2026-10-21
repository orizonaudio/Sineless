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

# An engine-only build needs no VST3 SDK (see ORIZON_ENGINE_ONLY).
if(NOT ORIZON_ENGINE_ONLY)
    FetchContent_MakeAvailable(vst3sdk)
    # A tag can be moved; the other dependencies are pinned by their archives' hashes. The tag is
    # fetched shallow, and must name this commit.
    set(vst3sdkCommit 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96)
    find_package(Git REQUIRED)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD WORKING_DIRECTORY ${vst3sdk_SOURCE_DIR}
                    OUTPUT_VARIABLE fetchedCommit OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT fetchedCommit STREQUAL vst3sdkCommit)
        message(FATAL_ERROR "VST3 SDK: v3.8.1_build_84 is ${fetchedCommit}, not ${vst3sdkCommit}")
    endif()
endif()
FetchContent_MakeAvailable(nlohmann_json)
# The fuzzers build without tests, and need no GoogleTest.
if(BUILD_TESTING)
    FetchContent_MakeAvailable(googletest)
endif()

# Apple's AudioUnitSDK for the Audio Unit (macOS only): the AU base classes and their dispatch.
# It ships an Xcode project and no CMake, so its sources build here as a static library: the ones
# an effect needs, without the MIDI and instrument classes. Version 1.4.0 needs C++23 (its headers
# use <expected>), which this target passes on to the Audio Unit module that links it; the rest of
# the project stays on C++20, which every compiler it is built with supports in full.
if(APPLE AND NOT ORIZON_ENGINE_ONLY)
    FetchContent_Declare(audiounitsdk
        URL https://github.com/apple/AudioUnitSDK/archive/refs/tags/AudioUnitSDK-1.4.0.tar.gz
        URL_HASH SHA256=a3ac2f94e8df5370df46f8bdf5e34cc07d1329d011bfa61503c8e503309c29e7
        SYSTEM
    )
    FetchContent_MakeAvailable(audiounitsdk)
    set(audioUnitSdkSources ${audiounitsdk_SOURCE_DIR}/src/AudioUnitSDK)
    add_library(audiounitsdk STATIC
        ${audioUnitSdkSources}/AUBase.cpp
        ${audioUnitSdkSources}/AUBuffer.cpp
        ${audioUnitSdkSources}/AUBufferAllocator.cpp
        ${audioUnitSdkSources}/AUEffectBase.cpp
        ${audioUnitSdkSources}/AUInputElement.cpp
        ${audioUnitSdkSources}/AUOutputElement.cpp
        ${audioUnitSdkSources}/AUPlugInDispatch.cpp
        ${audioUnitSdkSources}/AUScopeElement.cpp
        ${audioUnitSdkSources}/ComponentBase.cpp
    )
    target_include_directories(audiounitsdk SYSTEM PUBLIC ${audiounitsdk_SOURCE_DIR}/include)
    target_compile_features(audiounitsdk PUBLIC cxx_std_23)
    target_link_libraries(audiounitsdk PUBLIC
        "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
endif()

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
if(NOT ORIZON_ENGINE_ONLY)
    smtg_enable_vst3_sdk()
endif()
include(GoogleTest)
