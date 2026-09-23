#!/bin/sh
set -eu

VERSION="0.65.1"
INSTALL_DIR="${SWIFTLINT_INSTALL_DIR:-$PWD/.tools/swiftlint/$VERSION}"

if [ -x "$INSTALL_DIR/swiftlint" ]; then
    exit 0
fi

case "$(uname -s)-$(uname -m)" in
    Darwin-*)
        archive="portable_swiftlint.zip"
        checksum="c1e429b0599cf1b516f369a2d9ec04eaf0e436f3c12b637df8851fa52ff694d0"
        ;;
    Linux-x86_64)
        archive="swiftlint_linux_amd64.zip"
        checksum="caeed6f4a679c35539ffaf124f6c4ab4a8416917f7d8796279dc52b74026059d"
        ;;
    Linux-aarch64 | Linux-arm64)
        archive="swiftlint_linux_arm64.zip"
        checksum="9ffa52f478e6d8eb485d37d14715ffac90abc81c58f3370d598bf75be05605f8"
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
