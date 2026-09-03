#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/artifact.sh"
output=${1:-"$root/build/ci-artifacts/swift"}
consumer=packages/swift-markdown-core/Tests/Consumer
root_scratch="$root/build/ci-swift-tests/root"
consumer_scratch="$root/build/ci-swift-tests/consumer"

cd "$root"
rm -rf build/ci-swift-tests build/xcode-tests
CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    swift build --build-tests --disable-sandbox --scratch-path "$root_scratch"
# The repository-wide test-layout audit runs on a Linux runner without a Swift
# toolchain, so the producer itself must assert the built test products
# discover a non-empty Swift Testing graph (mirroring the CTest inventory
# assertion in build-c-test-artifact.sh).
swift_test_list=$(CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    swift test --disable-sandbox --scratch-path "$root_scratch" list)
if [ -z "$swift_test_list" ]; then
    echo "Swift test artifact discovers no Swift Testing suites" >&2
    exit 1
fi
CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    swift build --build-tests --disable-sandbox --package-path "$consumer" --scratch-path "$consumer_scratch"
CLANG_MODULE_CACHE_PATH="$root/build/swift-module-cache" \
    xcodebuild build-for-testing \
        -scheme swift-markdown-core-Package \
        -destination 'generic/platform=iOS Simulator' \
        -derivedDataPath build/xcode-tests

if find build/ci-swift-tests build/xcode-tests -iname '*benchmark*' -print -quit | grep -q .; then
    echo "Swift test artifact staging contains a benchmark payload" >&2
    exit 1
fi

rm -rf "$output"
mkdir -p "$output"
tar -czf "$output/swift-test-products.tar.gz" \
    build/ci-swift-tests/root \
    build/ci-swift-tests/consumer \
    build/xcode-tests
cat >"$output/manifest.txt" <<EOF
schema=1
kind=swift-test-products
source_sha=$(artifact_source_sha "$root")
EOF
artifact_sha256_write "$output" swift-test-products.tar.gz manifest.txt
