#!/bin/sh
set -eu

EXPECTED_VERSION="22.1.8"
CLANG_FORMAT=${CLANG_FORMAT:-clang-format}

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

find packages/markdown-core -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) \
    ! -path 'packages/markdown-core/core/scanners.c' \
    ! -path 'packages/markdown-core/extensions/ext_scanners.c' \
    ! -path 'packages/markdown-core/core/include/markdown-core-export.h' \
    ! -path 'packages/markdown-core/core/include/markdown-core-version.h' \
    ! -path 'packages/markdown-core/core/include/config.h' \
    -print0 | xargs -0 "$CLANG_FORMAT" $clang_format_args
