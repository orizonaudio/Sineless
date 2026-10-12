# libFuzzer targets (ORIZON_FUZZ, Clang on Linux; see the fuzz preset and scripts/fuzz.sh).
# ProjectOptions.cmake instruments the whole build for coverage and AddressSanitizer and
# UndefinedBehaviorSanitizer; here each target adds libFuzzer's driver.
# Added from the top level, after every module: the session fuzzer uses the editor's sources.

set(protocolTestData ${PROJECT_SOURCE_DIR}/protocol/tests/data)
set(engineTestData ${PROJECT_SOURCE_DIR}/engine/tests/data)

function(orizon_add_fuzzer name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE engine orizon_warnings)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

orizon_add_fuzzer(state_fuzzer StateFuzzer.cpp)
orizon_add_fuzzer(setting_json_fuzzer SettingJsonFuzzer.cpp)
orizon_add_fuzzer(session_fuzzer
    SessionFuzzer.cpp
    ${PROJECT_SOURCE_DIR}/editor/src/Session.cpp
    ${PROJECT_SOURCE_DIR}/editor/src/Transport.cpp
)
target_include_directories(session_fuzzer PRIVATE ${PROJECT_SOURCE_DIR}/editor/include)

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
set(seedDir ${CMAKE_CURRENT_BINARY_DIR}/seeds)
file(GLOB stateFiles CONFIGURE_DEPENDS ${engineTestData}/*.json)
foreach(file IN LISTS stateFiles)
    get_filename_component(fileName ${file} NAME)
    configure_file(${file} ${seedDir}/state/${fileName} COPYONLY)
    configure_file(${file} ${seedDir}/setting_json/${fileName} COPYONLY)
endforeach()
configure_file(${protocolTestData}/messages.json ${seedDir}/setting_json/messages.json COPYONLY)
