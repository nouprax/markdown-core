macro(fuzzer name)
    add_executable(${name} ${name}.c)
    set_target_properties(${name} PROPERTIES COMPILE_FLAGS "-fsanitize=fuzzer" LINK_FLAGS "-fsanitize=fuzzer")
    # The facade is all a fuzzer reads, so the installed header is all it sees.
    target_link_libraries(${name} libmarkdown-core-public-static)
endmacro()

fuzzer(fuzz_quadratic)
fuzzer(fuzz_quadratic_brackets)
