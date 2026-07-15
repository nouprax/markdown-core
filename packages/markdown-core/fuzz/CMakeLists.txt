include_directories(${MARKDOWN_CORE_EXTENSIONS_BINARY_DIR} ${MARKDOWN_CORE_CORE_BINARY_DIR}
                    ${MARKDOWN_CORE_EXTENSIONS_SOURCE_DIR} ${MARKDOWN_CORE_CORE_SOURCE_DIR})

macro(fuzzer name)
    add_executable(${name} ${name}.c)
    set_target_properties(${name} PROPERTIES COMPILE_FLAGS "-fsanitize=fuzzer" LINK_FLAGS "-fsanitize=fuzzer")
    target_link_libraries(${name} libmarkdown-core-extensions_static libmarkdown-core_static)
endmacro()

fuzzer(fuzz_quadratic)
fuzzer(fuzz_quadratic_brackets)
