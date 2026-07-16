#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:?usage: build-kotlin-product-artifact.sh linux-release|macos-native OUTPUT_DIRECTORY}
output=${2:?usage: build-kotlin-product-artifact.sh linux-release|macos-native OUTPUT_DIRECTORY}
stage="$root/build/ci-product/kotlin-$mode"

case "$mode" in
    linux-release | macos-native) ;;
    *) echo "unsupported Kotlin product mode: $mode" >&2; exit 2 ;;
esac

"$root/scripts/stage-maven-publications.sh" "$stage" "$mode"
rm -rf "$output"
mkdir -p "$output"
tar -czf "$output/kotlin-product-publications.tar.gz" -C "$stage" .
cat >"$output/manifest.txt" <<EOF
schema=1
kind=kotlin-product-publications
mode=$mode
source_sha=${GITHUB_SHA:-$(git -C "$root" rev-parse HEAD)}
EOF
(
    cd "$output"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum kotlin-product-publications.tar.gz manifest.txt >SHA256SUMS
    else
        shasum -a 256 kotlin-product-publications.tar.gz manifest.txt >SHA256SUMS
    fi
)
