#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION="23.1.0"
INSTALL_DIR="${CLANG_FORMAT_INSTALL_DIR:-$root/.tools/clang-format/$VERSION}"
executable="$INSTALL_DIR/venv/bin/clang-format"

if [ -x "$executable" ]; then
    actual=$($executable --version | sed -E 's/.*version ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
    [ "$actual" = "$VERSION" ] && exit 0
fi

python3 -m venv "$INSTALL_DIR/venv"
"$INSTALL_DIR/venv/bin/python" -m pip install --disable-pip-version-check --quiet \
    --require-hashes --requirement "$root/scripts/requirements/clang-format.txt"

actual=$($executable --version | sed -E 's/.*version ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
[ "$actual" = "$VERSION" ] || {
    echo "Expected clang-format $VERSION, found $actual" >&2
    exit 1
}
