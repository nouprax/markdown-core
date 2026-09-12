#!/usr/bin/env bash
# Element-owned lexers are committed output of the pinned generator. Normal
# builds never invoke re2c. --write is the Makefile's maintenance entry point;
# checking and regeneration use this one command and encoding declaration.
# A missing/different generator reports SKIP, never a verified pass.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
expected_re2c="re2c 4.6"
write=false
if [ "${1:-}" = --write ]; then
    write=true
    shift
fi

if ! command -v re2c >/dev/null 2>&1; then
    echo "SKIP: re2c is not installed; committed scanners.c was NOT re-verified" >&2
    [ "$write" = false ] || exit 1
    exit 0
fi
actual_re2c=$(re2c --version)
if [ "$actual_re2c" != "$expected_re2c" ]; then
    echo "SKIP: found '$actual_re2c' but the committed output is pinned to" \
        "'$expected_re2c'; committed scanners.c was NOT re-verified" >&2
    [ "$write" = false ] || exit 1
    exit 0
fi

temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT

if [ "$#" = 0 ]; then
    set -- "$root"/packages/markdown-core/elements/*_scanners.re
fi
for source in "$@"; do
    flags=(-W -Werror --case-insensitive -b -i --no-generation-date --encoding-policy substitute)
    if grep -q '^// re2c-encoding: utf8$' "$source"; then
        flags+=(-8)
    fi
    generated="$temp_dir/$(basename "${source%.re}").c"
    re2c "${flags[@]}" -o "$generated" "$source"
    if [ "$write" = true ]; then
        cp "$generated" "${source%.re}.c"
    elif ! cmp "$generated" "${source%.re}.c"; then
        echo "${source%.re}.c is not reproducible with $expected_re2c" >&2
        exit 1
    fi
    echo "$(basename "${source%.re}.c") is reproducible with $expected_re2c"
done
