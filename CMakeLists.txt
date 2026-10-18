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
include(cmake/Licences.cmake)

# Module order: each module may depend only on modules added before it.
add_subdirectory(protocol)
add_subdirectory(engine)
if(NOT ORIZON_ENGINE_ONLY)
    add_subdirectory(editor)
    add_subdirectory(plugin)
    if(APPLE)
        add_subdirectory(au) # The Audio Unit, on Apple's AudioUnitSDK.
    endif()
endif()
if(ORIZON_FUZZ OR BUILD_TESTING)
    add_subdirectory(engine/tests/fuzz) # After the modules: it builds the editor's session too.
endif()

# The module order above, checked: a module linking one added after it fails configuration. The
# engine also stays free of the VST3 SDK, and only the Audio Unit links Apple's AudioUnitSDK (which
# also brings C++23 with it).
orizon_forbid_dependencies(protocol engine editor_core editor plugin Sineless au audiounitsdk)
orizon_forbid_dependencies(engine editor_core editor plugin Sineless au sdk sdk_common
                           pluginterfaces base audiounitsdk)
# The editor's format-free core links neither plugin SDK; the VST3 view around it may link VST3's.
orizon_forbid_dependencies(editor_core editor plugin Sineless au sdk sdk_common pluginterfaces base
                           audiounitsdk)
orizon_forbid_dependencies(editor plugin Sineless au audiounitsdk)
orizon_forbid_dependencies(plugin au audiounitsdk)
orizon_forbid_dependencies(Sineless au audiounitsdk)
# The two formats stay apart: the Audio Unit links neither the VST3 glue (processor or view) nor
# the VST3 SDK.
orizon_forbid_dependencies(au editor plugin Sineless sdk sdk_common pluginterfaces base)
