#!/bin/sh
# Kotlin JVM coverage producer.
#
# Runs the JVM correctness and conformance suites under JaCoCo and hands the
# XML report to the repository's one coverage gate.
#
# The JVM target is the measured one because every other Kotlin target compiles
# the same commonMain sources: instrumenting one run measures the shared
# binding logic once instead of reporting each common file four times. Sources
# that exist only for Android, Native, or the JNI layer are gated by their own
# platform suites, not by this report.
#
# Pass --update-ledger through to record an improvement.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

report="packages/kotlin-markdown-core/build/reports/jacoco/jvmCoverageReport/jvmCoverageReport.xml"

# -PmarkdownCoreCoverage turns JaCoCo instrumentation on. Without it the agent
# stays detached, so the ordinary `pnpm test:kotlin-jvm` path runs exactly what
# it ran before this gate existed.
sh scripts/gradle.sh -PmarkdownCoreCoverage :packages:kotlin-markdown-core:jvmCoverageReport

if [ ! -f "$report" ]; then
    echo "coverage: Gradle produced no report at $report." >&2
    exit 1
fi

exec node scripts/check-coverage.mjs \
    --platform kotlin-jvm \
    --format jacoco \
    --input "$report" \
    --source-root packages/kotlin-markdown-core/src/commonMain/kotlin \
    --source-root packages/kotlin-markdown-core/src/jvmMain/kotlin \
    "$@"
