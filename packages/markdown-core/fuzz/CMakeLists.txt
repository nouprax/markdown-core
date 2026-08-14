include_directories(${MARKDOWN_CORE_EXTENSIONS_BINARY_DIR} ${MARKDOWN_CORE_CORE_INCLUDE_DIR}
                    ${MARKDOWN_CORE_EXTENSIONS_SOURCE_DIR} ${MARKDOWN_CORE_CORE_SOURCE_DIR})

macro(fuzzer name)
    add_executable(${name} ${name}.c)
    set_target_properties(${name} PROPERTIES COMPILE_FLAGS "-fsanitize=fuzzer" LINK_FLAGS "-fsanitize=fuzzer")
    target_link_libraries(${name} libmarkdown-core-extensions_static libmarkdown-core_static)
endmacro()

if(MARKDOWN_CORE_FUZZ_QUADRATIC)
    fuzzer(fuzz_quadratic)
    fuzzer(fuzz_quadratic_brackets)
endif()

# The document append-script target verifies through the shared replay
# harness, which lives in the test support library.
if(MARKDOWN_CORE_FUZZ_APPENDS)
    if(NOT MARKDOWN_CORE_TESTS)
        message(FATAL_ERROR "MARKDOWN_CORE_FUZZ_APPENDS requires MARKDOWN_CORE_TESTS=ON")
    endif()
    fuzzer(fuzz_document_appends)
    target_link_libraries(fuzz_document_appends markdown-core-test-support)
endif()
