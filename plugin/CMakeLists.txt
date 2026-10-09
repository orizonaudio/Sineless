# The VST3 processor, controller and factory.
add_library(plugin STATIC
    src/Controller.cpp
    src/EngineRegistry.cpp
    src/Processor.cpp
)
target_include_directories(plugin PUBLIC src)
target_link_libraries(plugin
    PUBLIC engine editor sdk "$<$<PLATFORM_ID:Darwin>:-framework CoreFoundation>"
    PRIVATE orizon_warnings
)

smtg_add_vst3plugin(O-Scope src/Factory.cpp)
target_link_libraries(O-Scope PRIVATE plugin orizon_warnings)
set_target_properties(O-Scope PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resource/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER com.orizonaudio.o-scope
    MACOSX_BUNDLE_BUNDLE_NAME O-Scope
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
)
# Finder shows the bundle as one item only when it has a PkgInfo file, which the SDK writes
# for the Xcode generator alone.
if(APPLE)
    add_custom_command(TARGET O-Scope POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/resource/PkgInfo
            $<TARGET_BUNDLE_CONTENT_DIR:O-Scope>/PkgInfo
        VERBATIM
    )
endif()

if(BUILD_TESTING)
    add_executable(plugin_tests
        tests/EngineLinkTests.cpp tests/ProcessorTests.cpp tests/StateTests.cpp)
    # The SDK ships MemoryStream (a host-side IBStream) as a source file only.
    target_sources(plugin_tests
        PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/memorystream.cpp)
    target_link_libraries(plugin_tests
        PRIVATE plugin sdk_hosting engine_test_support orizon_warnings GTest::gtest_main)
    gtest_discover_tests(plugin_tests)
endif()

# The audio-thread benchmark: process() timed against the block deadline. See bench/ProcessBench.cpp.
if(ORIZON_BENCH)
    add_executable(process_bench bench/ProcessBench.cpp)
    target_link_libraries(process_bench PRIVATE plugin orizon_warnings)
endif()
