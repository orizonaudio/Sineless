configure_file(include/engine/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/engine/Version.h @ONLY)

add_library(engine STATIC
    src/AnalysisWorker.cpp
    src/Analyzer.cpp
    src/BlockStats.cpp
    src/CaptureRing.cpp
    src/EngineRegistry.cpp
    src/ChannelViews.cpp
    src/FrameEncoder.cpp
    src/HoldBuffer.cpp
    src/InstanceRegistry.cpp
    src/LaneHold.cpp
    src/PairAnalysis.cpp
    src/Readouts.cpp
    src/Reducer.cpp
    src/Roll.cpp
    src/SettingJson.cpp
    src/Settings.cpp
    src/State.cpp
    src/Sweep.cpp
    src/SweepImage.cpp
    src/Timeline.cpp
    src/Trigger.cpp
    src/Version.cpp
)
target_include_directories(engine PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/include)
target_link_libraries(engine PUBLIC protocol nlohmann_json::nlohmann_json PRIVATE orizon_warnings)
# The engine's loops run per sample, and the development build is the one tried in hosts: it is
# optimised even in Debug builds (debug information stays). MSVC's Debug run-time checks cannot be
# combined with optimisation, so Windows hosts are tried with a release build instead.
if(NOT MSVC)
    target_compile_options(engine PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()

if(BUILD_TESTING)
    # Test-only library that replaces the global allocation operators.
    add_library(engine_test_support STATIC tests/support/AllocationCounter.cpp)
    target_include_directories(engine_test_support PUBLIC tests/support)
    target_link_libraries(engine_test_support PRIVATE orizon_warnings)

    add_executable(engine_test_support_tests tests/support/AllocationCounterTests.cpp)
    target_link_libraries(engine_test_support_tests
        PRIVATE engine_test_support orizon_warnings GTest::gtest_main)
    gtest_discover_tests(engine_test_support_tests DISCOVERY_TIMEOUT 30)

    add_executable(engine_tests
        tests/AnalysisWorkerTests.cpp
        tests/AnalyzerGridTests.cpp
        tests/AnalyzerTests.cpp
        tests/BlockStatsTests.cpp
        tests/CaptureRingTests.cpp
        tests/ChannelViewsTests.cpp
        tests/EngineRegistryTests.cpp
        tests/FrameEncoderTests.cpp
        tests/HoldBufferTests.cpp
        tests/InstanceRegistryTests.cpp
        tests/MatrixTests.cpp
        tests/PairAnalysisTests.cpp
        tests/ReadoutsTests.cpp
        tests/ReducerTests.cpp
        tests/RollTests.cpp
        tests/SettingsTests.cpp
        tests/SignalCorpusTests.cpp
        tests/StateFaultTests.cpp
        tests/StateTests.cpp
        tests/SweepTests.cpp
        tests/SweepImageTests.cpp
        tests/TimelineTests.cpp
        tests/TriggerTests.cpp
        tests/VersionTests.cpp
    )
    target_compile_definitions(engine_tests
        PRIVATE ORIZON_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
                ORIZON_PROTOCOL_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/protocol/tests/data"
                ORIZON_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus")
    target_link_libraries(engine_tests
        PRIVATE engine engine_test_support orizon_warnings GTest::gtest_main)
    if(MSVC)
        # The parameterised suites instantiate more templates than an optimised object file holds
        # sections for by default.
        target_compile_options(engine_tests PRIVATE /bigobj)
    endif()
    gtest_discover_tests(engine_tests DISCOVERY_TIMEOUT 30)

endif()

if(ORIZON_BENCH)
    # What the benchmarks share: the scenarios they play and the material, the same every run.
    add_library(engine_bench_support INTERFACE)
    target_include_directories(engine_bench_support INTERFACE bench/include)
    target_link_libraries(engine_bench_support INTERFACE protocol)

    # The analysis's work per frame, deterministic: counted under Cachegrind in CI
    # (scripts/engine-work.sh), timed on its own.
    add_executable(analysis_bench bench/AnalysisBench.cpp)
    target_link_libraries(analysis_bench PRIVATE engine engine_bench_support orizon_warnings)

    if(BUILD_TESTING)
        # The bench refuses a scenario none of whose frames shows anything (here, with no
        # columns to draw), so that CI never compares a scenario that measures nothing; the same
        # scenario drawn passes.
        add_test(NAME AnalysisBench.MeasuresAScenario
                 COMMAND analysis_bench --scenario=single-one-bar --frames=2)
        add_test(NAME AnalysisBench.RefusesAScenarioThatShowsNothing
                 COMMAND analysis_bench --scenario=single-one-bar --frames=2 --columns=0)
        set_tests_properties(AnalysisBench.RefusesAScenarioThatShowsNothing
                             PROPERTIES WILL_FAIL TRUE)
    endif()
endif()
