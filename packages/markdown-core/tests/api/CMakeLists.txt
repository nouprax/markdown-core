add_executable(api_test cplusplus.cpp harness.c harness.h main.c)
include_directories(
    ${MARKDOWN_CORE_CORE_SOURCE_DIR} ${MARKDOWN_CORE_CORE_BINARY_DIR} ${MARKDOWN_CORE_EXTENSIONS_SOURCE_DIR}
    ${MARKDOWN_CORE_EXTENSIONS_BINARY_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
# This suite validates parser internals retained for implementation coverage;
# it is deliberately not a consumer of the public shared-library ABI.
target_link_libraries(api_test libmarkdown-core-extensions_static libmarkdown-core_static)

# Compiler flags
if(MSVC)
    # Force to always compile with W4
    if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
        string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W4")
    endif()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4706 /D_CRT_SECURE_NO_WARNINGS")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /TP")
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -std=c99 -pedantic")
endif()
