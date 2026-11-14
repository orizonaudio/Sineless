# The webview editor.

# Build the UI with Vite and embed it in the binary.
# Vite runs through node directly: npm is a batch file on Windows, and a build step that calls a
# batch file without `call` never returns to its next command.
find_program(NODE_EXECUTABLE node REQUIRED)
# Vite comes with the packages npm ci installs; without them the build would stop at this step on a
# bare Node error.
if(NOT EXISTS ${PROJECT_SOURCE_DIR}/node_modules/vite/bin/vite.js)
    message(FATAL_ERROR "The editor's page is built with Vite: run npm ci in ${PROJECT_SOURCE_DIR}")
endif()
set(uiOutputDir ${CMAKE_CURRENT_BINARY_DIR}/ui-dist)
set(embeddedAssetData ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssetData.cpp)
file(GLOB_RECURSE uiInputs CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/ui/src/*
    ${PROJECT_SOURCE_DIR}/protocol/src/*
)
list(APPEND uiInputs ${PROJECT_SOURCE_DIR}/ui/index.html ${PROJECT_SOURCE_DIR}/ui/vite.config.ts)
add_custom_command(
    OUTPUT ${embeddedAssetData}
    # The page's brand follows SINELESS_BRANDING (cmake/Branding.cmake, ui/vite.config.ts).
    COMMAND ${CMAKE_COMMAND} -E env "SINELESS_BRANDING=$<IF:$<BOOL:${SINELESS_BRANDING}>,ON,OFF>"
            ${NODE_EXECUTABLE} ${PROJECT_SOURCE_DIR}/node_modules/vite/bin/vite.js build ui
            --outDir ${uiOutputDir} --emptyOutDir --logLevel warn
    COMMAND ${CMAKE_COMMAND} -DINPUT_DIR=${uiOutputDir} -DOUTPUT_FILE=${embeddedAssetData}
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedAssets.cmake
    DEPENDS ${uiInputs} ${PROJECT_SOURCE_DIR}/cmake/EmbedAssets.cmake
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Building and embedding the UI"
    VERBATIM
)

# The editor whatever the plugin format (editor_core: EditorCore, the webview, the session with
# the page), and the VST3 view around it (editor), which alone needs the VST3 SDK. The Audio Unit
# links editor_core.
add_library(editor_core STATIC
    src/ChosenSize.cpp
    src/EditorCore.cpp
    src/EmbeddedAssets.cpp
    src/FrameBarcode.cpp
    src/FrameFlow.cpp
    src/FrameView.cpp
    src/PointerHold.cpp
    src/PresentedCounts.cpp
    src/Session.cpp
    src/TraceGeometry.cpp
    src/TraceView.cpp
    src/Transport.cpp
    ${embeddedAssetData}
)
target_include_directories(editor_core PUBLIC include PRIVATE src)
target_link_libraries(editor_core PUBLIC engine PRIVATE branding orizon_warnings)
if(APPLE)
    target_sources(editor_core PRIVATE src/mac/DisplayScaleMac.cpp src/mac/MetalTraceRenderer.mm
                                       src/mac/TraceLayerMac.mm src/mac/UiDispatcherMac.mm
                                       src/mac/WebViewMac.mm)
    target_link_libraries(editor_core PRIVATE "-framework Cocoa" "-framework Metal"
                                              "-framework QuartzCore" "-framework WebKit")
    # With ARC, dispatch objects are released as Objective-C objects: the timers too.
    set_source_files_properties(src/mac/MetalTraceRenderer.mm src/mac/TraceLayerMac.mm
                                src/mac/UiDispatcherMac.mm src/mac/WebViewMac.mm
                                PROPERTIES COMPILE_OPTIONS -fobjc-arc)
elseif(WIN32)
    target_sources(editor_core PRIVATE src/win/DisplayScaleWin.cpp src/win/UiDispatcherWin.cpp
                                       src/win/WebViewWin.cpp)
    target_link_libraries(editor_core PRIVATE webview2 shlwapi shell32 ole32)
else()
    target_sources(editor_core PRIVATE src/linux/PlatformLinux.cpp) # Built for the checks.
endif()

add_library(editor STATIC src/Editor.cpp)
target_link_libraries(editor PUBLIC editor_core sdk PRIVATE orizon_warnings)

if(BUILD_TESTING)
    add_executable(editor_tests
        tests/ChosenSizeTests.cpp
        tests/EmbeddedAssetsTests.cpp
        tests/FrameBarcodeTests.cpp
        tests/FrameFlowTests.cpp
        tests/FrameViewTests.cpp
        tests/MinimumHeightTests.cpp
        tests/PointerHoldTests.cpp
        tests/PresentedCountsTests.cpp
        tests/SessionRecordingTests.cpp
        tests/SessionTests.cpp
        tests/TraceGeometryTests.cpp
        tests/TraceViewTests.cpp
        tests/TransportTests.cpp
    )
    target_compile_definitions(editor_tests
        PRIVATE ORIZON_PROTOCOL_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/protocol/tests/data"
                ORIZON_RECORDINGS_DIR="${PROJECT_SOURCE_DIR}/protocol/tests/data/sessions"
                ORIZON_TEST_BRAND_MARK="${PROJECT_SOURCE_DIR}/ui/src/brand/mark.svg")
    target_include_directories(editor_tests PRIVATE src)
    target_link_libraries(editor_tests
        PRIVATE editor engine_test_support branding orizon_warnings GTest::gtest_main)
    if(APPLE)
        target_sources(editor_tests PRIVATE tests/DisplayWaitTests.mm tests/EditorTests.mm tests/MetalTraceRendererTests.mm
                                            tests/NativeTracesTests.mm tests/TraceLayerTests.mm)
        target_link_libraries(editor_tests PRIVATE "-framework Cocoa" "-framework Metal"
                                                   "-framework QuartzCore" "-framework WebKit" z)
        set_source_files_properties(tests/DisplayWaitTests.mm tests/EditorTests.mm tests/MetalTraceRendererTests.mm
                                    tests/NativeTracesTests.mm tests/TraceLayerTests.mm
                                    PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    elseif(WIN32)
        target_sources(editor_tests PRIVATE tests/EditorTestsWin.cpp)
    else()
        target_sources(editor_tests PRIVATE tests/EditorTestsLinux.cpp)
    endif()
    # The editor tests run alone, though the other tests run in parallel: on Windows every
    # process's views share one WebView2 browser process, which some tests end on purpose, and a
    # page loaded beside a full machine misses the tests' timeouts.
    # Metal's API validation (read by Metal only, so macOS only) checks how the native traces call
    # Metal: a binding shorter than the shader's argument stops the test, as nothing checks a
    # release build. The renderer's tests, which draw every pipeline and binding offscreen with no
    # display link, always run with it. The others draw at a display's pace and wait a second or
    # two for it, which validation's extra work can overrun on CI's virtual Macs: they run with it
    # off CI only (GitHub Actions sets CI), where the real drawable and its presenting are checked.
    gtest_discover_tests(editor_tests DISCOVERY_TIMEOUT 30 TEST_FILTER "MetalTraceRendererTest.*"
                         PROPERTIES RUN_SERIAL TRUE ENVIRONMENT MTL_DEBUG_LAYER=1)
    gtest_discover_tests(editor_tests DISCOVERY_TIMEOUT 30 TEST_FILTER "-MetalTraceRendererTest.*"
                         PROPERTIES RUN_SERIAL TRUE ${ORIZON_LOCAL_METAL_VALIDATION})
endif()

# The performance benchmark: the real page under a simulated host, in WKWebView (macOS only).
# See bench/PerfBench.mm.
if(ORIZON_BENCH AND APPLE)
    add_executable(perf_bench bench/PerfBench.mm)
    target_include_directories(perf_bench PRIVATE src)
    target_link_libraries(perf_bench
        PRIVATE editor engine_bench_support orizon_warnings "-framework Cocoa" "-framework WebKit"
                "-framework IOKit")
    set_source_files_properties(bench/PerfBench.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    if(BUILD_TESTING)
        # perf_bench refuses a scenario it cannot play, rather than playing another under its name:
        # one that pans while frozen, one that is triggered (it plays free-running), and one that
        # is not there. It refuses before it opens a window. (A zoomed one it plays: --view.)
        foreach(scenario compare-15-frozen-pan single-triggered no-such-scenario)
            add_test(NAME PerfBench.Refuses.${scenario}
                     COMMAND ${CMAKE_COMMAND} -DBENCH=$<TARGET_FILE:perf_bench>
                             -DARGS=--scenario=${scenario}
                             "-DREFUSES=no scenario perf_bench can play is named ${scenario}"
                             -P ${PROJECT_SOURCE_DIR}/engine/bench/BenchTest.cmake)
        endforeach()
    endif()

    # The showcase video, filmed from the real page frame by frame. See showcase/Showcase.mm.
    add_executable(showcase showcase/Showcase.mm)
    target_link_libraries(showcase
        PRIVATE editor orizon_warnings "-framework Cocoa" "-framework WebKit"
                "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
                "-framework CoreText")
    set_source_files_properties(showcase/Showcase.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
endif()
