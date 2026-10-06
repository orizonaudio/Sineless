configure_file(include/engine/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/engine/Version.h @ONLY)

add_library(engine STATIC
    src/AnalysisWorker.cpp
    src/CaptureRing.cpp
    src/Version.cpp
)
target_include_directories(engine PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/include)
target_link_libraries(engine PUBLIC protocol PRIVATE orizon_warnings)

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
        tests/AnalysisWorkerTests.cpp tests/CaptureRingTests.cpp tests/VersionTests.cpp)
    target_link_libraries(engine_tests
        PRIVATE engine engine_test_support orizon_warnings GTest::gtest_main)
    gtest_discover_tests(engine_tests)

endif()
