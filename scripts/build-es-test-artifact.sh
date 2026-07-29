#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/artifact.sh"
output=${1:-"$root/build/ci-artifacts/es"}
product=${2:-}

if [ -n "$product" ]; then
    artifact_verify "$product" es-product-dist
    artifact_extract "$product" es-product-dist.tar.gz "$root"
else
    node "$root/packages/es-markdown-core/scripts/build.mjs"
fi
node "$root/packages/es-markdown-core/scripts/bundle-conformance-fixtures.mjs"
rm -rf "$output"
mkdir -p "$output"
tar -czf "$output/es-dist.tar.gz" -C "$root" \
    packages/es-markdown-core/dist \
    packages/es-markdown-core/build/generated/conformance
cat >"$output/manifest.txt" <<EOF
schema=1
kind=es-test-dist
source_sha=$(artifact_source_sha "$root")
EOF
artifact_sha256_write "$output" es-dist.tar.gz manifest.txt
