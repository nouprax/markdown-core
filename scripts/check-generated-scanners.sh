#!/usr/bin/env bash
# Reproducibility guard for the committed re2c output (see the Makefile
# header): asserts that the maintenance rule for scanners.c reproduces the
# tracked file byte for byte. The generated file is tracked and re2c never
# runs during normal build or test, so drift between scanners.re and
# scanners.c is otherwise invisible until the next manual regeneration.
#
# Both scanner families are raw, reproducible output of the pinned generator.
# A missing/different generator reports SKIP, never a verified pass.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
expected_re2c="re2c 4.6"

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

# Exactly the Makefile maintenance rules, including the extension encoding.
for family in core/scanners extensions/ext_scanners; do
    flags=(-W -Werror --case-insensitive -b -i --no-generation-date --encoding-policy substitute)
    if [ "$family" = extensions/ext_scanners ]; then
        flags+=(-8)
    fi
    generated="$temp_dir/$(basename "$family").c"
    re2c "${flags[@]}" \
        -o "$generated" "$root/packages/markdown-core/$family.re"
    if ! cmp "$generated" "$root/packages/markdown-core/$family.c"; then
        echo "$family.c is not reproducible with $expected_re2c" >&2
        exit 1
    fi
    echo "$family.c is reproducible with $expected_re2c"
done
