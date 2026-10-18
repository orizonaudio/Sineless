# The VST3 processor, controller and factory.
add_library(plugin STATIC
    src/Controller.cpp
    src/Processor.cpp
)
target_include_directories(plugin PUBLIC src)
target_link_libraries(plugin
    PUBLIC engine editor sdk "$<$<PLATFORM_ID:Darwin>:-framework CoreFoundation>"
    PRIVATE orizon_warnings
)

smtg_add_vst3plugin(Sineless src/Factory.cpp)
target_link_libraries(Sineless PRIVATE plugin orizon_warnings)
# The licences travel with the plugin: Sineless's, and the embedded fonts'.
smtg_target_add_plugin_resources(Sineless RESOURCES
    ${PROJECT_SOURCE_DIR}/LICENSE
    ${PROJECT_SOURCE_DIR}/ui/src/fonts/OFL-Roboto.txt
    ${PROJECT_SOURCE_DIR}/ui/src/fonts/OFL-RobotoMono.txt
)
set_target_properties(Sineless PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resource/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER com.orizonaudio.sineless
    MACOSX_BUNDLE_BUNDLE_NAME Sineless
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
)
# Finder shows the bundle as one item only when it has a PkgInfo file, which the SDK writes
# for the Xcode generator alone.
if(APPLE)
    add_custom_command(TARGET Sineless POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/resource/PkgInfo
            $<TARGET_BUNDLE_CONTENT_DIR:Sineless>/PkgInfo
        VERBATIM
    )
endif()

if(BUILD_TESTING)
    add_executable(plugin_tests
        tests/EngineLinkTests.cpp tests/ProcessorTests.cpp tests/StateFaultTests.cpp
        tests/StateTests.cpp)
    # The SDK ships MemoryStream (a host-side IBStream) as a source file only.
    target_sources(plugin_tests
        PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/memorystream.cpp)
    target_link_libraries(plugin_tests
        PRIVATE plugin sdk_hosting engine_test_support orizon_warnings GTest::gtest_main)
    gtest_discover_tests(plugin_tests DISCOVERY_TIMEOUT 30)
endif()

# The audio-thread benchmark: process() timed against the block deadline. See bench/ProcessBench.cpp.
if(ORIZON_BENCH)
    add_executable(process_bench bench/ProcessBench.cpp)
    target_link_libraries(process_bench PRIVATE plugin orizon_warnings)
endif()
