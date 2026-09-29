#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/scripts/shared/artifact.sh"
artifact_dir=${1:-}
test_preset=${2:-}
configuration=${3:-}

test -d "$artifact_dir"
case "$test_preset" in
    correctness | conformance | incremental | correctness-debug | correctness-asan | correctness-ubsan | correctness-tsan) ;;
    *)
        echo "usage: $0 <artifact-dir> <ctest-preset> [configuration]" >&2
        exit 2
        ;;
esac

artifact_verify "$artifact_dir" ctest-tree
artifact_extract "$artifact_dir" c-test-tree.tar.gz "$root"

command=(ctest --preset "$test_preset" --output-on-failure)
# The incremental set is one independent test per family, so it runs them
# side by side on every processor of the runner.
if [ "$test_preset" = incremental ]; then
    command+=(--parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)")
fi
if [ -n "$configuration" ]; then
    command+=(-C "$configuration")
fi
"${command[@]}"
