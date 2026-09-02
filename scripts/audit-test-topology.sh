#!/bin/sh
# Test coverage and corpus audit.
#
# Verifies externally meaningful coverage boundaries: every binding consumes
# the shared conformance contract, tests do not fetch mutable inputs at runtime,
# vendored corpora are integrity checked, and CTest selections discover the
# workloads they claim to run.
set -eu

failures=0
fail() {
    echo "FAIL: $1" >&2
    failures=$((failures + 1))
}
note() {
    echo "ok: $1"
}

# 1. Every platform consumes the shared conformance contract.
if [ ! -f specs/canonical-ast/manifest.json ]; then
    fail "root shared canonical AST manifest is missing"
fi
if ! grep -q 'specs/canonical-ast' packages/markdown-core/tests/CMakeLists.txt \
    || ! grep -q 'plugins: \[.plugin(name: "GenerateCanonicalASTResources")\]' Package.swift \
    || ! grep -q 'specs/canonical-ast' packages/swift-markdown-core/Plugins/GenerateCanonicalASTResources/plugin.swift \
    || ! grep -q 'GenerateCanonicalAstFixtures' packages/kotlin-markdown-core/build.gradle.kts \
    || ! grep -q 'bundle:conformance-fixtures' packages/es-markdown-core/package.json \
    || ! grep -q 'specs/canonical-ast' packages/es-markdown-core/scripts/bundle-conformance-fixtures.mjs; then
    fail "one or more native conformance targets do not consume the shared canonical AST spec"
else
    note "C, SwiftPM plugin, Gradle task, and ES package lifecycle consume the shared spec"
fi

# 2. No runtime network dependency in build/test/local-benchmark plumbing. The only
# allowed network use is the explicit `update-spec` maintenance target.
if grep -n 'git clone' Makefile package.json CMakePresets.json \
    packages/markdown-core/tests/CMakeLists.txt 2>/dev/null; then
    fail "runtime git clone found in build/test plumbing"
else
    note "no runtime clone in build/test plumbing"
fi
if grep -n -E 'curl|wget' Makefile | grep -v 'update-spec' | grep -v "^[0-9]*:update-spec" \
    | grep -v 'raw.githubusercontent.com/jgm/CommonMark' >/dev/null; then
    fail "network fetch outside the update-spec maintenance target"
else
    note "network fetch limited to explicit maintenance"
fi

