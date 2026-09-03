#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/lib/artifact.sh"
target=${1:-}
output=${2:-"$root/build/ci-artifacts/kotlin-$target"}
project_build="$root/packages/kotlin-markdown-core/build"

# Gradle Sync cleans each owned leaf, but not sibling staging directories from
# a target removed from the current graph. Start the CI-only staging root from
# a known empty state so stale classes cannot enter the archive.
rm -rf "$project_build/ci-test-artifact"

case "$target" in
    linuxX64)
        "$root/scripts/gradle.sh" --console=plain --stacktrace \
            :packages:kotlin-markdown-core:stageJvmTestArtifact \
            :packages:kotlin-markdown-core:stageAndroidHostTestArtifact \
            :packages:kotlin-markdown-core:linkDebugTestLinuxX64
        classpath=$(find "$project_build/ci-test-artifact/jvm/lib" -type f -name '*.jar' -print | paste -sd: -)
        javac -cp "$classpath" \
            -d "$project_build/ci-test-artifact/jvm/classes" \
            "$root/scripts/ci/KotlinJvmTestLauncher.java"
        ;;
    macosArm64)
        "$root/scripts/gradle.sh" --console=plain --stacktrace \
            :packages:kotlin-markdown-core:linkDebugTestMacosArm64
        ;;
    *)
        echo "usage: $0 linuxX64|macosArm64 [output-dir]" >&2
        exit 2
        ;;
esac

if find "$project_build/ci-test-artifact" -iname '*benchmark*' -print -quit | grep -q .; then
    echo "Kotlin test artifact staging contains a benchmark payload" >&2
    exit 1
fi

rm -rf "$output"
mkdir -p "$output"
if [ "$target" = linuxX64 ]; then
    tar -czf "$output/kotlin-test-products.tar.gz" -C "$root" \
        packages/kotlin-markdown-core/build/ci-test-artifact \
        packages/kotlin-markdown-core/build/bin/linuxX64/debugTest/test.kexe
else
    tar -czf "$output/kotlin-test-products.tar.gz" -C "$root" \
        packages/kotlin-markdown-core/build/bin/macosArm64/debugTest/test.kexe
fi
cat >"$output/manifest.txt" <<EOF
schema=1
kind=kotlin-host-test-products
target=$target
source_sha=$(artifact_source_sha "$root")
EOF
artifact_sha256_write "$output" kotlin-test-products.tar.gz manifest.txt
