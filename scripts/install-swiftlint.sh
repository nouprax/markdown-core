#!/bin/sh
set -eu

VERSION="0.65.0"
INSTALL_DIR="${SWIFTLINT_INSTALL_DIR:-$PWD/.tools/swiftlint/$VERSION}"

if [ -x "$INSTALL_DIR/swiftlint" ]; then
    exit 0
fi

case "$(uname -s)-$(uname -m)" in
    Darwin-*)
        archive="portable_swiftlint.zip"
        checksum="d6cb0aa7a2f5f1ef306fc9e37bcb54dc9a26facc8f7784ac0c3dd3eccf5c6ba6"
        ;;
    Linux-x86_64)
        archive="swiftlint_linux_amd64.zip"
        checksum="79306a34e5c7cc55a220cd108cbb861dcad5f10138dcdf261e2624ae8b0a486b"
        ;;
    Linux-aarch64 | Linux-arm64)
        archive="swiftlint_linux_arm64.zip"
        checksum="12d3b84bc5b69ae13a99a5a5c79904f9ce25867f099f6368d0037854f9ee6c26"
        ;;
    *)
        echo "Unsupported SwiftLint host: $(uname -s)-$(uname -m)" >&2
        exit 1
        ;;
esac

temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT

url="https://github.com/realm/SwiftLint/releases/download/$VERSION/$archive"
curl --fail --location --silent --show-error "$url" --output "$temp_dir/$archive"

if command -v shasum >/dev/null 2>&1; then
    actual_checksum=$(shasum -a 256 "$temp_dir/$archive" | awk '{print $1}')
else
    actual_checksum=$(sha256sum "$temp_dir/$archive" | awk '{print $1}')
fi

if [ "$actual_checksum" != "$checksum" ]; then
    echo "SwiftLint checksum mismatch" >&2
    exit 1
fi

mkdir -p "$INSTALL_DIR"
unzip -q "$temp_dir/$archive" -d "$INSTALL_DIR"

if [ ! -x "$INSTALL_DIR/swiftlint" ]; then
    swiftlint_path=$(find "$INSTALL_DIR" -type f -name swiftlint | head -n 1)
    if [ -z "$swiftlint_path" ]; then
        echo "SwiftLint executable not found in $archive" >&2
        exit 1
    fi
    mv "$swiftlint_path" "$INSTALL_DIR/swiftlint"
    chmod +x "$INSTALL_DIR/swiftlint"
fi
