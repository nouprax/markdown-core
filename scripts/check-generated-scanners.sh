#!/usr/bin/env bash
# Reproducibility guard for the committed re2c output (see the Makefile
# header): asserts that the maintenance rule for scanners.c reproduces the
# tracked file byte for byte. The generated file is tracked and re2c never
# runs during normal build or test, so drift between scanners.re and
# scanners.c is otherwise invisible until the next manual regeneration.
#
# CI runs this in the Health Check - C job with --require, where the pinned
# re2c is provisioned by `scripts/init-environment.sh --install re2c`; there
# a missing or mismatched re2c is a hard failure. Without --require the
# check reports an explicit SKIP instead (it must not be read as a verified
# pass). ext_scanners.c is covered since its #131 regeneration replaced the
# old hand-formatted copy with raw generator output.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
expected_version="4.5.1"
expected_re2c="re2c $expected_version"

require=false
case "${1:-}" in
    --require) require=true ;;
    "") ;;
    *)
        echo "usage: scripts/check-generated-scanners.sh [--require]" >&2
        exit 2
        ;;
esac

skip() {
    if [ "$require" = true ]; then
        echo "FAIL: $1" >&2
        exit 1
    fi
    echo "SKIP: $1" >&2
    exit 0
}

re2c_bin="$root/.tools/re2c/$expected_version/bin/re2c"
if [ ! -x "$re2c_bin" ]; then
    re2c_bin=$(command -v re2c 2>/dev/null || true)
fi
if [ -z "$re2c_bin" ]; then
    skip "re2c is not installed (scripts/init-environment.sh --install re2c); committed scanners.c was NOT re-verified"
fi
actual_re2c=$("$re2c_bin" --version)
if [ "$actual_re2c" != "$expected_re2c" ]; then
    skip "found '$actual_re2c' but the committed output is pinned to '$expected_re2c'; committed scanners.c was NOT re-verified"
fi

temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT

# Exactly the Makefile maintenance rule for $(SRCDIR)/scanners.c.
"$re2c_bin" -W -Werror --case-insensitive -b -i --no-generation-date -8 \
    --encoding-policy substitute \
    -o "$temp_dir/scanners.c" \
    "$root/packages/markdown-core/core/scanners.re"
if ! cmp "$temp_dir/scanners.c" "$root/packages/markdown-core/core/scanners.c"; then
    echo "committed packages/markdown-core/core/scanners.c is not the $expected_re2c" \
        "output of scanners.re with the Makefile flags" >&2
    exit 1
fi

# Exactly the Makefile maintenance rule for $(EXTDIR)/ext_scanners.c.
"$re2c_bin" -W -Werror --case-insensitive -b -i --no-generation-date -8 \
    --encoding-policy substitute \
    -o "$temp_dir/ext_scanners.c" \
    "$root/packages/markdown-core/extensions/ext_scanners.re"
if ! cmp "$temp_dir/ext_scanners.c" "$root/packages/markdown-core/extensions/ext_scanners.c"; then
    echo "committed packages/markdown-core/extensions/ext_scanners.c is not the" \
        "$expected_re2c output of ext_scanners.re with the Makefile flags" >&2
    exit 1
fi
echo "committed scanners.c and ext_scanners.c are reproducible with $expected_re2c"
