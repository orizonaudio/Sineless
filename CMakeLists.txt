cmake_minimum_required(VERSION 3.25)

set(CMAKE_OSX_DEPLOYMENT_TARGET "13.1" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")

project(OrizonOscilloscope VERSION 0.5.2 LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX) # The macOS editor uses Cocoa and WebKit.
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

# The engine and protocol build anywhere. The plugin and editor need the VST3 SDK, and the editor
# shows only on macOS and Windows: on Linux they build for the checks alone (CI's linux preset),
# and by default only the engine and protocol build there, for contributors without the SDK.
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
if(ORIZON_FUZZ OR BUILD_TESTING)
    add_subdirectory(engine/tests/fuzz) # After the modules: it builds the editor's session too.
endif()

# The module order above, checked: a module linking one added after it fails configuration. The
# engine also stays free of the VST3 SDK.
orizon_forbid_dependencies(protocol engine editor plugin Sineless)
orizon_forbid_dependencies(engine editor plugin Sineless sdk sdk_common pluginterfaces base)
orizon_forbid_dependencies(editor plugin Sineless)
