#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
variant=${1:?usage: build-c-product-artifact.sh VARIANT ON|OFF OUTPUT_DIRECTORY [CONFIGURATION]}
shared=${2:?usage: build-c-product-artifact.sh VARIANT ON|OFF OUTPUT_DIRECTORY [CONFIGURATION]}
output=${3:?usage: build-c-product-artifact.sh VARIANT ON|OFF OUTPUT_DIRECTORY [CONFIGURATION]}
configuration=${4:-Release}
build_dir=build/cmake

cmake -S "$root" -B "$root/$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMARKDOWN_CORE_TESTS=OFF \
    -DMARKDOWN_CORE_SHARED="$shared" \
    -DMARKDOWN_CORE_WARNINGS_AS_ERRORS=ON
cmake --build "$root/$build_dir" --config "$configuration" --parallel

rm -rf "$output"
mkdir -p "$output"
tar -czf "$output/c-product-tree.tar.gz" -C "$root" "$build_dir"
cat >"$output/manifest.txt" <<EOF
schema=1
kind=c-product-tree
variant=$variant
shared=$shared
configuration=$configuration
source_sha=${GITHUB_SHA:-$(git -C "$root" rev-parse HEAD)}
EOF
(
    cd "$output"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum c-product-tree.tar.gz manifest.txt >SHA256SUMS
    else
        shasum -a 256 c-product-tree.tar.gz manifest.txt >SHA256SUMS
    fi
)
