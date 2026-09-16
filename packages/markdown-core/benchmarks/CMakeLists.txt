# The stage benchmark's two runners.
#
# Both are built from one driver source and one engine source so that neither
# parser can be handed a different harness. They are never registered with
# CTest: a callgrind measurement is evidence for a reviewer, and a test graph
# that can report it is a test graph that can eventually gate on it.
#
# The cmark runner needs the pinned oracle checkout, which is a local tool and
# not part of the repository. It is built only when the driver points at one,
# and its absence is reported rather than silently producing a one-sided
# comparison.

add_executable(markdown_core_stage_runner stage_runner.c markdown_core_stages.c)
target_include_directories(markdown_core_stage_runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                                              "${CMAKE_CURRENT_SOURCE_DIR}/../include")
# The static engine on purpose: the stage boundaries the driver reads are
# internal symbols of the parse transaction, and a static link keeps them in
# the measured binary regardless of the library's exported surface.
target_link_libraries(markdown_core_stage_runner PRIVATE libmarkdown-core-public-static)

if(MARKDOWN_CORE_CMARK_SOURCE_DIR AND MARKDOWN_CORE_CMARK_BUILD_DIR)
    if(NOT EXISTS "${MARKDOWN_CORE_CMARK_SOURCE_DIR}/cmark.h")
        message(FATAL_ERROR "MARKDOWN_CORE_CMARK_SOURCE_DIR does not hold cmark.h: ${MARKDOWN_CORE_CMARK_SOURCE_DIR}")
    endif()
    # Re-resolved on every configure: find_library caches its answer, and a
    # cached path from an earlier output directory would link the comparison
    # against an archive built with flags nobody asked for.
    unset(MARKDOWN_CORE_CMARK_LIBRARY CACHE)
    # Static suffixes only. CMake searches ".so" before ".a" on Unix, and
    # configuring with BUILD_SHARED_LIBS=OFF does not delete what an earlier
    # shared build left behind -- so an unconstrained lookup in a reused tree
    # links a stale DSO whose bytes no provenance check here covers, because
    # both the .comment check and the binary digest read the executable.
    set(_markdown_core_find_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_STATIC_LIBRARY_SUFFIX}")
    find_library(
        MARKDOWN_CORE_CMARK_LIBRARY
        NAMES cmark
        PATHS "${MARKDOWN_CORE_CMARK_BUILD_DIR}"
        NO_DEFAULT_PATH REQUIRED)
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${_markdown_core_find_suffixes}")
    add_executable(cmark_stage_runner stage_runner.c cmark_stages.c)
    target_include_directories(
        cmark_stage_runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${MARKDOWN_CORE_CMARK_SOURCE_DIR}"
                                   "${MARKDOWN_CORE_CMARK_BUILD_DIR}")
    # The pinned oracle is built as a static archive, so the comparison links
    # the same translation units the driver compiled with the profile flags.
    target_compile_definitions(cmark_stage_runner PRIVATE CMARK_STATIC_DEFINE)
    target_link_libraries(cmark_stage_runner PRIVATE "${MARKDOWN_CORE_CMARK_LIBRARY}")
endif()

if(MARKDOWN_CORE_CMARK_GFM_SOURCE_DIR AND MARKDOWN_CORE_CMARK_GFM_BUILD_DIR)
    if(NOT EXISTS "${MARKDOWN_CORE_CMARK_GFM_SOURCE_DIR}/src/cmark-gfm.h")
        message(
            FATAL_ERROR
                "MARKDOWN_CORE_CMARK_GFM_SOURCE_DIR does not hold src/cmark-gfm.h: ${MARKDOWN_CORE_CMARK_GFM_SOURCE_DIR}"
        )
    endif()
    # Re-resolved and static-only for the same reasons as cmark above.
    unset(MARKDOWN_CORE_CMARK_GFM_LIBRARY CACHE)
    unset(MARKDOWN_CORE_CMARK_GFM_PLUGINS_LIBRARY CACHE)
    set(_markdown_core_find_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_STATIC_LIBRARY_SUFFIX}")
    find_library(
        MARKDOWN_CORE_CMARK_GFM_LIBRARY
        NAMES cmark-gfm
        PATHS "${MARKDOWN_CORE_CMARK_GFM_BUILD_DIR}/src"
        NO_DEFAULT_PATH REQUIRED)
    find_library(
        MARKDOWN_CORE_CMARK_GFM_PLUGINS_LIBRARY
        NAMES cmark-gfm-extensions
        PATHS "${MARKDOWN_CORE_CMARK_GFM_BUILD_DIR}/extensions"
        NO_DEFAULT_PATH REQUIRED)
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${_markdown_core_find_suffixes}")
    add_executable(cmark_gfm_stage_runner stage_runner.c cmark_gfm_stages.c)
    target_include_directories(
        cmark_gfm_stage_runner
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${MARKDOWN_CORE_CMARK_GFM_SOURCE_DIR}/src"
                "${MARKDOWN_CORE_CMARK_GFM_SOURCE_DIR}/extensions" "${MARKDOWN_CORE_CMARK_GFM_BUILD_DIR}/src")
    target_compile_definitions(cmark_gfm_stage_runner PRIVATE CMARK_GFM_STATIC_DEFINE)
    # Extensions first: they call back into the core archive.
    target_link_libraries(cmark_gfm_stage_runner PRIVATE "${MARKDOWN_CORE_CMARK_GFM_PLUGINS_LIBRARY}"
                                                         "${MARKDOWN_CORE_CMARK_GFM_LIBRARY}")
endif()
