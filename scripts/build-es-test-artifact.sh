#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output=${1:-"$root/build/ci-artifacts/es"}
product=${2:-}

if [ -n "$product" ]; then
    test -d "$product"
    (
        cd "$product"
        sha256sum --check SHA256SUMS
    )
    grep -Fxq 'kind=es-product-dist' "$product/manifest.txt"
    if [ -n "${GITHUB_SHA:-}" ]; then
        grep -Fxq "source_sha=$GITHUB_SHA" "$product/manifest.txt"
    fi
    tar -xzf "$product/es-product-dist.tar.gz" -C "$root"
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
source_sha=${GITHUB_SHA:-$(git -C "$root" rev-parse HEAD)}
EOF
(
    cd "$output"
    sha256sum es-dist.tar.gz manifest.txt >SHA256SUMS
)
