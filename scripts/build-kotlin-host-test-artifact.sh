#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-}
output=${2:-"$root/build/ci-artifacts/kotlin-$target"}
project_build="$root/packages/kotlin-markdown-core/build"

case "$target" in
    linuxX64)
        "$root/scripts/gradle.sh" --console=plain --stacktrace \
            :packages:kotlin-markdown-core:stageJvmTestArtifact \
            :packages:kotlin-markdown-core:stageJvmBenchmarkArtifact \
            :packages:kotlin-markdown-core:stageAndroidHostTestArtifact \
            :packages:kotlin-markdown-core:jvmJar \
            :packages:kotlin-markdown-core:linkDebugTestLinuxX64
        classpath=$(find "$project_build/ci-test-artifact/jvm/lib" -type f -name '*.jar' -print | paste -sd: -)
        javac -cp "$classpath" \
            -d "$project_build/ci-test-artifact/jvm/classes" \
            "$root/scripts/ci/KotlinJvmTestLauncher.java"
        mkdir -p "$project_build/ci-test-artifact/jvm-benchmark/size"
        jvm_jar="$project_build/libs/kotlin-markdown-core-jvm-$(cat "$root/VERSION").jar"
        test -f "$jvm_jar"
        cp "$jvm_jar" "$project_build/ci-test-artifact/jvm-benchmark/size/"
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
source_sha=${GITHUB_SHA:-$(git -C "$root" rev-parse HEAD)}
EOF
(
    cd "$output"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum kotlin-test-products.tar.gz manifest.txt >SHA256SUMS
    else
        shasum -a 256 kotlin-test-products.tar.gz manifest.txt >SHA256SUMS
    fi
)
