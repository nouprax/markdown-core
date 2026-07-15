cmake_minimum_required(VERSION 3.20)

if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" MARKDOWN_CORE_VERSION LIMIT_COUNT 1)
    if(NOT MARKDOWN_CORE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "VERSION must contain a stable semantic version")
    endif()
    project(markdown-core VERSION "${MARKDOWN_CORE_VERSION}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FindAsan)

if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(
        FATAL_ERROR
            "Do not build in-source.\nPlease remove CMakeCache.txt and the CMakeFiles/ directory.\nThen: mkdir build ; cd build ; cmake .. ; make"
    )
endif()

option(MARKDOWN_CORE_TESTS "Build markdown-core tests and enable testing" ON)
option(MARKDOWN_CORE_STATIC "Build static libmarkdown-core library" ON)
option(MARKDOWN_CORE_SHARED "Build shared libmarkdown-core library" ON)
option(MARKDOWN_CORE_LIB_FUZZER "Build libFuzzer fuzzing harness" OFF)
option(MARKDOWN_CORE_FUZZ_QUADRATIC "Build quadratic fuzzing harness" OFF)
option(MARKDOWN_CORE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(MARKDOWN_CORE_WARNINGS_AS_ERRORS)
    if(MSVC)
        add_compile_options(/WX)
    else()
        add_compile_options(-Werror)
    endif()
endif()

set(MARKDOWN_CORE_CORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/core")
set(MARKDOWN_CORE_CORE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/core")
set(MARKDOWN_CORE_EXTENSIONS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extensions")
set(MARKDOWN_CORE_EXTENSIONS_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/extensions")
set(MARKDOWN_CORE_TESTS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
set(MARKDOWN_CORE_TESTS_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/tests")

if(MARKDOWN_CORE_FUZZ_QUADRATIC)
    set(FUZZER_FLAGS "-fsanitize=fuzzer-no-link,address -g")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${FUZZER_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FUZZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${FUZZER_FLAGS}")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${FUZZER_FLAGS}")
endif()

# Sanitizer build types instrument the whole package (core, extensions, and
# test runners), not just the core directory.
if(CMAKE_BUILD_TYPE STREQUAL "Ubsan")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined -g -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined -g -fno-omit-frame-pointer")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Tsan")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread -g -O1 -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -g -O1 -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=thread")
endif()

add_subdirectory(core)
add_subdirectory(extensions)

if(MARKDOWN_CORE_SHARED OR MARKDOWN_CORE_STATIC)
    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/markdown-core-config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/markdown-core-config.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/markdown-core")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/markdown-core-config-version.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/markdown-core-config.cmake"
                  "${CMAKE_CURRENT_BINARY_DIR}/markdown-core-config-version.cmake"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/markdown-core")
endif()
if(MARKDOWN_CORE_TESTS AND (MARKDOWN_CORE_SHARED OR MARKDOWN_CORE_STATIC))
    add_subdirectory(tests/api)
endif()
if(MARKDOWN_CORE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(MARKDOWN_CORE_FUZZ_QUADRATIC)
    add_subdirectory(fuzz)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE
        "Release"
        CACHE STRING "Choose the type of build, options are: Debug Profile Release Asan Ubsan." FORCE)
endif(NOT CMAKE_BUILD_TYPE)
