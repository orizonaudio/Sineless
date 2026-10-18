# The Audio Unit (AUv2, macOS only): the AU glue on Apple's AudioUnitSDK, as plugin/ is on the VST3
# SDK. The SDK target brings C++23 with it. The component bundle that hosts load comes with the
# packaging; the tests register the Audio Unit in their own process.

add_library(au STATIC src/AudioUnit.cpp)
target_include_directories(au PUBLIC src)
target_link_libraries(au PUBLIC engine audiounitsdk PRIVATE orizon_warnings)

if(BUILD_TESTING)
    add_executable(au_tests tests/AudioUnitTests.cpp)
    target_compile_definitions(au_tests
        PRIVATE ORIZON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/engine/tests/data")
    target_link_libraries(au_tests
        PRIVATE au engine_test_support orizon_warnings GTest::gtest_main)
    gtest_discover_tests(au_tests DISCOVERY_TIMEOUT 30)
endif()
