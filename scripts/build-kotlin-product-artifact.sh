#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/artifact.sh"
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
source_sha=$(artifact_source_sha "$root")
EOF
artifact_sha256_write "$output" kotlin-product-publications.tar.gz manifest.txt
