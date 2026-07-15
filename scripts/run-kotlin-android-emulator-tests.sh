#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
gradle="$root/scripts/gradle.sh"

case "${1:-}" in
    correctness)
        filter="-Pandroid.testInstrumentationRunnerArguments.notClass=com.nouprax.markdown.core.AstTest"
        ;;
    conformance)
        filter="-Pandroid.testInstrumentationRunnerArguments.class=com.nouprax.markdown.core.AstTest"
        ;;
    *)
        echo "usage: $0 correctness|conformance" >&2
        exit 2
        ;;
esac

cd "$root"
for task in \
    :packages:kotlin-markdown-core:markdownCoreApi36Page4kAndroidDeviceTest \
    :packages:kotlin-markdown-core:markdownCoreApi36Page16kAndroidDeviceTest
do
    "$gradle" "$task" "$filter"
done
