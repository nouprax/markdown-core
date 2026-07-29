#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/artifact.sh"
output=${1:-"$root/build/ci-artifacts/swift"}
consumer=packages/swift-markdown-core/Tests/Consumer

cd "$root"
CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    swift build --build-tests --disable-sandbox
# The repository-wide topology audit runs on a Linux runner without a Swift
# toolchain, so the producer itself must assert the built test products
# discover a non-empty Swift Testing graph (mirroring the CTest inventory
# assertion in build-c-test-artifact.sh).
swift_test_list=$(CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    swift test --disable-sandbox list)
if [ -z "$swift_test_list" ]; then
    echo "Swift test artifact discovers no Swift Testing suites" >&2
    exit 1
fi
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
source_sha=$(artifact_source_sha "$root")
EOF
artifact_sha256_write "$output" swift-test-products.tar.gz manifest.txt
