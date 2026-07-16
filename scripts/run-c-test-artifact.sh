#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
artifact_dir=${1:-}
test_preset=${2:-}
configuration=${3:-}

test -d "$artifact_dir"
case "$test_preset" in
    correctness | conformance | benchmark | correctness-asan | correctness-ubsan | correctness-tsan) ;;
    *)
        echo "usage: $0 <artifact-dir> <ctest-preset> [configuration]" >&2
        exit 2
        ;;
esac

(
    cd "$artifact_dir"
    sha256sum --check SHA256SUMS
)
grep -Fxq 'kind=ctest-tree' "$artifact_dir/manifest.txt"
if [ -n "${GITHUB_SHA:-}" ]; then
    grep -Fxq "source_sha=$GITHUB_SHA" "$artifact_dir/manifest.txt"
fi
tar -xzf "$artifact_dir/c-test-tree.tar.gz" -C "$root"

command=(ctest --preset "$test_preset" --output-on-failure)
if [ "$test_preset" = benchmark ]; then
    command+=(--verbose)
fi
if [ -n "$configuration" ]; then
    command+=(-C "$configuration")
fi
"${command[@]}"
