if(${CMAKE_VERSION} VERSION_GREATER "3.3")
    cmake_policy(SET CMP0063 NEW)
endif()

include(GNUInstallDirs)

set(LIBRARY "libmarkdown-core")
set(STATICLIBRARY "libmarkdown-core_static")
set(HEADERS
    markdown-core.h
    markdown-core-extension-api.h
    parser.h
    buffer.h
    node.h
    iterator.h
    chunk.h
    references.h
    footnotes.h
    map.h
    utf8.h
    scanners.h
    inlines.h
    houdini.h
    markdown_core_ctype.h
    extension.h
    text.h)
set(LIBRARY_SOURCES
    markdown_core.c
    node.c
    iterator.c
    blocks.c
    inlines.c
    scanners.c
    scanners.re
    utf8.c
    buffer.c
    references.c
    footnotes.c
    map.c
    houdini_html_u.c
    markdown_core_ctype.c
    linked_list.c
    text.c
    ${HEADERS})

# Keep the engine target private, but make its sources available to the
# extensions directory so the public static archive can contain the complete
# parser and facade implementation. Relative paths here would otherwise be
# resolved against the extensions source directory.
set(MARKDOWN_CORE_ENGINE_SOURCES "")
foreach(source IN LISTS LIBRARY_SOURCES)
    list(APPEND MARKDOWN_CORE_ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
endforeach()
set(MARKDOWN_CORE_ENGINE_SOURCES
    "${MARKDOWN_CORE_ENGINE_SOURCES}"
    PARENT_SCOPE)

set(PROGRAM "markdown-core")
set(PROGRAM_SOURCES main.c)

include_directories(. ${CMAKE_CURRENT_BINARY_DIR})
include_directories(${MARKDOWN_CORE_EXTENSIONS_BINARY_DIR})

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/markdown-core-version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/markdown-core-version.h)

include(GenerateExportHeader)

add_executable(${PROGRAM} ${PROGRAM_SOURCES})

# The CLI is a repository-owned diagnostic adapter and needs private engine
# symbols. Keep it on the internal static graph so the installed shared facade
# can export only the reviewed read-only API.
target_link_libraries(${PROGRAM} libmarkdown-core-extensions_static libmarkdown-core_static)

# Disable the PUBLIC declarations when compiling the executable:
set_target_properties(${PROGRAM} PROPERTIES COMPILE_FLAGS
                                            "-DMARKDOWN_CORE_STATIC_DEFINE -DMARKDOWN_CORE_EXTENSIONS_STATIC_DEFINE")

# Check integrity of node structure when compiled as debug:
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DMARKDOWN_CORE_DEBUG_NODES -DDEBUG")
set(CMAKE_LINKER_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG}")

set(CMAKE_C_FLAGS_PROFILE "${CMAKE_C_FLAGS_RELEASE} -pg")
set(CMAKE_LINKER_PROFILE "${CMAKE_LINKER_FLAGS_RELEASE} -pg")

# -fvisibility=hidden
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

if(MARKDOWN_CORE_STATIC OR MARKDOWN_CORE_SHARED)
    add_library(${STATICLIBRARY} STATIC ${LIBRARY_SOURCES})
    target_compile_definitions(${STATICLIBRARY} PUBLIC MARKDOWN_CORE_STATIC_DEFINE)
    set_target_properties(${STATICLIBRARY} PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if(MSVC)
        set_target_properties(${STATICLIBRARY} PROPERTIES OUTPUT_NAME "markdown-core_static" VERSION ${PROJECT_VERSION})
    else()
        set_target_properties(${STATICLIBRARY} PROPERTIES OUTPUT_NAME "markdown-core" VERSION ${PROJECT_VERSION})
    endif(MSVC)

    generate_export_header(${STATICLIBRARY} BASE_NAME MARKDOWN_CORE EXPORT_FILE_NAME
                           ${CMAKE_CURRENT_BINARY_DIR}/markdown-core-export.h)

endif()

if(MSVC)
    set_property(
        TARGET ${PROGRAM}
        APPEND
        PROPERTY LINK_FLAGS /INCREMENTAL:NO)
endif(MSVC)

if(NOT MSVC OR CMAKE_HOST_SYSTEM_NAME STREQUAL Windows)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS ON)
    include(InstallRequiredSystemLibraries)
endif()

set(libdir lib${LIB_SUFFIX})

install(TARGETS ${PROGRAM} RUNTIME DESTINATION bin)

if(MARKDOWN_CORE_SHARED OR MARKDOWN_CORE_STATIC)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/markdown-core.pc.in ${CMAKE_CURRENT_BINARY_DIR}/markdown-core.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/markdown-core.pc DESTINATION ${libdir}/pkgconfig)

    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../include/markdown_core.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

# Feature tests
include(CheckIncludeFile)
include(CheckCSourceCompiles)
include(CheckCSourceRuns)
include(CheckSymbolExists)
check_include_file(stdbool.h HAVE_STDBOOL_H)
check_c_source_compiles("int main() { __builtin_expect(0,0); return 0; }" HAVE___BUILTIN_EXPECT)
check_c_source_compiles(
    "
  int f(void) __attribute__ (());
  int main() { return 0; }
"
    HAVE___ATTRIBUTE__)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)

# Always compile with warnings
if(MSVC)
    # Force to always compile with W4
    if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
        string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W4")
    endif()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /WX /wd4706 /wd4204 /wd4221 /wd4100 /D_CRT_SECURE_NO_WARNINGS")
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -std=c99 -pedantic")
endif()

# Compile as C++ under MSVC older than 12.0
if(MSVC AND MSVC_VERSION LESS 1800)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /TP")
endif()

if(MARKDOWN_CORE_LIB_FUZZER)
    set(FUZZ_HARNESS "markdown-core-fuzz")
    add_executable(${FUZZ_HARNESS} ${MARKDOWN_CORE_TESTS_SOURCE_DIR}/core/markdown-core-fuzz.c ${LIBRARY_SOURCES})
    target_link_libraries(${FUZZ_HARNESS} "${CMAKE_LIB_FUZZER_PATH}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize-coverage=trace-pc-guard")

    # Markdown Core is written in C but the libFuzzer runtime is written in C++ which
    # needs to link against the C++ runtime. Explicitly link it into markdown-core-fuzz.
    set_target_properties(${FUZZ_HARNESS} PROPERTIES LINK_FLAGS "-lstdc++")
endif()
