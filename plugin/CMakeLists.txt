# The VST3 processor, controller and factory (DESIGN 5.1). Sources arrive with T-002.
add_library(plugin INTERFACE)
target_link_libraries(plugin INTERFACE engine editor sdk)
