# The VST3 processor, controller and factory (DESIGN 5.1).
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

smtg_add_vst3plugin(Oscilloscope src/Factory.cpp)
target_link_libraries(Oscilloscope PRIVATE plugin orizon_warnings)
set_target_properties(Oscilloscope PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resource/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER com.orizonaudio.oscilloscope
    MACOSX_BUNDLE_BUNDLE_NAME Oscilloscope
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
)

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
