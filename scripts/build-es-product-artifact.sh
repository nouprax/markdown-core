#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output=${1:-"$root/build/ci-artifacts/es-product"}

node "$root/packages/es-markdown-core/scripts/build.mjs"
rm -rf "$output"
mkdir -p "$output"
tar -czf "$output/es-product-dist.tar.gz" -C "$root" packages/es-markdown-core/dist
cat >"$output/manifest.txt" <<EOF
schema=1
kind=es-product-dist
source_sha=${GITHUB_SHA:-$(git -C "$root" rev-parse HEAD)}
EOF
(
    cd "$output"
    sha256sum es-product-dist.tar.gz manifest.txt >SHA256SUMS
)
