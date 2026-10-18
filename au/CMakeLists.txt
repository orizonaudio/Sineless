# The Audio Unit (AUv2, macOS only): the AU glue on Apple's AudioUnitSDK, as plugin/ is on the VST3
# SDK, and its view around the format-free editor. The SDK target brings C++23 with it. The
# component bundle that hosts load comes with the packaging; the tests register the Audio Unit in
# their own process.

add_library(au STATIC src/AudioUnit.cpp src/AudioUnitView.mm)
target_include_directories(au PUBLIC src)
target_link_libraries(au PUBLIC engine editor_core audiounitsdk
                         PRIVATE orizon_warnings "-framework Cocoa" "-framework WebKit")
set_source_files_properties(src/AudioUnitView.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)

# The view's Objective-C classes are named for this build: Sineless, the version and an id made
# once per build directory, so that two copies or versions in one host (an installed release
# beside a development build) never share a class. Hosts look the factory up by name in the
# component's bundle, so it is named at compile time, in the bundle's own image.
if(NOT ORIZON_AU_BUILD_ID)
    string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef buildId)
    set(ORIZON_AU_BUILD_ID ${buildId} CACHE INTERNAL "The Audio Unit view's class-name id")
endif()
string(REPLACE "." "_" auVersion "${PROJECT_VERSION}")
target_compile_definitions(au
    PRIVATE ORIZON_AU_CLASS_PREFIX=Sineless_${auVersion}_${ORIZON_AU_BUILD_ID})

# The component hosts load: Sineless.component, a loadable bundle around the Audio Unit whose
# binary exports the factory function (src/ComponentEntry.cpp). Its Info.plist entry for hosts
# (AudioComponents), signing and installation come with the packaging.
add_library(SinelessAU MODULE src/ComponentEntry.cpp)
target_link_libraries(SinelessAU PRIVATE au orizon_warnings)
set_target_properties(SinelessAU PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION component
    OUTPUT_NAME Sineless
    MACOSX_BUNDLE_GUI_IDENTIFIER com.orizonaudio.sineless.component
    MACOSX_BUNDLE_BUNDLE_NAME Sineless)

if(BUILD_TESTING)
    add_executable(au_tests tests/AudioUnitTests.cpp)
    target_compile_definitions(au_tests
        PRIVATE ORIZON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/engine/tests/data")
    target_link_libraries(au_tests
        PRIVATE au engine_test_support orizon_warnings GTest::gtest_main)
    gtest_discover_tests(au_tests DISCOVERY_TIMEOUT 30)

    # The view loads the real page in WKWebView, so its tests run alone, as the editor's do. They
    # run from a bundle, as the component does, so that a host's lookup of the view works as in a
    # host.
    add_executable(au_view_tests MACOSX_BUNDLE tests/AudioUnitViewTests.mm)
    target_include_directories(au_view_tests PRIVATE ${PROJECT_SOURCE_DIR}/editor/tests)
    target_link_libraries(au_view_tests
        PRIVATE au orizon_warnings GTest::gtest_main "-framework Cocoa" "-framework WebKit")
    set_source_files_properties(tests/AudioUnitViewTests.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    gtest_discover_tests(au_view_tests DISCOVERY_TIMEOUT 30 PROPERTIES RUN_SERIAL TRUE)

    # The component as a host meets it: an app of its own that loads Sineless.component and links
    # none of the Audio Unit's code, so that the component's bundle and the host's differ.
    add_executable(au_component_tests MACOSX_BUNDLE tests/ComponentTests.mm)
    target_include_directories(au_component_tests
        PRIVATE src ${PROJECT_SOURCE_DIR}/editor/tests)
    target_compile_definitions(au_component_tests
        PRIVATE ORIZON_TEST_COMPONENT="$<TARGET_BUNDLE_DIR:SinelessAU>")
    target_link_libraries(au_component_tests
        PRIVATE orizon_warnings GTest::gtest_main "-framework Cocoa" "-framework AudioToolbox")
    add_dependencies(au_component_tests SinelessAU)
    set_source_files_properties(tests/ComponentTests.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    gtest_discover_tests(au_component_tests DISCOVERY_TIMEOUT 30 PROPERTIES RUN_SERIAL TRUE)
    orizon_forbid_dependencies(au_component_tests au editor_core engine)
endif()
