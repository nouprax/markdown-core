#!/bin/sh
# Test topology and architecture audit.
#
# Verifies that the frozen Phase 7 test architecture still holds: CTest is
# the single C suite graph, labels and runner case lists agree, pnpm entries
# only route to native runners, no scripting-language test runners or
# runtime network/corpus dependencies exist, and vendored corpora (if any)
# are manifested, licensed, and hash-verified.
set -eu

failures=0
fail() {
    echo "FAIL: $1" >&2
    failures=$((failures + 1))
}
note() {
    echo "ok: $1"
}

# 1. Package ownership: no root test tree; the only cross-package test data is
# the runner-free canonical AST contract under root specs/.
if [ -e tests ]; then
    fail "root tests/ exists; every test must be owned by one package"
else
    note "root tests/ is absent"
fi
for legacy_setup in spm swift wrappers; do
    if [ -d "$legacy_setup" ] && find "$legacy_setup" -type f | grep -q .; then
        fail "retired root setup remains: $legacy_setup"
    fi
done
if git ls-files android | while IFS= read -r path; do [ -e "$path" ] && echo "$path"; done \
    | grep -q .; then
    fail "retired root Android/Prefab sources remain"
else
    note "legacy Swift, wrapper, and Android/Prefab setup is absent"
fi
if find packages/markdown-core/tests -name '*.py' | grep -q .; then
    fail "Python files present under test directories"
else
    note "test directories contain no Python runners"
fi
canonical_locations=$(find packages -type d -name canonical-ast -print)
if [ -n "$canonical_locations" ]; then
    fail "package-local canonical AST corpus exists: $canonical_locations"
elif [ ! -f specs/canonical-ast/manifest.json ]; then
    fail "root shared canonical AST manifest is missing"
else
    note "root specs/canonical-ast is the only canonical AST corpus"
fi
if find specs/canonical-ast -type f \
    ! -name 'README.md' ! -name 'manifest.json' ! -name '*.md' ! -name '*.ast' | grep -q .; then
    fail "root canonical AST spec contains a runner or unsupported file type"
else
    note "root canonical AST spec contains contract data only"
fi
if find specs -type f \( -name '*.c' -o -name '*.h' -o -name '*.swift' -o -name '*.kt' \
    -o -name '*.mjs' -o -name '*.js' -o -name '*.ts' -o -name '*.sh' \) | grep -q .; then
    fail "root specs/ contains executable source"
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
if grep -q 'specs/canonical-ast' packages/es-markdown-core/tests/conformance.test.mjs \
    || grep -q 'specs/canonical-ast' packages/swift-markdown-core/Tests/MarkdownCoreConformanceTests/ConformanceSuite.swift; then
    fail "a binding conformance test bypasses its package build-tool output"
else
    note "binding tests consume only build-tool-derived fixtures"
fi
if grep -R -n 'Document scope=' \
    packages/swift-markdown-core/Tests/MarkdownCoreConformanceTests \
    packages/kotlin-markdown-core/src/commonTest/kotlin/com/nouprax/markdown/core/AstTest.kt \
    packages/es-markdown-core/tests/conformance.test.mjs >/dev/null; then
    fail "a binding conformance test retains a package-local expected tree literal"
else
    note "binding conformance tests retain no second expected-tree list"
fi
for owned in \
    packages/swift-markdown-core/Tests/Consumer \
    packages/kotlin-markdown-core/consumers \
    packages/kotlin-markdown-core/contracts \
    packages/kotlin-markdown-core/android-runtime \
    packages/es-markdown-core/tests; do
    if [ ! -d "$owned" ]; then
        fail "missing package-owned test layout: $owned"
    fi
done
swift_consumer=packages/swift-markdown-core/Tests/Consumer
if { [ -d "$swift_consumer/Sources" ] \
    && find "$swift_consumer/Sources" -type f | grep -q .; } \
    || grep -q 'executableTarget' "$swift_consumer/Package.swift"; then
    fail "Swift consumer uses a dummy executable instead of directly consuming the public product"
elif ! grep -q 'testTarget' "$swift_consumer/Package.swift" \
    || ! grep -q 'product(name: "MarkdownCore"' "$swift_consumer/Package.swift"; then
    fail "Swift consumer test target does not directly depend on the public MarkdownCore product"
