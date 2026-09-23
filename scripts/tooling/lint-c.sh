#!/bin/sh
set -eu

BUILD_DIR=${MARKDOWN_CORE_C_LINT_BUILD_DIR:-build/lint-c}

# The benchmark runners are C this repository compiles, so they are linted
# with everything else. The cmark runner needs the pinned oracle checkout and
# builds only when the stage benchmark's driver points the build at one.
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMARKDOWN_CORE_BENCHMARKS=ON \
    -DMARKDOWN_CORE_WARNINGS_AS_ERRORS=ON
cmake --build "$BUILD_DIR" --parallel