# 3. Vendored corpora must be manifested, licensed, and hash-verified.
if [ -d packages/markdown-core/tests/corpora ]; then
    for corpus in packages/markdown-core/tests/corpora/*/; do
        [ -d "$corpus" ] || continue
        for required in MANIFEST.json LICENSE SHA256SUMS; do
            if [ ! -f "$corpus$required" ]; then
                fail "corpus $corpus is missing $required"
            fi
        done
        if [ -f "$corpus/SHA256SUMS" ]; then
            (cd "$corpus" && shasum -a 256 -c SHA256SUMS >/dev/null) \
                || fail "corpus $corpus fails checksum verification"
        fi
    done
    note "vendored corpora are manifested and verified"
fi

# 4. Source audit stays compilation-free. Dynamic CTest inventory is evaluated
# against an already-built artifact tree when the producer passes its path and
# optional multi-config configuration; the repository health-check must not
# perform a duplicate platform build.
BUILD_DIR=${1:-}
CTEST_CONFIGURATION=${2:-}
if [ -n "$BUILD_DIR" ]; then
    if [ ! -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
        fail "requested CTest tree is not configured: $BUILD_DIR"
    else
        ctest_inventory() {
            if [ -n "$CTEST_CONFIGURATION" ]; then
                ctest --test-dir "$BUILD_DIR" -C "$CTEST_CONFIGURATION" "$@"
            else
                ctest --test-dir "$BUILD_DIR" "$@"
            fi
        }
        normalize_lines() {
            tr -d '\r'
        }

        tests_all=$(ctest_inventory -N | normalize_lines | sed -n 's/^  Test *#[0-9]*: //p')
        for label in api facade conformance consumer spec extensions regression pathological fuzz packaging; do
            count=$(ctest_inventory -N -L "^${label}$" | normalize_lines | sed -n 's/^Total Tests: //p')
            if [ "${count:-0}" -lt 1 ]; then
                fail "no CTest tests carry label '$label'"
            fi
        done
        note "every required label resolves to at least one test"

        if ctest_inventory -N | normalize_lines | grep -q 'Disabled'; then
            fail "disabled tests present in the CTest graph"
        else
            note "no disabled tests in the CTest graph"
        fi

        correctness_list=$(ctest_inventory -N -LE '^conformance$' | normalize_lines \
            | sed -n 's/^  Test *#[0-9]*: //p')
        if echo "$correctness_list" | grep -Eq '^(facade_native$|facade_dump_cli$)'; then
            fail "correctness selection includes conformance workloads"
        else
            note "correctness selection excludes conformance"
        fi

        if ctest_inventory -N -L '^(benchmark|complexity)$' | normalize_lines \
            | grep -Eq 'Total Tests: [1-9]'; then
            fail "default CTest graph contains a wall-clock performance test"
        else
            note "default CTest graph contains no wall-clock performance tests"
        fi

        conformance_list=$(ctest_inventory -N -L '^conformance$' | normalize_lines \
            | sed -n 's/^  Test *#[0-9]*: //p')
        if [ "$conformance_list" != "facade_native
facade_dump_cli" ]; then
            fail "C conformance selection does not contain exactly the public contract checks"
        else
            note "C conformance selection is isolated from correctness"
        fi

        runner_dir="$BUILD_DIR/packages/markdown-core/tests"
        pathological_runner=$(find "$runner_dir" -maxdepth 2 -type f \
            \( -name pathological_runner -o -name pathological_runner.exe \) -print -quit)
        stress_runner=$(find "$runner_dir" -maxdepth 2 -type f \
            \( -name stress_runner -o -name stress_runner.exe \) -print -quit)
        if [ -z "$pathological_runner" ] || [ -z "$stress_runner" ]; then
            fail "CTest discovery runners are missing from the built tree"
        else
            for case_name in $("$pathological_runner" --list | normalize_lines); do
                echo "$tests_all" | grep -q "^pathological_${case_name}$" \
                    || fail "pathological case '$case_name' is not registered in CTest"
            done
            for case_name in $("$stress_runner" --list | normalize_lines); do
                echo "$tests_all" | grep -q "^pathological_stress_${case_name}$" \
                    || fail "stress case '$case_name' is not registered in CTest"
            done
        fi
        echo "$tests_all" | grep -q '^regression_strict_oom$' \
            || fail "strict OOM case is not registered in CTest"
        for concurrency_case in facade_concurrent_first_parse facade_concurrent_stress regression_instance_lifecycle; do
            echo "$tests_all" | grep -q "^${concurrency_case}$" \
                || fail "multi-instance concurrency case '$concurrency_case' is not registered in CTest"
        done
        note "runner discovery, strict OOM, and multi-instance concurrency match CTest registration"
    fi
else
    note "dynamic CTest inventory deferred to the C test-artifact producer"
fi

# 5. The Swift producer performs toolchain-backed discovery after building.
# The repository-only audit checks that both owned test targets declare tests.
if grep -R -q '@Test' packages/swift-markdown-core/Tests/MarkdownCoreTests \
    && grep -R -q '@Test' packages/swift-markdown-core/Tests/MarkdownCoreConformanceTests; then
    note "Swift correctness and conformance targets declare Swift Testing tests"
else
    fail "Swift correctness or conformance target declares no Swift Testing tests"
fi

if [ "$failures" -gt 0 ]; then
    echo "$failures test topology violation(s)" >&2
    exit 1
fi
echo "test topology audit passed"
