# The language engine (docs/plans/2026-10-23-lsp-completion-engine.md,
# section 4). It consumes the installed facade and nothing else. It is not
# installed yet: its C contract grows with the completion providers, and the
# platform bindings build it from source.
set(MARKDOWN_CORE_LSP_SOURCES server.c text.c)
add_library(libmarkdown-core-lsp STATIC ${MARKDOWN_CORE_LSP_SOURCES} include/markdown_core_lsp.h text.h)
target_include_directories(libmarkdown-core-lsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(libmarkdown-core-lsp PUBLIC MARKDOWN_CORE_LSP_STATIC_DEFINE)
target_link_libraries(libmarkdown-core-lsp PUBLIC libmarkdown-core-public-static)
set_target_properties(
    libmarkdown-core-lsp
    PROPERTIES OUTPUT_NAME markdown-core-lsp
               C_STANDARD 99
               C_STANDARD_REQUIRED YES
               C_EXTENSIONS NO
               POSITION_INDEPENDENT_CODE ON)

# The JSON-RPC adapter (section 4.2): the protocol's framing and messages,
# mapped onto the engine. The transcript runner drives the same objects the
# executable does.
add_library(libmarkdown-core-adapter STATIC adapter.c frame.c json.c adapter.h frame.h json.h)
target_include_directories(libmarkdown-core-adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(libmarkdown-core-adapter PUBLIC libmarkdown-core-lsp)
set_target_properties(
    libmarkdown-core-adapter
    PROPERTIES OUTPUT_NAME markdown-core-adapter
               C_STANDARD 99
               C_STANDARD_REQUIRED YES
               C_EXTENSIONS NO)

add_executable(markdown-core-server main.c)
target_link_libraries(markdown-core-server libmarkdown-core-adapter)
set_target_properties(
    markdown-core-server
    PROPERTIES C_STANDARD 99
               C_STANDARD_REQUIRED YES
               C_EXTENSIONS NO)
