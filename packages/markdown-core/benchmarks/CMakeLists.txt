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
    find_library(
        MARKDOWN_CORE_CMARK_LIBRARY
        NAMES cmark
        PATHS "${MARKDOWN_CORE_CMARK_BUILD_DIR}"
        NO_DEFAULT_PATH REQUIRED)
    add_executable(cmark_stage_runner stage_runner.c cmark_stages.c)
    target_include_directories(
        cmark_stage_runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${MARKDOWN_CORE_CMARK_SOURCE_DIR}"
                                   "${MARKDOWN_CORE_CMARK_BUILD_DIR}")
    # The pinned oracle is built as a static archive, so the comparison links
    # the same translation units the driver compiled with the profile flags.
    target_compile_definitions(cmark_stage_runner PRIVATE CMARK_STATIC_DEFINE)
    target_link_libraries(cmark_stage_runner PRIVATE "${MARKDOWN_CORE_CMARK_LIBRARY}")
endif()
