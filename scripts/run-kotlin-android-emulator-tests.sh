#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
gradle="$root/scripts/gradle.sh"

mode=${1:-}
device=${2:-all}

case "$mode" in
    correctness)
        filter="-Pandroid.testInstrumentationRunnerArguments.notClass=com.nouprax.markdown.core.AstTest"
        ;;
    conformance)
        filter="-Pandroid.testInstrumentationRunnerArguments.class=com.nouprax.markdown.core.AstTest"
        ;;
    *)
        echo "usage: $0 correctness|conformance [4k|16k|all]" >&2
        exit 2
        ;;
esac

case "$device" in
    4k)
        tasks=(
            :packages:kotlin-markdown-core:markdownCoreApi36Page4kAndroidDeviceTest
        )
        ;;
    16k)
        tasks=(
            :packages:kotlin-markdown-core:markdownCoreApi36Page16kAndroidDeviceTest
        )
        ;;
    all)
        tasks=(
            :packages:kotlin-markdown-core:markdownCoreApi36Page4kAndroidDeviceTest
            :packages:kotlin-markdown-core:markdownCoreApi36Page16kAndroidDeviceTest
        )
        ;;
    *)
        echo "usage: $0 correctness|conformance [4k|16k|all]" >&2
        exit 2
        ;;
esac

cd "$root"
for task in "${tasks[@]}"; do
    started_at=$SECONDS
    printf 'Starting Android %s task %s\n' "$mode" "$task"
    if "$gradle" --console=plain --stacktrace "$task" "$filter"; then
        printf 'Finished Android %s task %s in %ss\n' \
            "$mode" "$task" "$((SECONDS - started_at))"
    else
        status=$?
        printf 'Failed Android %s task %s after %ss\n' \
            "$mode" "$task" "$((SECONDS - started_at))" >&2
        exit "$status"
    fi
done
