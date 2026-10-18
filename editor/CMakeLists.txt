# The webview editor.

# Build the UI with Vite and embed it in the binary.
# Vite runs through node directly: npm is a batch file on Windows, and a build step that calls a
# batch file without `call` never returns to its next command.
find_program(NODE_EXECUTABLE node REQUIRED)
set(uiOutputDir ${CMAKE_CURRENT_BINARY_DIR}/ui-dist)
set(embeddedAssetData ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssetData.cpp)
file(GLOB_RECURSE uiInputs CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/ui/src/*
    ${PROJECT_SOURCE_DIR}/protocol/src/*
)
list(APPEND uiInputs ${PROJECT_SOURCE_DIR}/ui/index.html ${PROJECT_SOURCE_DIR}/ui/vite.config.ts)
add_custom_command(
    OUTPUT ${embeddedAssetData}
    COMMAND ${NODE_EXECUTABLE} ${PROJECT_SOURCE_DIR}/node_modules/vite/bin/vite.js build ui
            --outDir ${uiOutputDir} --emptyOutDir --logLevel warn
    COMMAND ${CMAKE_COMMAND} -DINPUT_DIR=${uiOutputDir} -DOUTPUT_FILE=${embeddedAssetData}
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedAssets.cmake
    DEPENDS ${uiInputs} ${PROJECT_SOURCE_DIR}/cmake/EmbedAssets.cmake
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Building and embedding the UI"
    VERBATIM
)

add_library(editor STATIC
    src/Editor.cpp
    src/EditorCore.cpp
    src/EmbeddedAssets.cpp
    src/FrameFlow.cpp
    src/Session.cpp
    src/Transport.cpp
    ${embeddedAssetData}
)
target_include_directories(editor PUBLIC include PRIVATE src)
target_link_libraries(editor PUBLIC engine sdk PRIVATE orizon_warnings)
if(APPLE)
    target_sources(editor PRIVATE src/mac/DisplayScaleMac.cpp src/mac/UiDispatcherMac.mm
                                  src/mac/WebViewMac.mm)
    target_link_libraries(editor PRIVATE "-framework Cocoa" "-framework WebKit")
    set_source_files_properties(src/mac/WebViewMac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
elseif(WIN32)
    target_sources(editor PRIVATE src/win/DisplayScaleWin.cpp src/win/UiDispatcherWin.cpp
                                  src/win/WebViewWin.cpp)
    target_link_libraries(editor PRIVATE webview2 shlwapi shell32 ole32)
else()
    target_sources(editor PRIVATE src/linux/PlatformLinux.cpp) # Built for the checks; no editor.
endif()

if(BUILD_TESTING)
    add_executable(editor_tests
        tests/EmbeddedAssetsTests.cpp
        tests/FrameFlowTests.cpp
        tests/MinimumHeightTests.cpp
        tests/SessionTests.cpp
        tests/TransportTests.cpp
    )
    target_compile_definitions(editor_tests
        PRIVATE ORIZON_PROTOCOL_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/protocol/tests/data")
    target_include_directories(editor_tests PRIVATE src)
    target_link_libraries(editor_tests PRIVATE editor engine_test_support orizon_warnings GTest::gtest_main)
    if(APPLE)
        target_sources(editor_tests PRIVATE tests/EditorTests.mm)
        target_link_libraries(editor_tests PRIVATE "-framework Cocoa" "-framework WebKit")
        set_source_files_properties(tests/EditorTests.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    elseif(WIN32)
        target_sources(editor_tests PRIVATE tests/EditorTestsWin.cpp)
    else()
        target_sources(editor_tests PRIVATE tests/EditorTestsLinux.cpp)
    endif()
    # The editor tests run alone, though the other tests run in parallel: on Windows every
    # process's views share one WebView2 browser process, which some tests end on purpose, and a
    # page loaded beside a full machine misses the tests' timeouts.
    gtest_discover_tests(editor_tests DISCOVERY_TIMEOUT 30 PROPERTIES RUN_SERIAL TRUE)
endif()

# The performance benchmark: the real page under a simulated host, in WKWebView (macOS only).
# See bench/PerfBench.mm.
if(ORIZON_BENCH AND APPLE)
    add_executable(perf_bench bench/PerfBench.mm)
    target_link_libraries(perf_bench
        PRIVATE editor orizon_warnings "-framework Cocoa" "-framework WebKit" "-framework IOKit")
    set_source_files_properties(bench/PerfBench.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)

    # The showcase video, filmed from the real page frame by frame. See showcase/Showcase.mm.
    add_executable(showcase showcase/Showcase.mm)
    target_link_libraries(showcase
        PRIVATE editor orizon_warnings "-framework Cocoa" "-framework WebKit"
                "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
                "-framework CoreText")
    set_source_files_properties(showcase/Showcase.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
endif()
