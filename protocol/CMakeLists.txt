# Generates the C++ header from the TypeScript protocol schema. It is generated at
# build time and never committed, so the C++ build cannot use a header that is out of date.
find_program(NODE_EXECUTABLE node REQUIRED)
execute_process(COMMAND ${NODE_EXECUTABLE} --version OUTPUT_VARIABLE nodeVersion
                OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX REPLACE "^v" "" nodeVersion "${nodeVersion}")
# The oldest Node that runs the TypeScript directly, from package.json's engines field.
file(READ ${PROJECT_SOURCE_DIR}/package.json packageJson)
string(JSON nodeFloor GET "${packageJson}" engines node)
string(REGEX REPLACE "^>=" "" nodeFloor "${nodeFloor}")
if(nodeVersion VERSION_LESS nodeFloor)
    message(FATAL_ERROR "Node ${nodeVersion} found; ${nodeFloor} or newer is required "
                        "(nvm install 22, for the newest 22)")
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
    gtest_discover_tests(protocol_tests DISCOVERY_TIMEOUT 30)
endif()
