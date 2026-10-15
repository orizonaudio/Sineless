# Compiler settings shared by all project targets.
add_library(orizon_warnings INTERFACE)
if(MSVC)
    # /utf-8: sources are UTF-8. /permissive-: standard conformance. /Zc:__cplusplus: the real
    # standard version in __cplusplus.
    target_compile_options(orizon_warnings INTERFACE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
    # /fp:precise: the VST3 SDK's CMake adds /fp:fast to every target, ours included, and an
    # optimised build may then assume there is no NaN or infinity, which the engine measures
    # around and sends for "no value". The last /fp option wins, and a target's own options come
    # after the directory's.
    target_compile_options(orizon_warnings INTERFACE /fp:precise)
else()
    target_compile_options(orizon_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)

    # Compile-time check of functions marked ORIZON_NONBLOCKING.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wfunction-effects ORIZON_HAS_FUNCTION_EFFECTS)
    if(ORIZON_HAS_FUNCTION_EFFECTS)
        target_compile_options(orizon_warnings INTERFACE -Wfunction-effects)
    endif()
endif()

# Run-time check: RealtimeSanitizer aborts when a nonblocking function allocates, locks or blocks.
option(ORIZON_THREAD_SANITIZER "Build with ThreadSanitizer" OFF)
if(ORIZON_THREAD_SANITIZER)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

option(ORIZON_ADDRESS_SANITIZER "Build with AddressSanitizer (memory errors; leaks too, but not with MSVC)" OFF)
if(ORIZON_ADDRESS_SANITIZER AND MSVC)
    # MSVC's links its runtime itself (/INFERASANLIBS), with either C runtime, and needs debug
    # information for its reports. It does not combine with the run-time checks (/RTC) or
    # incremental linking, which CMake's Debug defaults add. Its runtime DLL must be on the PATH,
    # as the Visual Studio developer environment puts it (scripts/windows-env.ps1).
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/DEBUG /INCREMENTAL:NO)
    foreach(language C CXX)
        string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_${language}_FLAGS_DEBUG "${CMAKE_${language}_FLAGS_DEBUG}")
    endforeach()
elseif(ORIZON_ADDRESS_SANITIZER)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

option(ORIZON_REALTIME_SANITIZER "Build with RealtimeSanitizer (needs Clang 20+, not AppleClang)" OFF)
if(ORIZON_REALTIME_SANITIZER)
    add_compile_options(-fsanitize=realtime)
    add_link_options(-fsanitize=realtime)
endif()

option(ORIZON_COVERAGE "Build with Clang source-based coverage (see scripts/coverage.sh)" OFF)
if(ORIZON_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ORIZON_COVERAGE needs Clang")
    endif()
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
endif()

# Fails configuration if <target> links any of the given targets, directly or transitively.
function(orizon_forbid_dependencies target)
    set(pending ${target})
    set(seen "")
    while(pending)
        list(POP_FRONT pending current)
        if(current IN_LIST seen OR NOT TARGET ${current})
            continue()
        endif()
        list(APPEND seen ${current})
        get_target_property(links ${current} LINK_LIBRARIES)
        get_target_property(interfaceLinks ${current} INTERFACE_LINK_LIBRARIES)
        foreach(link IN LISTS links interfaceLinks)
            if(link IN_LIST ARGN)
                message(FATAL_ERROR "Module '${target}' must not depend on '${link}'")
            endif()
            list(APPEND pending ${link})
        endforeach()
    endwhile()
endfunction()

# Fuzzing: libFuzzer with AddressSanitizer and UndefinedBehaviorSanitizer. The whole build is
# instrumented for coverage, so the fuzzers are guided through the engine's code, not only their
# own. Clang on Linux only: libFuzzer ships with Clang, and CI fuzzes on Linux.
option(ORIZON_FUZZ "Build the libFuzzer targets in engine/tests/fuzz (Clang on Linux)" OFF)
if(ORIZON_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "ORIZON_FUZZ needs Clang on Linux (try the fuzz preset)")
    endif()
    # -O1 even in Debug, as libFuzzer recommends, and the standard library left untraced: filling
    # each capture buffer one traced float at a time would leave a few runs a second.
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer -O1
                        -fsanitize-coverage-ignorelist=${PROJECT_SOURCE_DIR}/engine/tests/fuzz/coverage-ignorelist.txt)
    add_link_options(-fsanitize=address,undefined)
endif()

option(ORIZON_BENCH "Build the performance benchmarks (perf_bench, process_bench)" OFF)
