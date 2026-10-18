# The component's binary exports only the factory hosts call: everything else (the engine, the
# editor, the SDK) stays inside it, so nothing in it can clash with another plugin's code in the
# same host. Run with -DBINARY=<the component's executable>.
# Every architecture's slice: Intel Macs load the x86_64 one.
execute_process(COMMAND lipo -archs "${BINARY}" OUTPUT_VARIABLE architectures
                OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "lipo failed on ${BINARY}")
endif()
string(REPLACE " " ";" architectures "${architectures}")
foreach(architecture IN LISTS architectures)
    execute_process(COMMAND nm -arch ${architecture} -gU "${BINARY}"
                    OUTPUT_VARIABLE symbols RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "nm failed on the ${architecture} slice of ${BINARY}")
    endif()
    string(REGEX MATCHALL "[^\n]+" lines "${symbols}")
    set(exported "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^.* " "" name "${line}")
        list(APPEND exported "${name}")
    endforeach()
    if(NOT exported STREQUAL "_SinelessFactory")
        list(LENGTH exported count)
        message(FATAL_ERROR
                "${architecture}: expected only _SinelessFactory to be exported; found ${count}: ${exported}")
    endif()
    message(STATUS "${architecture}: exports only _SinelessFactory")
endforeach()
