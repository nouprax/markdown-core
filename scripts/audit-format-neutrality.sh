#!/bin/sh
# Prove a formatting-only change generated no code.
#
# Builds the tree at REV and the working tree with identical flags, then
# compares every object's disassembly with addresses normalized away. A
# formatting change that alters one instruction fails here; nothing else in
# this repository looks at generated code at all.
#
# Build type matters. Release compiles `assert` away, so the comparison is exact
# and any difference is a real one. A debug build type (Asan, Ubsan) expands
# `assert`'s `__LINE__`, so moving a line moves an immediate; those differences
# are reported rather than hidden, because a tool that filters by shape cannot
# tell a line number from a constant.
#
# This is a MEASUREMENT TOOL, not a standing gate: it needs a REV to compare
# against, and it refuses to report success when there is nothing to compare,
# so it cannot pass vacuously. `scripts/format-c.sh --check` is the gate.
#
# Usage: scripts/audit-format-neutrality.sh <rev> [build-type]
set -eu

if [ $# -lt 1 ]; then
    echo "Usage: $0 <rev> [build-type]" >&2
    echo "  <rev>        the commit whose generated code the working tree must reproduce" >&2
    exit 2
fi
REV=$1
BUILD_TYPE=${2:-Release}

if ! git diff --quiet "$REV" -- '*.c' '*.h' '*.cpp'; then
    changed=$(git diff --name-only "$REV" -- '*.c' '*.h' '*.cpp' | wc -l | tr -d ' ')
else
    echo "$REV and the working tree have identical C sources: nothing to compare." >&2
    exit 2
fi

work=$(mktemp -d)
trap 'git worktree remove --force "$work/tree" >/dev/null 2>&1 || true; rm -rf "$work"' EXIT

echo "comparing $changed changed C source(s) against $REV, build type $BUILD_TYPE"

git worktree add --detach "$work/tree" "$REV" >/dev/null 2>&1
cmake -S "$work/tree" -B "$work/before" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" >"$work/cfg-before.log" 2>&1
cmake --build "$work/before" --parallel >"$work/build-before.log" 2>&1
cmake -S . -B "$work/after" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" >"$work/cfg-after.log" 2>&1
cmake --build "$work/after" --parallel >"$work/build-after.log" 2>&1

# Addresses and the object's own path are the only things allowed to move.
normalize() {
    objdump --disassemble --no-show-raw-insn "$1" 2>/dev/null |
        sed -e '1,2d' -e 's/^[[:space:]]*[0-9a-f]*:[[:space:]]*//'
}

identical=0
differing=0
missing=0
for after in $(find "$work/after" -name '*.o' | sort); do
    rel=${after#"$work"/after/}
    before="$work/before/$rel"
    if [ ! -f "$before" ]; then
        echo "  object present only after: $rel"
        missing=$((missing + 1))
        continue
    fi
    if cmp -s "$before" "$after"; then
        identical=$((identical + 1))
        continue
    fi
    if normalize "$before" >"$work/a.txt" && normalize "$after" >"$work/b.txt" &&
        cmp -s "$work/a.txt" "$work/b.txt"; then
        identical=$((identical + 1))
    else
        echo "  GENERATED CODE DIFFERS: $rel"
        diff "$work/a.txt" "$work/b.txt" | head -20
        differing=$((differing + 1))
    fi
done

total=$((identical + differing + missing))
if [ "$total" -eq 0 ]; then
    echo "no objects were built: nothing was compared" >&2
    exit 2
fi
echo "format neutrality: $identical/$total objects generate identical code"
if [ "$differing" -ne 0 ] || [ "$missing" -ne 0 ]; then
    echo "format neutrality audit FAILED" >&2
    exit 1
fi
echo "format neutrality audit passed."
