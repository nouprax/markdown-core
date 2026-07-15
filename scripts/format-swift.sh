#!/bin/sh
set -eu

EXPECTED_VERSION="6.3.0"
actual_version=$(swift format --version)
if [ "$actual_version" != "$EXPECTED_VERSION" ]; then
    echo "Expected swift-format $EXPECTED_VERSION, found $actual_version" >&2
    exit 1
fi

case "${1:-}" in
    --check)
        swift_format_args="lint"
        ;;
    "")
        swift_format_args="--in-place"
        ;;
    *)
        echo "Usage: $0 [--check]" >&2
        exit 2
        ;;
esac

    {
        printf '%s\0' Package.swift
        find packages/swift-markdown-core \
            \( -type d \( -name '.build' -o -name 'Generated' \) -prune \) -o \
            \( -type f -name '*.swift' -print0 \)
    } | xargs -0 swift format $swift_format_args --configuration .swift-format
