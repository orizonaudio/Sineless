configure_file(include/engine/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/engine/Version.h @ONLY)

add_library(engine STATIC
    src/AnalysisWorker.cpp
    src/Analyzer.cpp
    src/CaptureRing.cpp
    src/ChannelViews.cpp
    src/FrameEncoder.cpp
    src/HoldBuffer.cpp
    src/Readouts.cpp
    src/Reducer.cpp
    src/Roll.cpp
    src/SettingJson.cpp
    src/Settings.cpp
    src/State.cpp
    src/Sweep.cpp
    src/Timeline.cpp
    src/Trigger.cpp
    src/Version.cpp
)
target_include_directories(engine PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/include)
target_link_libraries(engine PUBLIC protocol nlohmann_json::nlohmann_json PRIVATE orizon_warnings)

if(BUILD_TESTING)
    # Test-only library that replaces the global allocation operators (QG-1, DESIGN 8.2).
    add_library(engine_test_support STATIC tests/support/AllocationCounter.cpp)
    target_include_directories(engine_test_support PUBLIC tests/support)
    target_link_libraries(engine_test_support PRIVATE orizon_warnings)

    add_executable(engine_test_support_tests tests/support/AllocationCounterTests.cpp)
    target_link_libraries(engine_test_support_tests
        PRIVATE engine_test_support orizon_warnings GTest::gtest_main)
    gtest_discover_tests(engine_test_support_tests)

    add_executable(engine_tests
        tests/AnalysisWorkerTests.cpp
        tests/AnalyzerTests.cpp
        tests/CaptureRingTests.cpp
        tests/ChannelViewsTests.cpp
        tests/FrameEncoderTests.cpp
        tests/HoldBufferTests.cpp
        tests/ReadoutsTests.cpp
        tests/ReducerTests.cpp
        tests/RollTests.cpp
        tests/SettingsTests.cpp
        tests/StateTests.cpp
        tests/SweepTests.cpp
        tests/TimelineTests.cpp
        tests/TriggerTests.cpp
        tests/VersionTests.cpp
    )
    target_compile_definitions(engine_tests
        PRIVATE ORIZON_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
                ORIZON_PROTOCOL_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/protocol/tests/data")
    target_link_libraries(engine_tests
        PRIVATE engine engine_test_support orizon_warnings GTest::gtest_main)
    gtest_discover_tests(engine_tests)

endif()
