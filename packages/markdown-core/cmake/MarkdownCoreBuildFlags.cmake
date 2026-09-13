# ONE SET OF RELEASE FLAGS FOR EVERY TARGET THAT CARRIES THE ENGINE.
#
# The engine is compiled into several targets (the engine and element
# archives, the facade shared library, the complete public archive, the
# diagnostics archive and the Kotlin JNI payloads), and each of them used to
# choose its own flags: only the core directory hid symbols, and none of them
# optimized across translation units, so the kind-to-descriptor lookups and
# buffer primitives that every hot path touches stayed real calls. Apply this
# to a target and it gets the same policy as every other:
#
# - hidden visibility, so the export list (the version script or exports
#   file) is the only public surface and every other symbol may be inlined
#   or dropped;
# - interprocedural optimization in the optimized configurations, when the
#   toolchain supports it and MARKDOWN_CORE_LTO is ON, so cross-unit calls
#   are inlined the way a single-unit build would inline them;
# - NDEBUG in the optimized configurations, which CMake's default Release,
#   RelWithDebInfo and MinSizeRel flags already define.
#
# Sanitizer build types keep their assertions and stay unoptimized across
# units, which is what makes their reports point at the right line.
include_guard(GLOBAL)

option(MARKDOWN_CORE_LTO "Optimize the engine across translation units in optimized configurations" ON)

set(MARKDOWN_CORE_IPO_AVAILABLE OFF)
if(MARKDOWN_CORE_LTO AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    include(CheckIPOSupported)
    check_ipo_supported(
        RESULT MARKDOWN_CORE_IPO_AVAILABLE
        OUTPUT MARKDOWN_CORE_IPO_OUTPUT
        LANGUAGES C)
endif()
if(NOT MARKDOWN_CORE_IPO_AVAILABLE)
    set(MARKDOWN_CORE_IPO_AVAILABLE OFF)
endif()

# A program that links an archive compiled across units links across units
# too: clang loads its LTO linker plugin only for a link that asks for it, and
# without the plugin GNU ld reads no bitcode ("file format not recognized"),
# while GCC hands its plugin to every link. The variables seed the property of
# every target defined in the directory that includes this module and below
# it -- the runners, tests and benchmarks that link an engine archive -- in
# the same optimized configurations the engine targets take it in.
if(MARKDOWN_CORE_IPO_AVAILABLE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
endif()

function(markdown_core_apply_build_flags target)
    set_target_properties(${target} PROPERTIES C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    if(MARKDOWN_CORE_IPO_AVAILABLE)
        set_target_properties(
            ${target}
            PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
                       INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
                       INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
    endif()
endfunction()
