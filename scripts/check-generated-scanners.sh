#!/usr/bin/env bash
# Reproducibility guard for the committed re2c output (see the Makefile
# header): asserts that the maintenance rule for scanners.c reproduces the
# tracked file byte for byte. The generated file is tracked and re2c never
# runs during normal build or test, so drift between scanners.re and
# scanners.c is otherwise invisible until the next manual regeneration.
#
# The check runs only when the pinned re2c is available; otherwise it reports
# an explicit SKIP (it must not be read as a verified pass). ext_scanners.c is
# not covered: its committed copy predates the raw-output policy and carries
# hand formatting on top of the generated code. That used to cite
# docs/deprecated/specs/c-naming.md, WHICH IS NOT IN THIS REPOSITORY -- the
# citation outlived the document, and Step 15A found it while making sure no
# executable file points into docs/deprecated/.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
expected_re2c="re2c 4.5.1"

if ! command -v re2c >/dev/null 2>&1; then
    echo "SKIP: re2c is not installed; committed scanners.c was NOT re-verified" >&2
    exit 0
fi
actual_re2c=$(re2c --version)
if [ "$actual_re2c" != "$expected_re2c" ]; then
    echo "SKIP: found '$actual_re2c' but the committed output is pinned to" \
        "'$expected_re2c'; committed scanners.c was NOT re-verified" >&2
    exit 0
fi

temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT

# Exactly the Makefile maintenance rule for $(SRCDIR)/scanners.c.
re2c -W -Werror --case-insensitive -b -i --no-generation-date -8 \
    --encoding-policy substitute \
    -o "$temp_dir/scanners.c" \
    "$root/packages/markdown-core/core/scanners.re"
if ! cmp "$temp_dir/scanners.c" "$root/packages/markdown-core/core/scanners.c"; then
    echo "committed packages/markdown-core/core/scanners.c is not the $expected_re2c" \
        "output of scanners.re with the Makefile flags" >&2
    exit 1
fi
echo "committed scanners.c is reproducible from scanners.re with $expected_re2c"
