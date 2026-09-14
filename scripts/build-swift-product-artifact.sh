#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/artifact.sh"
output=${1:-"$root/build/ci-artifacts/swift-product"}

cd "$root"
# The product is the release configuration: it is what ships and what any
# measurement of the Swift package must come from. The test producers keep
# their own scratch paths and configurations.
CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    swift build --target MarkdownCore -c release --disable-sandbox
rm -rf "$output"
mkdir -p "$output"
tar -czf "$output/swift-product-tree.tar.gz" .build
cat >"$output/manifest.txt" <<EOF
schema=1
kind=swift-product-tree
source_sha=$(artifact_source_sha "$root")
EOF
artifact_sha256_write "$output" swift-product-tree.tar.gz manifest.txt
