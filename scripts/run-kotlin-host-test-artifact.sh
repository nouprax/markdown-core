#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
artifact_dir=${1:-}
platform=${2:-}
suite=${3:-}

case "$suite" in correctness | conformance | benchmark) ;; *) exit 2 ;; esac
(
    cd "$artifact_dir"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum --check SHA256SUMS
    else
        shasum -a 256 --check SHA256SUMS
    fi
)
grep -Fxq 'kind=kotlin-host-test-products' "$artifact_dir/manifest.txt"
if [ -n "${GITHUB_SHA:-}" ]; then
    grep -Fxq "source_sha=$GITHUB_SHA" "$artifact_dir/manifest.txt"
fi
tar -xzf "$artifact_dir/kotlin-test-products.tar.gz" -C "$root"
project_build="$root/packages/kotlin-markdown-core/build"

case "$platform" in
    linux-x64)
        executable="$project_build/bin/linuxX64/debugTest/test.kexe"
        ;;
    macos-arm64)
        executable="$project_build/bin/macosArm64/debugTest/test.kexe"
        ;;
    jvm)
        if [ "$suite" = benchmark ]; then
            benchmark="$project_build/ci-test-artifact/jvm-benchmark"
            java --enable-native-access=ALL-UNNAMED \
                -cp "$benchmark/classes:$benchmark/lib/*" \
                com.nouprax.markdown.core.benchmark.BenchmarkKt
            exit
        fi
        jvm="$project_build/ci-test-artifact/jvm"
        java --enable-native-access=ALL-UNNAMED \
            -cp "$jvm/classes:$jvm/lib/*" \
            com.nouprax.markdown.core.ci.KotlinJvmTestLauncher "$suite"
        exit
        ;;
    android-host)
        test "$suite" != benchmark
        host="$project_build/ci-test-artifact/android-host"
        mapfile -t classes < <(
            find "$host/classes/com/nouprax/markdown/core" -type f -name '*Test.class' ! -name '*$*' \
                | sed "s#^$host/classes/##; s#/#.#g; s#\.class\$##" \
                | LC_ALL=C sort
        )
        selected=()
        for class_name in "${classes[@]}"; do
            if { [ "$suite" = conformance ] && [[ "$class_name" = *.AstTest ]]; } ||
                { [ "$suite" = correctness ] && [[ "$class_name" != *.AstTest ]]; }; then
                selected+=("$class_name")
            fi
        done
        test "${#selected[@]}" -gt 0
        native=$(find "$host/native" -maxdepth 1 -type f -name 'libmarkdown_core_kotlin.so' -print -quit)
        test -n "$native"
        java --enable-native-access=ALL-UNNAMED \
            -Dmarkdown.core.hostNativeLibrary="$native" \
            -cp "$host/classes:$host/lib/*" \
            org.junit.runner.JUnitCore "${selected[@]}"
        exit
        ;;
    *)
        echo "usage: $0 <artifact-dir> jvm benchmark | <artifact-dir> linux-x64|macos-arm64|jvm|android-host correctness|conformance" >&2
        exit 2
        ;;
esac

test "$suite" != benchmark

if [ "$suite" = conformance ]; then
    "$executable" '--ktest_gradle_filter=*AstTest*'
else
    "$executable" '--ktest_negative_gradle_filter=*AstTest*'
fi
