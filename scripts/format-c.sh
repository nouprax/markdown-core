#!/bin/sh
set -eu

EXPECTED_VERSION="22.1.8"
REPO_CLANG_FORMAT="$PWD/.tools/clang-format/$EXPECTED_VERSION/venv/bin/clang-format"
if [ -n "${CLANG_FORMAT:-}" ]; then
    :
elif [ -x "$REPO_CLANG_FORMAT" ]; then
    CLANG_FORMAT=$REPO_CLANG_FORMAT
else
    CLANG_FORMAT=clang-format
fi

actual_version=$($CLANG_FORMAT --version | sed -E 's/.*version ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
if [ "$actual_version" != "$EXPECTED_VERSION" ]; then
    echo "Expected clang-format $EXPECTED_VERSION, found $actual_version" >&2
    exit 1
fi

case "${1:-}" in
    "")
        clang_format_args="-i"
        ;;
    --check)
        clang_format_args="--dry-run --Werror"
        ;;
    *)
        echo "Usage: $0 [--check]" >&2
        exit 2
        ;;
esac

# Every C source the build compiles, not only the engine's: the ES bridge and the
# Kotlin JNI bridge are compiled by their own builds and were outside this list.
find packages/markdown-core \
    packages/es-markdown-core/src \
    packages/kotlin-markdown-core/src/native \
    -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) \
    ! -path 'packages/markdown-core/core/scanners.c' \
    ! -path 'packages/markdown-core/extensions/ext_scanners.c' \
    ! -path 'packages/markdown-core/core/include/markdown-core-export.h' \
    ! -path 'packages/markdown-core/core/include/markdown-core-version.h' \
    ! -path 'packages/markdown-core/core/include/config.h' \
    -print0 | xargs -0 "$CLANG_FORMAT" $clang_format_args
