cmake_minimum_required(VERSION 3.25)

set(CMAKE_OSX_DEPLOYMENT_TARGET "13.1" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")

project(OrizonOscilloscope VERSION 0.5.0 LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX) # The macOS editor uses Cocoa and WebKit.
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

# The engine and protocol build anywhere; the editor and plugin need the macOS or Windows SDKs.
# Elsewhere (Linux CI, contributors without a Mac or Windows) only the engine and protocol build.
if(APPLE OR WIN32)
    set(orizonPlatformDefault OFF)
else()
    set(orizonPlatformDefault ON)
endif()
option(ORIZON_ENGINE_ONLY "Build only the engine and protocol, without the VST3 SDK"
       ${orizonPlatformDefault})

include(cmake/ProjectOptions.cmake)
include(cmake/Dependencies.cmake)

# Module order: each module may depend only on modules added before it.
add_subdirectory(protocol)
add_subdirectory(engine)
if(NOT ORIZON_ENGINE_ONLY)
    add_subdirectory(editor)
    add_subdirectory(plugin)
endif()
if(ORIZON_FUZZ)
    add_subdirectory(engine/tests/fuzz) # After the modules: it builds the editor's session too.
endif()

orizon_forbid_dependencies(engine sdk sdk_common pluginterfaces base)
