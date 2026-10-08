# The webview editor.

# Build the UI with Vite and embed it in the binary.
find_program(NPM_EXECUTABLE npm REQUIRED)
set(uiOutputDir ${CMAKE_CURRENT_BINARY_DIR}/ui-dist)
set(embeddedAssetData ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssetData.cpp)
file(GLOB_RECURSE uiInputs CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/ui/src/*
    ${PROJECT_SOURCE_DIR}/protocol/src/*
)
list(APPEND uiInputs ${PROJECT_SOURCE_DIR}/ui/index.html ${PROJECT_SOURCE_DIR}/ui/vite.config.ts)
add_custom_command(
    OUTPUT ${embeddedAssetData}
    COMMAND ${NPM_EXECUTABLE} exec -- vite build ui --outDir ${uiOutputDir} --emptyOutDir
            --logLevel warn
    COMMAND ${CMAKE_COMMAND} -DINPUT_DIR=${uiOutputDir} -DOUTPUT_FILE=${embeddedAssetData}
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedAssets.cmake
    DEPENDS ${uiInputs} ${PROJECT_SOURCE_DIR}/cmake/EmbedAssets.cmake
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Building and embedding the UI"
    VERBATIM
)

add_library(editor STATIC
    src/Editor.cpp
    src/EmbeddedAssets.cpp
    src/FrameFlow.cpp
    src/Session.cpp
    src/Transport.cpp
    src/mac/UiDispatcherMac.mm
    src/mac/WebViewMac.mm
    ${embeddedAssetData}
)
target_include_directories(editor PUBLIC include PRIVATE src)
target_link_libraries(editor
    PUBLIC engine sdk
    PRIVATE orizon_warnings "-framework Cocoa" "-framework WebKit"
)
set_source_files_properties(src/mac/WebViewMac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)

if(BUILD_TESTING)
    add_executable(editor_tests
        tests/EditorTests.mm
        tests/EmbeddedAssetsTests.cpp
        tests/FrameFlowTests.cpp
        tests/SessionTests.cpp
        tests/TransportTests.cpp
    )
    target_compile_definitions(editor_tests
        PRIVATE ORIZON_PROTOCOL_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/protocol/tests/data")
    target_include_directories(editor_tests PRIVATE src)
    target_link_libraries(editor_tests
        PRIVATE editor orizon_warnings GTest::gtest_main "-framework Cocoa" "-framework WebKit")
    set_source_files_properties(tests/EditorTests.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    gtest_discover_tests(editor_tests DISCOVERY_TIMEOUT 30)
endif()

# The performance benchmark: the real page under a simulated host. See bench/PerfBench.mm.
if(ORIZON_BENCH)
    add_executable(perf_bench bench/PerfBench.mm)
    target_link_libraries(perf_bench
        PRIVATE editor orizon_warnings "-framework Cocoa" "-framework WebKit" "-framework IOKit")
    set_source_files_properties(bench/PerfBench.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
endif()
