cmake_minimum_required(VERSION 3.25)

set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")

project(OrizonOscilloscope VERSION 0.2.0 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
include(cmake/ProjectOptions.cmake)
include(cmake/Dependencies.cmake)

# Module order: each module may depend only on modules added before it.
add_subdirectory(protocol)
add_subdirectory(engine)
add_subdirectory(editor)
add_subdirectory(plugin)

orizon_forbid_dependencies(engine sdk sdk_common pluginterfaces base)
