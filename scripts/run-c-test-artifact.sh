#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/artifact.sh"
artifact_dir=${1:-}
test_preset=${2:-}
configuration=${3:-}

test -d "$artifact_dir"
case "$test_preset" in
    correctness | conformance | correctness-asan | correctness-ubsan | correctness-tsan) ;;
    *)
        echo "usage: $0 <artifact-dir> <ctest-preset> [configuration]" >&2
        exit 2
        ;;
esac

artifact_verify "$artifact_dir" ctest-tree
artifact_extract "$artifact_dir" c-test-tree.tar.gz "$root"

command=(ctest --preset "$test_preset" --output-on-failure)
if [ -n "$configuration" ]; then
    command+=(-C "$configuration")
fi
"${command[@]}"