else
    note "Swift consumer directly tests the public product without a dummy executable"
fi
if [ -e packages/kotlin-markdown-core-android-native ]; then
    fail "retired sibling Kotlin Android native module still exists"
else
    note "Kotlin Android runtime is nested under its owning package"
fi

# 2. No Python or downgrade-skip plumbing in the CTest graph.
if grep -R -n -E 'find_package\(Python|PYTHON_EXECUTABLE|doctest|skipping_spectests' \
    packages/markdown-core/tests/CMakeLists.txt >/dev/null; then
    fail "test CMakeLists still references Python or skip plumbing"
else
    note "CTest graph has no Python dependency and no skip branch"
fi

# 3. No unmanaged corpus state, and .gitignore must not hide any.
for path in progit alltests.md packages/markdown-core/benchmarks/benchinput.md; do
    if [ -e "$path" ]; then
        fail "unmanaged corpus state present: $path"
    fi
done
if grep -n -E 'progit|benchinput|alltests' .gitignore >/dev/null; then
    fail ".gitignore hides retired corpus paths"
else
    note "workspace and .gitignore are free of retired corpus traces"
fi

# 4. No runtime network dependency in build/test/bench plumbing.  The only
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

# 5. Vendored corpora must be manifested, licensed, and hash-verified.
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

# 6. Root routing stops at real execution platforms. Correctness,
# conformance, and benchmark are independent lanes; native runners own suites.
if ! node <<'NODE'
import fs from "node:fs";

const manifest = JSON.parse(fs.readFileSync("package.json", "utf8"));
const scripts = manifest.scripts;
const families = {
    test: ["c-host", "swift-macos", "swift-ios-simulator", "kotlin-jvm", "kotlin-android-host", "kotlin-android-emulator", "kotlin-macos-arm64", "kotlin-linux-x64", "es-node", "es-browser"],
    conformance: ["c-host", "swift-macos", "swift-ios-simulator", "kotlin-jvm", "kotlin-android-host", "kotlin-android-emulator", "kotlin-macos-arm64", "kotlin-linux-x64", "es-node"],
    benchmark: ["c-host", "swift-macos", "kotlin-jvm", "es-node"]
};
const expectedNames = new Set();
for (const [family, platforms] of Object.entries(families)) {
    for (const platform of platforms) expectedNames.add(`${family}:${platform}`);
    if (scripts[family]) throw new Error(`${family} aggregate hides incompatible platform targets`);
}
for (const name of Object.keys(scripts)) {
    if (/^(test|conformance|benchmark):/.test(name) && !expectedNames.has(name)) {
        throw new Error(`suite-level or obsolete routing task remains: ${name}`);
    }
}
if (Object.keys(scripts).some((name) => /^(bench|stress)(:|$)/.test(name)))
    throw new Error("bench/stress aliases must not be public task families");
if (fs.existsSync("scripts/run-test-suite.mjs")) throw new Error("generic family router still exists");
NODE
then
    fail "pnpm task families drifted from the platform-target contract"
else
    note "pnpm exposes only independent test/conformance/benchmark platform targets"
fi

check_script() {
    name=$1
    expected=$2
    actual=$(node -e "process.stdout.write(require('./package.json').scripts[process.argv[1]] ?? '')" "$name")
    if [ "$actual" != "$expected" ]; then
        fail "pnpm script '$name' drifted from the frozen routing contract"
    fi
}

check_script benchmark:c-host "cmake --preset default && cmake --build --preset default --parallel && ctest --preset benchmark"
check_script clean:kotlin-android-emulator "scripts/gradle.sh :packages:kotlin-markdown-core:cleanManagedDevices"
check_script conformance:c-host "cmake --preset default && cmake --build --preset default --parallel && ctest --preset conformance"
check_script test:c-host "cmake --preset default && cmake --build --preset default --parallel && ctest --preset correctness"
check_script conformance:es-node "pnpm --dir packages/es-markdown-core run conformance"
note "platform tasks delegate directly to named native targets"

if grep -qE 'finalizedBy\([^)]*cleanManagedDevices|dependsOn\([^)]*cleanManagedDevices' \
    packages/kotlin-markdown-core/build.gradle.kts; then
    fail "Android tests automatically destroy the managed-device cache"
else
    note "managed-device cleanup remains an explicit maintenance task"
