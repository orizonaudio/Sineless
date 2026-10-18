# Runs analysis_bench as a test (engine/CMakeLists.txt):
#   cmake -DBENCH=<analysis_bench> -DARGS="<arguments>" [-DREFUSES=<message>]
#         [-DJSON=<file> -DNODE=<node> -DGATE=<scripts/engine-work.ts>] -P BenchTest.cmake
# With REFUSES, the bench must fail and say why, so that it cannot pass for another failure. With
# JSON, it writes its run there: with REFUSES, into a folder that is not there; otherwise it must
# succeed and write a run that the engine-work gate reads. Paths go in JSON, not ARGS, which is
# split at spaces.
separate_arguments(args UNIX_COMMAND "${ARGS}")
if(JSON)
    get_filename_component(jsonDir "${JSON}" DIRECTORY)
    if(REFUSES)
        file(REMOVE_RECURSE "${jsonDir}")
    else()
        file(MAKE_DIRECTORY "${jsonDir}")
        file(REMOVE "${JSON}")
    endif()
    list(APPEND args "--json=${JSON}")
endif()
execute_process(COMMAND "${BENCH}" ${args} RESULT_VARIABLE result OUTPUT_VARIABLE output
                ERROR_VARIABLE output)
if(REFUSES)
    if(result EQUAL 0)
        message(FATAL_ERROR "analysis_bench ${ARGS} succeeded:\n${output}")
    endif()
    string(FIND "${output}" "${REFUSES}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "analysis_bench ${ARGS} failed without saying \"${REFUSES}\":\n${output}")
    endif()
    return()
endif()
if(NOT result EQUAL 0)
    message(FATAL_ERROR "analysis_bench ${ARGS} failed (${result}):\n${output}")
endif()
if(JSON)
    execute_process(COMMAND "${NODE}" "${GATE}" check "${JSON}" RESULT_VARIABLE checked
                    OUTPUT_VARIABLE gateOutput ERROR_VARIABLE gateOutput)
    if(NOT checked EQUAL 0)
        message(FATAL_ERROR "The engine-work gate refuses the run analysis_bench wrote:\n${gateOutput}")
    endif()
endif()
