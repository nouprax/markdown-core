#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/artifact.sh"
artifact_dir=${1:-}
suite=${2:-}

artifact_verify "$artifact_dir" es-test-dist
artifact_extract "$artifact_dir" es-dist.tar.gz "$root"

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
    *)
        echo "usage: $0 <artifact-dir> node-correctness|browser-correctness|node-conformance" >&2
        exit 2
        ;;
esac