fi

node packages/es-markdown-core/scripts/run-tests.mjs --target node --list \
    | grep -q '^conformance$' && fail "ES correctness discovery includes conformance"
node packages/es-markdown-core/scripts/run-tests.mjs --target node --list \
    | grep -q '^robustness$' || fail "ES correctness discovery omits robustness"
grep -q 'tests/conformance.test.mjs' packages/es-markdown-core/scripts/run-conformance.mjs \
    || fail "ES conformance target is not isolated"
node <<'NODE' || fail "ES package lifecycle does not generate conformance fixtures"
import fs from "node:fs";

const manifest = JSON.parse(fs.readFileSync("packages/es-markdown-core/package.json", "utf8"));
if (manifest.scripts.preconformance !== "pnpm run bundle:conformance-fixtures") process.exit(1);
if (manifest.scripts["bundle:conformance-fixtures"] !== "node scripts/bundle-conformance-fixtures.mjs") process.exit(1);
NODE
note "ES correctness and conformance discovery are disjoint"

if ! grep -q 'xcodebuild test' package.json \
    || ! grep -q 'markdownCoreAndroidPageSizesGroupAndroidDeviceTest' package.json \
    || ! grep -q 'managedDevices' packages/kotlin-markdown-core/build.gradle.kts; then
    fail "platform tasks do not invoke real iOS Simulator and Android emulator targets"
elif grep -q 'connectedAndroidDeviceTest' package.json \
    || grep -q 'reactivecircus/android-emulator-runner' .github/workflows/ci.yml \
    || [[ -e scripts/android-test-emulator.sh ]]; then
    fail "Android emulator routing depends on a host-configured AVD lifecycle"
else
    note "iOS Simulator and repo-managed Android emulator targets use native test runners"
fi
if [[ $(grep -c 'android.experimental.testOptions.managedDevices.maxConcurrentDevices=1' package.json) -ne 2 ]]; then
    fail "Android page-size managed devices are not serialized for CI runner capacity"
else
    note "Android page-size managed devices run one at a time"
fi
if ! grep -q 'pnpm test:swift-macos' .github/workflows/ci.yml \
    || ! grep -q 'pnpm conformance:swift-macos' .github/workflows/ci.yml \
    || ! grep -q 'pnpm test:kotlin-android-emulator' .github/workflows/ci.yml \
    || ! grep -q 'pnpm conformance:kotlin-android-emulator' .github/workflows/ci.yml \
    || ! grep -q 'pnpm test:es-node' .github/workflows/ci.yml \
    || ! grep -q 'pnpm conformance:es-node' .github/workflows/ci.yml; then
    fail "CI does not execute separate correctness and conformance platform targets"
else
    note "CI destinations execute separate correctness and conformance targets"
fi
if ! grep -q 'withDeviceTestBuilder' packages/kotlin-markdown-core/build.gradle.kts \
    || ! grep -q 'sourceSetTreeName = "test"' packages/kotlin-markdown-core/build.gradle.kts; then
    fail "Android device tests do not reuse the commonTest source tree"
else
    note "Android host/device/native targets reuse shared Kotlin test sources"
fi
note "only explicitly supported family/platform tasks are exposed"

# 7. CTest topology: configure/build if needed, then cross-check labels and
# per-case registrations against runner discovery.
BUILD_DIR=build/cmake
if [ ! -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
    cmake --preset default >/dev/null
    cmake --build --preset default --parallel >/dev/null
fi

tests_all=$("ctest" --test-dir "$BUILD_DIR" -N | sed -n 's/^  Test *#[0-9]*: //p')
for label in api facade conformance consumer spec extensions regression pathological fuzz packaging benchmark; do
    count=$(ctest --test-dir "$BUILD_DIR" -N -L "^${label}$" | sed -n 's/^Total Tests: //p')
    if [ "${count:-0}" -lt 1 ]; then
        fail "no CTest tests carry label '$label'"
    fi
done
note "every frozen label resolves to at least one test"

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
for workload in $("$runner_dir/bench_runner" --list); do
    echo "$tests_all" | grep -q "^benchmark_${workload}$" \
        || fail "benchmark workload '$workload' is not registered in CTest"
done
note "runner discovery matches CTest registration"

# 8. Swift suites must be real (not build-only) when a toolchain is present;
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
