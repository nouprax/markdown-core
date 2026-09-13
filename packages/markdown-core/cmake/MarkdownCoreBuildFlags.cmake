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
#
# The archives are also installed, and a consumer of an installed archive is
# any toolchain at all: one that links without LTO, or another compiler's
# (Kotlin/Native links the engine with its own clang). An object that holds
# only one compiler's intermediate code is unusable to every other, and even
# `nm` cannot tell its constants from its variables. So the objects are fat
# where the compiler can make them: native code next to the intermediate
# code, the former for any linker, the latter for a link across units. Where
# it cannot (Apple's clang), an archive is compiled per unit and only the
# shared library and the programs, whose objects never leave the build,
# optimize across units.
if(MARKDOWN_CORE_IPO_AVAILABLE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
    include(CheckCCompilerFlag)
    # Checked next to the LTO flag itself: the option means nothing without it.
    string(REPLACE ";" " " CMAKE_REQUIRED_FLAGS "${CMAKE_C_COMPILE_OPTIONS_IPO}")
    check_c_compiler_flag(-ffat-lto-objects MARKDOWN_CORE_HAVE_FAT_LTO_OBJECTS)
    unset(CMAKE_REQUIRED_FLAGS)
    if(MARKDOWN_CORE_HAVE_FAT_LTO_OBJECTS)
        list(REMOVE_ITEM CMAKE_C_COMPILE_OPTIONS_IPO "-fno-fat-lto-objects")
        list(APPEND CMAKE_C_COMPILE_OPTIONS_IPO "-ffat-lto-objects")
        list(REMOVE_ITEM CMAKE_CXX_COMPILE_OPTIONS_IPO "-fno-fat-lto-objects")
        list(APPEND CMAKE_CXX_COMPILE_OPTIONS_IPO "-ffat-lto-objects")
    endif()
endif()

function(markdown_core_apply_build_flags target)
    set_target_properties(${target} PROPERTIES C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    get_target_property(type ${target} TYPE)
    if(MARKDOWN_CORE_IPO_AVAILABLE AND (MARKDOWN_CORE_HAVE_FAT_LTO_OBJECTS OR NOT type STREQUAL "STATIC_LIBRARY"))
        set(across_units ON)
    else()
        # Explicit: the directory seeds ON for every target below the module.
        set(across_units OFF)
    endif()
    set_target_properties(
        ${target}
        PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ${across_units}
                   INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ${across_units}
                   INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ${across_units})
endfunction()
