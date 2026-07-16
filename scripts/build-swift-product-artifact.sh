#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output=${1:-"$root/build/ci-artifacts/swift-product"}

cd "$root"
CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    swift build --target MarkdownCore --disable-sandbox
rm -rf "$output"
mkdir -p "$output"
tar -czf "$output/swift-product-tree.tar.gz" .build
cat >"$output/manifest.txt" <<EOF
schema=1
kind=swift-product-tree
source_sha=${GITHUB_SHA:-$(git -C "$root" rev-parse HEAD)}
EOF
(
    cd "$output"
    shasum -a 256 swift-product-tree.tar.gz manifest.txt >SHA256SUMS
)
