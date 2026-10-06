configure_file(include/engine/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/engine/Version.h @ONLY)

add_library(engine STATIC src/Version.cpp)
target_include_directories(engine PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/include)
target_link_libraries(engine PRIVATE orizon_warnings)

if(BUILD_TESTING)
    add_executable(engine_tests tests/VersionTests.cpp)
    target_link_libraries(engine_tests PRIVATE engine orizon_warnings GTest::gtest_main)
    gtest_discover_tests(engine_tests)
endif()
