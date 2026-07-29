#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/artifact.sh"
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
source_sha=$(artifact_source_sha "$root")
EOF
artifact_sha256_write "$output" c-product-tree.tar.gz manifest.txt
