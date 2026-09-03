#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/artifact.sh"
preset=${1:-}
shared=${2:-}
output=${3:-}
configuration=${4:-}

case "$preset" in
    default) build_dir=build/cmake ;;
    asan | ubsan | tsan) build_dir="build/$preset" ;;
    *)
        echo "usage: $0 default|asan|ubsan|tsan <ON|OFF|-> <output-dir> [configuration]" >&2
        exit 2
        ;;
esac
test -n "$output"

# A test artifact is a snapshot of the current graph, not of whichever targets
# happened to exist in this build directory previously. Removed runners must
# not survive as stale executables in the archive.
cmake -E remove_directory "$root/$build_dir"
configure=(cmake --preset "$preset" -DMARKDOWN_CORE_TESTS=ON)
if [ "$shared" != - ]; then
    configure+=("-DMARKDOWN_CORE_SHARED=$shared")
fi
"${configure[@]}"

build=(cmake --build --preset "$preset" --parallel)
if [ -n "$configuration" ]; then
    build+=(--config "$configuration")
fi
"${build[@]}"

inventory=(ctest --test-dir "$root/$build_dir" -N)
if [ -n "$configuration" ]; then
    inventory+=(-C "$configuration")
fi
if ! "${inventory[@]}" | grep -Eq 'Total Tests: [1-9][0-9]*$'; then
    echo "C test artifact contains no discovered CTest tests" >&2
    exit 1
fi
bash "$root/scripts/audit-test-topology.sh" "$root/$build_dir" "$configuration"
if find "$root/$build_dir" -type f \( -name bench_runner -o -name bench_runner.exe \) -print -quit | grep -q .; then
    echo "C test artifact tree contains a benchmark executable" >&2
    exit 1
fi

rm -rf "$output"
mkdir -p "$output"
tar -czf "$output/c-test-tree.tar.gz" -C "$root" "$build_dir"
cat >"$output/manifest.txt" <<EOF
schema=1
kind=ctest-tree
preset=$preset
build_dir=$build_dir
configuration=$configuration
source_sha=$(artifact_source_sha "$root")
EOF
artifact_sha256_write "$output" c-test-tree.tar.gz manifest.txt
