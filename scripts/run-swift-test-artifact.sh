#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
artifact_dir=${1:-}
suite=${2:-}
consumer=packages/swift-markdown-core/Tests/Consumer

test -d "$artifact_dir"
(
    cd "$artifact_dir"
    shasum -a 256 --check SHA256SUMS
)
grep -Fxq 'kind=swift-test-products' "$artifact_dir/manifest.txt"
if [ -n "${GITHUB_SHA:-}" ]; then
    grep -Fxq "source_sha=$GITHUB_SHA" "$artifact_dir/manifest.txt"
fi
tar -xzf "$artifact_dir/swift-test-products.tar.gz" -C "$root"
cd "$root"

run_ios_suite() {
    local test_target=$1
    local destination
    local udid
    local status=0
    destination=$(scripts/prepare-swift-ios-simulator.sh)
    udid=${destination##*=}
    xcodebuild test-without-building \
        -scheme swift-markdown-core-Package \
        -destination "$destination" \
        -derivedDataPath build/xcode-tests \
        "-only-testing:$test_target" || status=$?
    xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true
    return "$status"
}

case "$suite" in
    macos-correctness)
        swift test --skip-build --disable-sandbox --filter '^MarkdownCoreTests\.'
        swift test --skip-build --disable-sandbox --package-path "$consumer"
        ;;
    macos-conformance)
        swift test --skip-build --disable-sandbox --filter '^MarkdownCoreConformanceTests\.'
        ;;
    ios-correctness)
        run_ios_suite MarkdownCoreTests
        ;;
    ios-conformance)
        run_ios_suite MarkdownCoreConformanceTests
        ;;
    macos-benchmark)
        build/ci-benchmark/swift/MarkdownCoreBenchmarks
        ;;
    *)
        echo "usage: $0 <artifact-dir> macos-correctness|macos-conformance|ios-correctness|ios-conformance|macos-benchmark" >&2
        exit 2
        ;;
esac
