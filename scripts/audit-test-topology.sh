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

# 2. No runtime network dependency in build/test/bench plumbing.  The only
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

# 4. CTest topology: configure/build if needed, then cross-check labels and
# per-case registrations against runner discovery.
BUILD_DIR=build/cmake
if [ ! -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
    cmake --preset default >/dev/null
    cmake --build --preset default --parallel >/dev/null
fi

tests_all=$("ctest" --test-dir "$BUILD_DIR" -N | sed -n 's/^  Test *#[0-9]*: //p')
for label in api facade conformance consumer spec equivalence extensions regression pathological complexity fuzz packaging benchmark; do
    count=$(ctest --test-dir "$BUILD_DIR" -N -L "^${label}$" | sed -n 's/^Total Tests: //p')
    if [ "${count:-0}" -lt 1 ]; then
        fail "no CTest tests carry label '$label'"
    fi
done
note "every required label resolves to at least one test"

if ctest --test-dir "$BUILD_DIR" -N | grep -q 'Disabled'; then
    fail "disabled tests present in the CTest graph"
else
    note "no disabled tests in the CTest graph"
fi

correctness_list=$(ctest --test-dir "$BUILD_DIR" -N -LE '^(benchmark|conformance)$' | sed -n 's/^  Test *#[0-9]*: //p')
if echo "$correctness_list" | grep -Eq '^(benchmark_|facade_native$|facade_dump_cli$)'; then
    fail "correctness selection includes conformance or benchmark workloads"
else
    note "correctness selection excludes conformance and benchmark"
fi

for preset in correctness-asan correctness-ubsan correctness-tsan; do
    if ctest --preset "$preset" -N | grep -q 'pathological_complexity_'; then
        fail "$preset includes wall-clock complexity gates"
    fi
done
note "sanitizer presets exclude wall-clock complexity gates"

conformance_list=$(ctest --test-dir "$BUILD_DIR" -N -L '^conformance$' | sed -n 's/^  Test *#[0-9]*: //p')
if [ "$conformance_list" != "facade_native
facade_dump_cli" ]; then
    fail "C conformance selection does not contain exactly the public contract checks"
else
    note "C conformance selection is isolated from correctness"
fi
benchmark_list=$(ctest --test-dir "$BUILD_DIR" -N -L '^benchmark$' | sed -n 's/^  Test *#[0-9]*: //p')
if echo "$benchmark_list" | grep -v '^benchmark_' | grep -q .; then
    fail "benchmark selection includes non-benchmark tests"
else
    note "benchmark selection contains only benchmark workloads"
fi

runner_dir="$BUILD_DIR/packages/markdown-core/tests"
for case_name in $("$runner_dir/pathological_runner" --list); do
    echo "$tests_all" | grep -q "^pathological_${case_name}$" \
        || fail "pathological case '$case_name' is not registered in CTest"
done
for case_name in $("$runner_dir/complexity_runner" --list); do
    echo "$tests_all" | grep -q "^pathological_complexity_${case_name}$" \
        || fail "complexity case '$case_name' is not registered in CTest"
done
for case_name in $("$runner_dir/stress_runner" --list); do
    echo "$tests_all" | grep -q "^pathological_stress_${case_name}$" \
        || fail "stress case '$case_name' is not registered in CTest"
done
for case_name in $("$runner_dir/fallback_runner" --list); do
    echo "$tests_all" | grep -q "^regression_fallback_${case_name}$" \
        || fail "fallback case '$case_name' is not registered in CTest"
done
for case_name in $("$runner_dir/equivalence_runner" --list); do
    echo "$tests_all" | grep -q "^equivalence_${case_name}$" \
        || fail "equivalence case '$case_name' is not registered in CTest"
done
for workload in $("$runner_dir/bench_runner" --list); do
    echo "$tests_all" | grep -q "^benchmark_${workload}$" \
        || fail "benchmark workload '$workload' is not registered in CTest"
done
note "runner discovery matches CTest registration"

# 5. Swift suites must be real (not build-only) when a toolchain is present;
# CI additionally executes them on the Swift platform job.
if command -v swift >/dev/null 2>&1; then
    if [ "$(CLANG_MODULE_CACHE_PATH="$BUILD_DIR/swift-module-cache" \
        swift test --disable-sandbox list 2>/dev/null | wc -l)" -lt 1 ]; then
        fail "swift test discovers no Swift Testing suites"
    else
        note "swift test discovers Swift Testing suites"
    fi
fi

if [ "$failures" -gt 0 ]; then
    echo "$failures test topology violation(s)" >&2
    exit 1
fi
echo "test topology audit passed"
