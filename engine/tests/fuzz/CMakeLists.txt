# libFuzzer targets (ORIZON_FUZZ, Clang on Linux; see the fuzz preset and scripts/fuzz.sh).
# ProjectOptions.cmake instruments the whole build for coverage and AddressSanitizer and
# UndefinedBehaviorSanitizer; here each target adds libFuzzer's driver. In every other build with
# tests, each target builds with FuzzReplay.cpp's main instead, and ctest replays its seeds and the
# inputs of fixed crashes (regressions/<target>/) on every platform: they are tested, if not fuzzed.
# Added from the top level, after every module: the session fuzzer uses the editor's sources.

set(protocolTestData ${PROJECT_SOURCE_DIR}/protocol/tests/data)
set(engineTestData ${PROJECT_SOURCE_DIR}/engine/tests/data)
set(seedDir ${CMAKE_CURRENT_BINARY_DIR}/seeds)

# orizon_add_fuzzer(<name> SEEDS <seeds subfolder> SOURCES <sources>...)
function(orizon_add_fuzzer name)
    cmake_parse_arguments(PARSE_ARGV 1 fuzzer "" "SEEDS" "SOURCES")
    if(ORIZON_FUZZ)
        add_executable(${name} ${fuzzer_SOURCES})
        target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    else()
        add_executable(${name} ${fuzzer_SOURCES} FuzzReplay.cpp)
        # A new fixed crash's input reconfigures the build, and joins the replay.
        file(GLOB regressions CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/regressions/${name}/*)
        add_test(NAME fuzz.${name} COMMAND ${name} ${seedDir}/${fuzzer_SEEDS} ${regressions})
    endif()
    target_link_libraries(${name} PRIVATE engine orizon_warnings)
endfunction()

orizon_add_fuzzer(state_fuzzer SEEDS state SOURCES StateFuzzer.cpp)
orizon_add_fuzzer(setting_json_fuzzer SEEDS setting_json SOURCES SettingJsonFuzzer.cpp)
orizon_add_fuzzer(session_fuzzer SEEDS session SOURCES
    SessionFuzzer.cpp
    ${PROJECT_SOURCE_DIR}/editor/src/Session.cpp
    ${PROJECT_SOURCE_DIR}/editor/src/Transport.cpp
)
target_include_directories(session_fuzzer PRIVATE ${PROJECT_SOURCE_DIR}/editor/include)

if(NOT ORIZON_FUZZ)
    # The replay refuses a path that names nothing, and a run with no input at all, rather than
    # passing with nothing replayed.
    # Beside real inputs, so that only the missing path can fail it: a seed folder moved or
    # misnamed must not leave the replay passing on the rest.
    add_test(NAME fuzz.replay.RefusesAMissingInput
             COMMAND state_fuzzer ${seedDir}/state ${CMAKE_CURRENT_BINARY_DIR}/no-such-input)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/no-inputs)
    add_test(NAME fuzz.replay.RefusesNoInputs
             COMMAND state_fuzzer ${CMAKE_CURRENT_BINARY_DIR}/no-inputs)
    set_tests_properties(fuzz.replay.RefusesAMissingInput fuzz.replay.RefusesNoInputs
                         PROPERTIES WILL_FAIL TRUE)
endif()

# The dictionary and the session's seeds, written from the protocol's tables and the message
# examples the contract tests share.
add_executable(fuzz_inputs FuzzInputs.cpp)
target_link_libraries(fuzz_inputs PRIVATE engine orizon_warnings)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fuzz.dict
    COMMAND fuzz_inputs ${protocolTestData}/messages.json ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS fuzz_inputs ${protocolTestData}/messages.json
    COMMENT "Writing the fuzz dictionary and session seeds"
    VERBATIM
)
add_custom_target(fuzz_inputs_written ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fuzz.dict)

# The other seeds are the saved states and message examples themselves, copied at configure time
# (and again whenever one changes).
file(GLOB stateFiles CONFIGURE_DEPENDS ${engineTestData}/*.json)
foreach(file IN LISTS stateFiles)
    get_filename_component(fileName ${file} NAME)
    configure_file(${file} ${seedDir}/state/${fileName} COPYONLY)
    configure_file(${file} ${seedDir}/setting_json/${fileName} COPYONLY)
endforeach()
configure_file(${protocolTestData}/messages.json ${seedDir}/setting_json/messages.json COPYONLY)
