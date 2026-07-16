#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
artifact_dir=${1:-}
suite=${2:-}

test -d "$artifact_dir"
(
    cd "$artifact_dir"
    sha256sum --check SHA256SUMS
)
grep -Fxq 'kind=es-test-dist' "$artifact_dir/manifest.txt"
if [ -n "${GITHUB_SHA:-}" ]; then
    grep -Fxq "source_sha=$GITHUB_SHA" "$artifact_dir/manifest.txt"
fi
tar -xzf "$artifact_dir/es-dist.tar.gz" -C "$root"

case "$suite" in
    node-correctness)
        node "$root/packages/es-markdown-core/scripts/run-tests.mjs" --target node --skip-build
        ;;
    browser-correctness)
        node "$root/packages/es-markdown-core/scripts/run-tests.mjs" --target browser --skip-build
        ;;
    node-conformance)
        node "$root/packages/es-markdown-core/scripts/run-conformance.mjs" --skip-build
        ;;
    node-benchmark)
        node "$root/packages/es-markdown-core/scripts/benchmark.mjs"
        ;;
    *)
        echo "usage: $0 <artifact-dir> node-correctness|browser-correctness|node-conformance|node-benchmark" >&2
        exit 2
        ;;
esac
