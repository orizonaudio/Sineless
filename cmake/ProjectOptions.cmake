# Compiler settings shared by all project targets.
add_library(orizon_warnings INTERFACE)
if(MSVC)
    # /utf-8: sources are UTF-8. /permissive-: standard conformance. /Zc:__cplusplus: the real
    # standard version in __cplusplus.
    target_compile_options(orizon_warnings INTERFACE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
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

option(ORIZON_ADDRESS_SANITIZER "Build with AddressSanitizer (memory errors and leaks)" OFF)
if(ORIZON_ADDRESS_SANITIZER)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

option(ORIZON_REALTIME_SANITIZER "Build with RealtimeSanitizer (needs Clang 20+, not AppleClang)" OFF)
if(ORIZON_REALTIME_SANITIZER)
    add_compile_options(-fsanitize=realtime)
    add_link_options(-fsanitize=realtime)
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

option(ORIZON_BENCH "Build the performance benchmarks (perf_bench, process_bench)" OFF)
