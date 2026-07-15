#!/bin/sh
set -eu

VERSION="0.65.0"
INSTALL_DIR="${SWIFTLINT_INSTALL_DIR:-$PWD/.tools/swiftlint/$VERSION}"

SWIFTLINT_INSTALL_DIR="$INSTALL_DIR" scripts/install-swiftlint.sh

actual_version=$($INSTALL_DIR/swiftlint version)
if [ "$actual_version" != "$VERSION" ]; then
    echo "Expected SwiftLint $VERSION, found $actual_version" >&2
    exit 1
fi

$INSTALL_DIR/swiftlint lint --strict --config .swiftlint.yml
