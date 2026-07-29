#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/artifact.sh"
output=${1:-"$root/build/ci-artifacts/kotlin-android-x86_64"}
apk_source="$root/packages/kotlin-markdown-core/build/outputs/apk/androidTest/kotlin-markdown-core-androidTest.apk"
apk_name=kotlin-markdown-core-androidTest.apk

rm -rf "$output"
mkdir -p "$output"

"$root/scripts/gradle.sh" \
    --console=plain \
    --stacktrace \
    :packages:kotlin-markdown-core:packageAndroidDeviceTest \
    -PmarkdownCore.android.abis=x86_64

test -f "$apk_source"
cp "$apk_source" "$output/$apk_name"

native_entries=$(unzip -Z1 "$output/$apk_name" | sed -n 's#^lib/\([^/]*\)/.*\.so$#\1#p' | LC_ALL=C sort -u)
if [ "$native_entries" != x86_64 ]; then
    printf 'Android test artifact must contain only x86_64 native code; found:\n%s\n' \
        "$native_entries" >&2
    exit 1
fi

source_sha=$(artifact_source_sha "$root")
cat >"$output/manifest.txt" <<EOF
schema=1
kind=android-instrumentation-apk
source_sha=$source_sha
abi=x86_64
apk=$apk_name
package=com.nouprax.markdown.core.test
runner=androidx.test.runner.AndroidJUnitRunner
EOF

artifact_sha256_write "$output" "$apk_name" manifest.txt

printf 'Staged Android test artifact at %s\n' "$output"
