#!/bin/sh
# C host coverage producer.
#
# Builds the instrumented CTest graph, runs the correctness and conformance
# suites over it, and hands the merged llvm-cov report to the repository's one
# coverage gate. Thresholds, exemptions, and the unpinned-surface record live in
# specs/coverage/policy.json; this script decides none of them.
#
# Pass --update-ledger through to record an improvement.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

# shellcheck source=scripts/lib/discover-llvm.sh
. "$root/scripts/lib/discover-llvm.sh"
markdown_core_discover_llvm

build_dir="build/coverage"
profile_dir="$build_dir/profraw"
merged="$build_dir/markdown-core.profdata"
report="$build_dir/coverage.json"

cmake --preset coverage
cmake --build --preset coverage --parallel

rm -rf "$profile_dir"
mkdir -p "$profile_dir"

# The coverage preset excludes the `facade` label along with the timing gates.
# Those are the concurrency suites, and which code paths a thread takes there
# is decided by the scheduler: two identical runs differ by a few branches
# across a dozen files, which would make a required gate flake. They remain
# required correctness gates under the ordinary presets, and every line they
# reach is also reached by the deterministic suites — what is lost is the
# attribution, not the testing.
#
# One raw profile per process: the suites fork runners and CTest runs them in
# parallel, so a single fixed path would have concurrent writers racing over
# the same file and losing counts.
LLVM_PROFILE_FILE="$root/$profile_dir/%p-%m.profraw" ctest --preset coverage

if ! ls "$profile_dir"/*.profraw >/dev/null 2>&1; then
    echo "coverage: the instrumented suites produced no raw profile." >&2
    echo "The build is not instrumented, or the runners never started." >&2
    exit 1
fi

"$LLVM_PROFDATA" merge -sparse -o "$merged" "$profile_dir"/*.profraw

# Every instrumented image contributes its own coverage mapping, and a source
# file's counters are only complete once each image that compiled it is
# listed. Test binaries are objects here but not subjects: the policy's
# include roots keep the gate on product sources.
objects=""
for binary in $(find "$build_dir" -type f -perm -100 ! -path '*CMakeFiles*' ! -name '*.a' | sort); do
    case $(file -b "$binary" 2>/dev/null) in
        *Mach-O*executable* | *Mach-O*bundle* | *ELF*executable* | *ELF*shared*)
            objects="$objects -object $binary"
            ;;
    esac
done

if [ -z "$objects" ]; then
    echo "coverage: found no instrumented image under $build_dir." >&2
    exit 1
fi

# shellcheck disable=SC2086
# The full report, not -summary-only: llvm-cov's per-file summary charges every
# branch of a function to the file that function starts in, so a table included
# into a function body lands on its host. Only the function-level records carry
# the true attribution, and scripts/lib/coverage.mjs rebuilds branches from
# them.
"$LLVM_COV" export $objects \
    -instr-profile="$merged" \
    -ignore-filename-regex='(/tests/|/fuzz/|^/usr/|Xcode\.app)' \
    >"$report"

exec node scripts/check-coverage.mjs \
    --platform c-host \
    --format llvm-cov \
    --input "$report" \
    "$@"
