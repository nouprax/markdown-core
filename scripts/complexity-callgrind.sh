#!/usr/bin/env bash
# THE CALLGRIND MATRIX (#169): the instruction count of ONE steady-state
# derivation, per unit of the shape, at two sizes, for every shape of the
# work ledger's matrix -- and the assertion that it does not grow with the
# size. Exact where a wall-clock slope is a ratio with the cache inside it:
# callgrind counts instructions, collected only inside
# markdown_core_parser_derive_tree and markdown_core_node_free, so the feed
# and the enrolling derivation stay out of the number. Two runs per shape
# and size, at 1 and at 1 + STEADY steady derivations after the enrolling
# one; the difference over STEADY is one steady derivation.
#
#   scripts/complexity-callgrind.sh [build-dir]     (default build/cmake)
#
# A shape whose spine is the size (the stair, the open nest) reads the
# per-level constant at both sizes -- about 900 and 770 instructions per
# open level. A width shape reads the accepted per-closed-block terms (the
# memo's memcpy and the vector's count, about 12 instructions per block
# as callgrind counts them) plus a constant that falls per unit with the
# size. The gate is the per-unit count at the large size against the
# small: at most RATIO_MAX times it, plus an allowance for the constant
# part. The width shapes are measured at 4,000 and 16,000 blocks rather
# than 1,000 and 4,000: below a size glibc's memcpy copies by vector
# loop and above it by `rep movsb`, which callgrind counts at one
# instruction per byte (0.8 against 8 per pointer), a strategy switch in
# the copy and not a term of ours -- both sizes must stand on the same
# side of it. The depth shapes stay at 1,000 and 4,000 levels: a stair of
# 4,000 levels is already a 16 MB document.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-$root/build/cmake}
runner="$build_dir/packages/markdown-core/tests/projection_runner"
steady=${COMPLEXITY_STEADY:-8}
ratio_max=${COMPLEXITY_RATIO_MAX:-1.10}
allowance=${COMPLEXITY_ALLOWANCE:-2000}
shapes=${COMPLEXITY_SHAPES:-"flat open_leaf fence_mixed stair closed_nest open_nest open_list open_quote"}

sizes_of() {
    case "$1" in
        stair | open_nest) echo "${COMPLEXITY_DEPTH_SMALL:-1000} ${COMPLEXITY_DEPTH_LARGE:-4000}" ;;
        *) echo "${COMPLEXITY_WIDTH_SMALL:-4000} ${COMPLEXITY_WIDTH_LARGE:-16000}" ;;
    esac
}

test -x "$runner" || { echo "complexity-callgrind: $runner is not built" >&2; exit 2; }
command -v valgrind >/dev/null || { echo "complexity-callgrind: valgrind is not installed" >&2; exit 2; }

collected() {
    # Instructions collected inside the two toggled functions.
    valgrind --tool=callgrind --callgrind-out-file=/dev/null --collect-atstart=no \
        --toggle-collect=markdown_core_parser_derive_tree --toggle-collect=markdown_core_node_free \
        "$runner" --case ledger_probe --shape "$1" --units "$2" --derivations "$3" 2>&1 >/dev/null |
        sed -n 's/^==[0-9]*== Collected : *\([0-9]*\).*/\1/p'
}

failures=0
printf '%-12s %8s %14s %14s %12s\n' shape units "Ir/derivation" "Ir/unit" "large/small"
for shape in $shapes; do
    per_unit_small=
    read -r small large <<<"$(sizes_of "$shape")"
    for units in "$small" "$large"; do
        one=$(collected "$shape" "$units" 1)
        many=$(collected "$shape" "$units" $((1 + steady)))
        test -n "$one" && test -n "$many" || { echo "complexity-callgrind: $shape/$units: no count collected" >&2; exit 1; }
        per_derivation=$(( (many - one) / steady ))
        per_unit=$(( per_derivation / units ))
        if [ "$units" = "$small" ]; then
            per_unit_small=$per_unit
            printf '%-12s %8s %14s %14s %12s\n' "$shape" "$units" "$per_derivation" "$per_unit" -
        else
            # Fixed-point ratio in hundredths, against the bound.
            ratio=$(( per_unit * 100 / (per_unit_small > 0 ? per_unit_small : 1) ))
            bound=$(( (per_unit_small * ${ratio_max/./} ) / 100 + allowance / units ))
            verdict=
            if [ "$per_unit" -gt "$bound" ]; then
                verdict=" [GROWS WITH THE SIZE: $per_unit > $bound]"
                failures=$((failures + 1))
            fi
            printf '%-12s %8s %14s %14s %11s.%02d%s\n' "$shape" "$units" "$per_derivation" "$per_unit" \
                "$((ratio / 100))" "$((ratio % 100))" "$verdict"
        fi
    done
done
if [ "$failures" -ne 0 ]; then
    echo "complexity-callgrind: $failures shape(s) read a per-unit count that grows with the size" >&2
    exit 1
fi
echo "complexity-callgrind: every shape's steady derivation is flat per unit across its two sizes"
