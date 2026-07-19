include_directories(${MARKDOWN_CORE_EXTENSIONS_BINARY_DIR} ${MARKDOWN_CORE_CORE_BINARY_DIR}
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

# The session edit-script target verifies through the shared replay harness,
# which lives in the test support library.
if(MARKDOWN_CORE_FUZZ_SESSION)
    if(NOT MARKDOWN_CORE_TESTS)
        message(FATAL_ERROR "MARKDOWN_CORE_FUZZ_SESSION requires MARKDOWN_CORE_TESTS=ON")
    endif()
    fuzzer(fuzz_session_edits)
    target_link_libraries(fuzz_session_edits markdown-core-test-support)
endif()
