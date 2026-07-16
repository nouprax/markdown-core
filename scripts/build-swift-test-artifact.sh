#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output=${1:-"$root/build/ci-artifacts/swift"}
consumer=packages/swift-markdown-core/Tests/Consumer

cd "$root"
CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    swift build --build-tests --disable-sandbox
CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    swift build --build-tests --disable-sandbox --package-path "$consumer"
CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    swift build --disable-sandbox -c release --product MarkdownCoreBenchmarks
benchmark_bin=$(swift build --disable-sandbox -c release --show-bin-path)
rm -rf build/ci-benchmark/swift
mkdir -p build/ci-benchmark/swift
cp "$benchmark_bin/MarkdownCoreBenchmarks" build/ci-benchmark/swift/
CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    xcodebuild build-for-testing \
        -scheme swift-markdown-core-Package \
        -destination 'generic/platform=iOS Simulator' \
        -derivedDataPath build/xcode-tests

rm -rf "$output"
mkdir -p "$output"
tar -czf "$output/swift-test-products.tar.gz" \
    .build \
    "$consumer/.build" \
    build/ci-benchmark/swift \
    build/xcode-tests
cat >"$output/manifest.txt" <<EOF
schema=1
kind=swift-test-products
source_sha=${GITHUB_SHA:-$(git -C "$root" rev-parse HEAD)}
EOF
(
    cd "$output"
    shasum -a 256 swift-test-products.tar.gz manifest.txt >SHA256SUMS
)
