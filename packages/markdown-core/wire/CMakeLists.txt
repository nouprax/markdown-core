# The MCB3 encoder (docs/architecture/wire-format.md), for the bindings that
# cannot hold C node handles. It consumes the installed facade and nothing
# else, is not installed, and each binding builds it into its own adapter.
add_library(libmarkdown-core-wire STATIC markdown_core_wire.c markdown_core_wire.h)
target_include_directories(libmarkdown-core-wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(libmarkdown-core-wire PUBLIC libmarkdown-core-public-static)
set_target_properties(
    libmarkdown-core-wire
    PROPERTIES OUTPUT_NAME markdown-core-wire
               C_STANDARD 99
               C_STANDARD_REQUIRED YES
               C_EXTENSIONS NO
               POSITION_INDEPENDENT_CODE ON)
