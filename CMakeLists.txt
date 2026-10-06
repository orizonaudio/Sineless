cmake_minimum_required(VERSION 3.25)

set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version (DESIGN C-2)")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary (DESIGN C-2)")

project(OrizonOscilloscope VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
include(cmake/Dependencies.cmake)
include(cmake/ProjectOptions.cmake)

# Module order follows DESIGN 5.1: each module may depend only on modules added before it.
add_subdirectory(engine)
add_subdirectory(editor)
add_subdirectory(plugin)

orizon_forbid_dependencies(engine sdk sdk_common pluginterfaces base)
