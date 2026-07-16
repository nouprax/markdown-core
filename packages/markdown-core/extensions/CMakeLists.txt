set(LIBRARY "libmarkdown-core-extensions")
set(STATICLIBRARY "libmarkdown-core-extensions_static")
set(PUBLIC_STATIC_LIBRARY "libmarkdown-core-public-static")
set(LIBRARY_SOURCES
    ast.c
    session.c
    adopt.c
    changeset.c
    core-extensions.c
    table.c
    strikethrough.c
    autolink.c
    formula.c
    directive.c
    ext_scanners.c
    ext_scanners.re
    ext_scanners.h
    tasklist.c)

include_directories(${MARKDOWN_CORE_CORE_SOURCE_DIR} ${MARKDOWN_CORE_CORE_BINARY_DIR}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../include)

include_directories(. ${CMAKE_CURRENT_BINARY_DIR})

set(CMAKE_C_FLAGS_PROFILE "${CMAKE_C_FLAGS_RELEASE} -pg")
set(CMAKE_LINKER_PROFILE "${CMAKE_LINKER_FLAGS_RELEASE} -pg")

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(ext_scanners.c PROPERTIES COMPILE_FLAGS "-Wno-unused-variable")
endif()

if(MARKDOWN_CORE_SHARED)
    add_library(${LIBRARY} SHARED ${LIBRARY_SOURCES})

    set_target_properties(
        ${LIBRARY}
        PROPERTIES OUTPUT_NAME "markdown-core"
                   DEFINE_SYMBOL "MARKDOWN_CORE_EXTENSIONS_EXPORTS"
                   SOVERSION ${PROJECT_VERSION_MAJOR}
                   VERSION ${PROJECT_VERSION})

    set_property(
        TARGET ${LIBRARY}
        APPEND
        PROPERTY MACOSX_RPATH true)

    # Avoid name clash between PROGRAM and LIBRARY pdb files.
    set_target_properties(${LIBRARY} PROPERTIES PDB_NAME markdown-core-extensions_dll)

    list(APPEND MARKDOWN_CORE_INSTALL ${LIBRARY})
    set(MARKDOWN_CORE_EXPORT_TARGET ${LIBRARY})
    target_link_libraries(${LIBRARY} PRIVATE libmarkdown-core_static)
    target_include_directories(${LIBRARY} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
                                                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    set_target_properties(${LIBRARY} PROPERTIES EXPORT_NAME markdown-core)

    # The engine and extension implementation retain private legacy symbols
    # internally, but the installed facade DSO exports only the reviewed
    # read-only C contract.
    if(APPLE)
        target_link_options(
            ${LIBRARY} PRIVATE
            "LINKER:-exported_symbols_list,${MARKDOWN_CORE_CORE_SOURCE_DIR}/exports/markdown_core.exports")
    elseif(UNIX)
        target_link_options(${LIBRARY} PRIVATE
                            "LINKER:--version-script=${MARKDOWN_CORE_CORE_SOURCE_DIR}/exports/markdown_core.map")
    endif()

endif()

if(MARKDOWN_CORE_STATIC OR MARKDOWN_CORE_SHARED)
    add_library(${STATICLIBRARY} STATIC ${LIBRARY_SOURCES})

    target_compile_definitions(${STATICLIBRARY} PUBLIC MARKDOWN_CORE_STATIC_DEFINE
                                                       MARKDOWN_CORE_EXTENSIONS_STATIC_DEFINE)
    set_target_properties(${STATICLIBRARY} PROPERTIES DEFINE_SYMBOL "MARKDOWN_CORE_EXTENSIONS_EXPORTS"
                                                      POSITION_INDEPENDENT_CODE ON)

    if(MSVC)
        set_target_properties(${STATICLIBRARY} PROPERTIES OUTPUT_NAME "markdown-core-extensions_static"
                                                          VERSION ${PROJECT_VERSION})
    else()
        set_target_properties(${STATICLIBRARY} PROPERTIES OUTPUT_NAME "markdown-core-extensions" VERSION
                                                                                                 ${PROJECT_VERSION})
    endif(MSVC)

endif()

# The internal static targets remain useful to repository-owned tools and
# bindings. Consumers get one complete archive containing both the parser
# engine and every supported extension, with no second link dependency.
if(MARKDOWN_CORE_STATIC)
    add_library(${PUBLIC_STATIC_LIBRARY} STATIC ${MARKDOWN_CORE_ENGINE_SOURCES} ${LIBRARY_SOURCES})
    target_compile_definitions(${PUBLIC_STATIC_LIBRARY} PUBLIC MARKDOWN_CORE_STATIC_DEFINE
                                                               MARKDOWN_CORE_EXTENSIONS_STATIC_DEFINE)
    set_target_properties(
        ${PUBLIC_STATIC_LIBRARY}
        PROPERTIES OUTPUT_NAME "markdown-core"
                   EXPORT_NAME markdown-core
                   POSITION_INDEPENDENT_CODE ON)
    target_include_directories(
        ${PUBLIC_STATIC_LIBRARY}
        PRIVATE ${MARKDOWN_CORE_CORE_SOURCE_DIR} ${MARKDOWN_CORE_CORE_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    list(APPEND MARKDOWN_CORE_INSTALL ${PUBLIC_STATIC_LIBRARY})
    if(NOT MARKDOWN_CORE_SHARED)
        set(MARKDOWN_CORE_EXPORT_TARGET ${PUBLIC_STATIC_LIBRARY})
    endif()
endif()

set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS ON)

include(InstallRequiredSystemLibraries)
install(
    TARGETS ${MARKDOWN_CORE_EXPORT_TARGET}
    EXPORT markdown-core-targets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(MARKDOWN_CORE_SHARED AND MARKDOWN_CORE_STATIC)
    install(TARGETS ${PUBLIC_STATIC_LIBRARY} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(MARKDOWN_CORE_SHARED OR MARKDOWN_CORE_STATIC)
    install(
        EXPORT markdown-core-targets
        FILE markdown-core-targets.cmake
        NAMESPACE markdown-core::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/markdown-core)
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

if(CMAKE_BUILD_TYPE STREQUAL "Ubsan")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined")
endif()
