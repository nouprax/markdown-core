#!/bin/sh
set -eu

BUILD_DIR=${MARKDOWN_CORE_C_LINT_BUILD_DIR:-build/lint-c}

cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMARKDOWN_CORE_WARNINGS_AS_ERRORS=ON
cmake --build "$BUILD_DIR" --parallel
