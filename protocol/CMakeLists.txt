# Generates the C++ header from the TypeScript protocol schema. It is generated at
# build time and never committed, so the C++ build cannot use a header that is out of date.
find_program(NODE_EXECUTABLE node REQUIRED)
execute_process(COMMAND ${NODE_EXECUTABLE} --version OUTPUT_VARIABLE nodeVersion
                OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX REPLACE "^v" "" nodeVersion "${nodeVersion}")
if(nodeVersion VERSION_LESS 22.18)
    message(FATAL_ERROR "Node ${nodeVersion} found; 22.18 or newer is required (see .nvmrc)")
endif()

set(generatedDir ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(protocolHeader ${generatedDir}/protocol/Protocol.h)
file(GLOB schemaSources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.ts)
add_custom_command(
    OUTPUT ${protocolHeader}
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-cpp.ts ${protocolHeader}
    DEPENDS ${schemaSources} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-cpp.ts
    COMMENT "Generating the C++ protocol header"
    VERBATIM
)
add_custom_target(protocol_header DEPENDS ${protocolHeader})

add_library(protocol INTERFACE)
target_include_directories(protocol INTERFACE ${generatedDir})
add_dependencies(protocol protocol_header)

if(BUILD_TESTING)
    add_executable(protocol_tests tests/ProtocolHeaderTests.cpp)
    target_link_libraries(protocol_tests PRIVATE protocol orizon_warnings GTest::gtest_main)
    gtest_discover_tests(protocol_tests)
endif()
