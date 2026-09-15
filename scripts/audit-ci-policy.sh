#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

ci=.github/workflows/ci.yml
changes=.github/workflows/changes.yml
codeql=.github/workflows/codeql.yml
release=.github/workflows/release.yml
release_dry_run=.github/workflows/release-dry-run.yml
stage_benchmark=.github/workflows/stage-benchmark.yml
ruleset=.github/rulesets/main.json
owner_review_ruleset=.github/rulesets/owner-review.json
release_ruleset=.github/rulesets/release-tags.json
release_environment=.github/environments/release.json
release_environment_policy=.github/environments/release-tag-policy.json

scripts/audit-toolchain-versions.sh

if command -v rg >/dev/null 2>&1; then
    search() {
        rg -q "$@"
    }
else
    search() {
        grep -Eq "$@"
    }
fi

# Extract the body of one top-level YAML key (a job under `jobs:` or a
# trigger under `on:`): starts at the named 4-space key and ends at the next
# 4-space key, whatever it is. Policy assertions must slice structurally so
# that reordering neighbouring jobs cannot silently change what they inspect
# (implementation layout is not CI contract; see docs/architecture/testing.md).
job_body() {
    awk -v key="$1" '
        BEGIN { target = "    " key ":" }
        $0 == target { collecting = 1; print; next }
        collecting && /^    [A-Za-z0-9_-]+:$/ { exit }
        collecting { print }
    ' "$2"
}

# Supply-chain pinning: every workflow action must reference an immutable
# commit SHA (a movable major tag lets a tag replacement change the code CI
# and release jobs execute without a reviewed diff). Repository-local
# composite actions (uses: ./…) are exempt from the SHA rule, but their
# manifests execute in the same jobs, so they are scanned alongside the
# workflows.
action_sources=(.github/workflows/)
if [ -d .github/actions ]; then
    action_sources+=(.github/actions/)
fi
if grep -rhoE 'uses: [^ ]+' "${action_sources[@]}" | grep -vE 'uses: [^ ]+@[0-9a-f]{40}$' | grep -v 'uses: \./' | grep -q .; then
    echo "workflow action references must be pinned to a full commit SHA:" >&2
    grep -rnE 'uses: [^ ]+' "${action_sources[@]}" | grep -vE '@[0-9a-f]{40}( #.*)?$' | grep -v 'uses: \./' >&2
    exit 1
fi

for required in \
    "$ci" \
    "$changes" \
    "$codeql" \
    "$release" \
    "$release_dry_run" \
    "$stage_benchmark" \
    "$ruleset" \
    "$owner_review_ruleset" \
    "$release_ruleset" \
    "$release_environment" \
    "$release_environment_policy"; do
    if [ ! -f "$required" ]; then
        echo "missing CI policy file: $required" >&2
        exit 1
    fi
done

node --test scripts/tests/ci-changes.test.mjs scripts/tests/callgrind.test.mjs

# THE PERFORMANCE PIPELINE MEASURES WORK, NOT TIME. Every hosted-runner
# wall-clock pipeline this repository has had was retired for the same reason:
# a number a neighbouring build can move is not evidence, and a comparison
# against a moving baseline needs a trust protocol to be safe at all. The
# replacement counts instructions and data references under callgrind and
# compares against a pinned cmark, so neither the runner nor a base build is
# part of the result.
for retired in \
    .github/workflows/benchmark.yml \
    .github/workflows/pr-benchmark.yml \
    .github/workflows/pr-benchmark-comment.yml \
    .github/workflows/pr-metrics.yml \
    .github/workflows/pr-metrics-comment.yml \
    scripts/collect-pr-metrics.mjs \
    scripts/pr-benchmark-result.mjs \
    packages/markdown-core/tests/runners/bench_runner.c \
    packages/es-markdown-core/scripts/benchmark.mjs \
    packages/kotlin-markdown-core/src/jvmBenchmark/kotlin/com/nouprax/markdown/core/benchmark/Benchmark.kt \
    packages/swift-markdown-core/Benchmarks/MarkdownCoreBenchmarks/main.swift; do
    if [ -e "$retired" ]; then
        echo "retired hosted-runner wall-clock pipeline still exists: $retired" >&2
        exit 1
    fi
done
if grep -Eq 'MarkdownCoreBenchmarks|kotlinBenchmark|jvmBenchmark|scripts/benchmark\.mjs|benchmark:(swift|kotlin|es|c-host)' \
    Package.swift \
    package.json \
    packages/kotlin-markdown-core/build.gradle.kts \
    packages/es-markdown-core/package.json; then
    echo "a retired wall-clock diagnostic is still routed by a package graph" >&2
    exit 1
fi

# The measurement's own contract. Each of these is a way the comparison stops
# being one without failing: a stage that stops being read from the call graph,
# an engine built with flags the other was not, a corpus only one side sees, or
# a report that reaches a privileged context as text.
test -x scripts/benchmark-stages.mjs
grep -Fq 'name: Stage Benchmark' "$stage_benchmark"
grep -Fq 'node scripts/benchmark-stages.mjs' "$stage_benchmark"
grep -Fq 'install --no-install-recommends --yes valgrind' "$stage_benchmark"
grep -Fq 'scripts/init-environment.sh --install oracle-cmark' "$stage_benchmark"
grep -Fq 'GITHUB_STEP_SUMMARY' "$stage_benchmark"
# The report is a job summary and an artifact, never a comment: a workflow that
# both runs pull-request code and holds a write token is the shape that made the
# previous pipeline need two workflows and an artifact-validation protocol.
if grep -Eq '^[[:space:]]+(pull-requests|issues|contents):[[:space:]]+write$' "$stage_benchmark"; then
    echo "the stage benchmark holds a write token while executing pull-request code" >&2
    exit 1
fi
if grep -Eq 'createComment|updateComment|issues\.create' "$stage_benchmark"; then
    echo "the stage benchmark writes untrusted measurement text back to a pull request" >&2
    exit 1
fi
# One harness, one corpus: both runners are the same driver source over the same
# generated document, and the driver checks each engine's receipt against it.
grep -Fq 'stage_runner.c markdown_core_stages.c' packages/markdown-core/benchmarks/CMakeLists.txt
grep -Fq 'stage_runner.c cmark_stages.c' packages/markdown-core/benchmarks/CMakeLists.txt
grep -Fq 'receiptBytes !== document.bytes' scripts/benchmark-stages.mjs
for boundary in \
    'markdown_core_parse_document_with_mem' \
    'S_parse_source' \
    'S_finish_parse' \
    'cmark_parser_feed' \
    'cmark_parser_finish'; do
    grep -Fq "$boundary" scripts/benchmark-stages.mjs || {
        echo "the stage benchmark no longer names the $boundary boundary" >&2
        exit 1
    }
done
# Both engines must be compiled from one pinned description, and the flags must
# keep the boundaries out of line: -O3 alone folds S_finish_parse into its
# caller, which reports the AST stage as absent rather than as cheap.
grep -Fq '"CMAKE_C_FLAGS_RELEASE": "-O3 -DNDEBUG -g -fno-inline-functions-called-once"' CMakePresets.json
grep -Fq 'CMAKE_C_FLAGS_RELEASE' scripts/benchmark-stages.mjs
grep -Fq 'verifyStageSymbols' scripts/benchmark-stages.mjs
# The preset is not the whole description of a build. CMake initializes
# CMAKE_C_FLAGS from CFLAGS and CMAKE_EXE_LINKER_FLAGS from LDFLAGS, once, at
# first configure -- so a tree keeps an inherited -march=native or -static for
# every later build. All four reach a command line, so all four are what a tree
# is identified and compared by; recording a subset makes two differently built
# comparisons look like one report.
for cached in \
    'CMAKE_C_FLAGS' \
    'CMAKE_C_FLAGS_RELEASE' \
    'CMAKE_EXE_LINKER_FLAGS' \
    'CMAKE_EXE_LINKER_FLAGS_RELEASE'; do
    grep -Fq "\"$cached\"" scripts/benchmark-stages.mjs || {
        echo "the stage benchmark does not read $cached, so it cannot describe the build it measured" >&2
        exit 1
    }
done
for inherited in CFLAGS LDFLAGS; do
    grep -Fq "process.env.$inherited" scripts/benchmark-stages.mjs || {
        echo "the stage benchmark does not account for an inherited $inherited" >&2
        exit 1
    }
done
# The resolved target is the compiler's whole answer, not the two model names in
# it: one -march/-mtune pair covers host CPUs that differ in which features they
# expose, and the cache sizes that steer unrolling are in the params rather than
# in --help=target.
for probe in '--help=target' '--help=params'; do
    grep -Fq -- "$probe" scripts/benchmark-stages.mjs || {
        echo "the stage benchmark does not ask the compiler for $probe" >&2
        exit 1
    }
done
# An identity row that cannot be determined fails the run. Recording "unknown"
# instead would make two hosts that could not answer compare as equal, which is
# the one thing the identity exists to prevent.
if grep -Eq '\breturn "unknown"|summary: "unknown"' scripts/benchmark-stages.mjs; then
    echo "the stage benchmark records an undetermined identity row as unknown" >&2
    exit 1
fi
# The loader's inputs are not part of the identity because they are not allowed
# into the measurement: an exported LD_PRELOAD or GLIBC_TUNABLES changes what
# the parse stages execute while every row of the report stays as it was.
grep -Fq 'measurementEnvironment' scripts/benchmark-stages.mjs || {
    echo "the stage benchmark measures under the caller's environment" >&2
    exit 1
}
# Built, not filtered. A denylist has to name every variable that can reach into
# a measurement -- the loader's, glibc's allocator controls, the locale -- and
# the one nobody named is admitted silently.
if grep -Fq '...process.env' scripts/benchmark-stages.mjs; then
    echo "the stage benchmark hands the caller's whole environment to the measurement" >&2
    exit 1
fi
# The oracle is the pinned commit only if nothing untracked is shadowing it: a
# stray src/config.h is neither tracked nor ignored, and the source directory
# precedes the build directory on the include path.
grep -Fq -- '--untracked-files=all' scripts/benchmark-stages.mjs || {
    echo "the cmark oracle check ignores untracked files" >&2
    exit 1
}
# The comparison links the archive the driver just built, never a DSO an
# earlier shared build left in the same tree ahead of it in suffix order.
grep -Fq 'CMAKE_FIND_LIBRARY_SUFFIXES' packages/markdown-core/benchmarks/CMakeLists.txt || {
    echo "the cmark oracle is linked by an unconstrained library lookup" >&2
    exit 1
}
# The toolchain is half the identity: the same binaries over different documents
# also move every count. The report names the workload it measured by digest,
# so an edited manifest or sample cannot be read as a parser change.
grep -Fq 'corpusDigest' scripts/benchmark-stages.mjs || {
    echo "the stage benchmark does not identify the corpus it measured" >&2
    exit 1
}
grep -Fq 'build/benchmark-stages/corpus' "$stage_benchmark" || {
    echo "the stage benchmark does not publish the corpus its digest names" >&2
    exit 1
}
# Both engines are rebuilt on every run. Nothing in a compiled binary says which
# source produced it, so an option to reuse one is an option for the report to
# state this commit's pins over another revision's instruction counts.
if grep -Eq 'skip-build|options\.build' scripts/benchmark-stages.mjs; then
    echo "the stage benchmark can reuse binaries it did not build" >&2
    exit 1
fi
# The engine keeps ONE parse entry and no measurement mode: the split is read
# out of the call graph afterwards, never built into the product.
if grep -R -nE 'markdown_core_parser_(begin|feed|read|finish)\b|MARKDOWN_CORE_(PARSE_)?PHASE' \
    packages/markdown-core/core packages/markdown-core/elements packages/markdown-core/include; then
    echo "a benchmark-only parse lifecycle leaked into the engine" >&2
    exit 1
fi
if grep -R -nE 'valgrind/callgrind\.h|CALLGRIND_' \
    packages/markdown-core/core packages/markdown-core/elements packages/markdown-core/include; then
    echo "profiler instrumentation leaked into the product sources" >&2
    exit 1
fi
# A measurement runner registered with CTest is a measurement one preset change
# away from being a merge gate.
if grep -Eq '(add_test|markdown_core_add_test)' packages/markdown-core/benchmarks/CMakeLists.txt; then
    echo "the stage runners are registered as tests" >&2
    exit 1
fi
if grep -R -nE 'START_TIMING|END_TIMING|TIMING[[:space:]]*[<>]=?|takes less than [0-9]+ms' \
    packages/markdown-core/tests; then
    echo "a wall-clock assertion leaked into the C correctness graph" >&2
    exit 1
fi

# The repo-managed installers must stay content-pinned: the emsdk manager
# to the immutable commit of its release tag, and the Python tool venvs to
# hash-locked requirements so an index-side replacement cannot change the
# bytes the tools run.
grep -q 'EMSCRIPTEN_COMMIT=[0-9a-f]\{40\}' scripts/init-environment.sh || {
    echo "init-environment.sh must pin the emsdk commit" >&2
    exit 1
}
grep -Fq 'scripts/install-clang-format.sh' scripts/init-environment.sh || {
    echo "init-environment.sh must use the repository clang-format installer" >&2
    exit 1
}
grep -q -- '--require-hashes' scripts/install-clang-format.sh || {
    echo "install-clang-format.sh must install from hash-locked requirements" >&2
    exit 1
}
grep -Fq 'scripts/install-clang-format.sh' "$ci" || {
    echo "CI must use the repository clang-format installer" >&2
    exit 1
}
grep -q -- '--require-hashes' scripts/format-cmake.sh || {
    echo "format-cmake.sh must install Python tools with --require-hashes" >&2
    exit 1
}

for adapter in \
    c \
    es \
    kotlin-host \
    swift; do
    test -x "scripts/build-${adapter}-test-artifact.sh"
    test -x "scripts/run-${adapter}-test-artifact.sh"
done
test -x scripts/build-kotlin-android-test-artifact.sh
test -x scripts/run-kotlin-android-test-artifact.sh
test -x scripts/prepare-swift-ios-simulator.sh
for platform in c es kotlin swift; do
    test -x "scripts/build-${platform}-product-artifact.sh"
done
grep -Fq -- '-DMARKDOWN_CORE_TESTS=OFF' scripts/build-c-product-artifact.sh
grep -Fq -- '-DMARKDOWN_CORE_TESTS=ON' scripts/build-c-test-artifact.sh
grep -Fq 'Total Tests: [1-9][0-9]*' scripts/build-c-test-artifact.sh
grep -Fq 'audit-test-topology.sh" "$root/$build_dir" "$configuration"' scripts/build-c-test-artifact.sh
grep -Fq "tr -d '\\r'" scripts/audit-test-topology.sh
if grep -Eq 'cmake --preset|cmake --build|swift test|gradle\.sh' scripts/audit-test-topology.sh; then
    echo "repository topology health-check must not duplicate a platform build or test discovery" >&2
    exit 1
fi
grep -Fq -- 'swift build --target MarkdownCore' scripts/build-swift-product-artifact.sh
grep -Fq -- '-DMARKDOWN_CORE_TESTS=OFF' scripts/stage-c-release.sh
grep -Fq 'Package.release.swift' scripts/check-swift-source-archive.sh
if grep -Eq 'swift package archive-source|cp .*Tests|cp .*Benchmarks|swift test|specs/canonical-ast' \
    scripts/check-swift-source-archive.sh; then
    echo "Swift release staging still includes test, benchmark, or conformance source" >&2
    exit 1
fi
if grep -Eq '\.testTarget|MarkdownCoreBenchmarks|Conformance|Plugins|Tools' \
    packages/swift-markdown-core/Package.release.swift; then
    echo "Swift release manifest contains non-product targets" >&2
    exit 1
fi
grep -Fq 'auditProductArchive' scripts/audit-maven-publications.mjs
grep -Fq 'publishes a test framework dependency' scripts/audit-maven-publications.mjs
# THESE TWO TEST A STRING IN A SIBLING SCRIPT, NOT THAT THE TASK EXISTS, which
# is how the release job stayed broken while this audit was green (§4.14.15H).
# The task is real now -- `abiValidation` is configured -- so the third line
# below is the one that would have caught it.
grep -Fq ':packages:kotlin-markdown-core:checkKotlinAbi' scripts/stage-maven-publications.sh
grep -Fq '"${abi_tasks[@]}"' scripts/stage-maven-publications.sh
scripts/gradle.sh --quiet :packages:kotlin-markdown-core:tasks --all |
    grep -Fq checkKotlinAbi ||
    {
        echo "stage-maven-publications.sh names checkKotlinAbi and the build defines no such task" >&2
        exit 1
    }
if grep -Eq 'bundle-conformance|run-tests|run-conformance' scripts/build-es-product-artifact.sh; then
    echo "ES product build contains test-only work" >&2
    exit 1
fi

for job in \
    health-check-repository \
    health-check-c \
    health-check-es \
    health-check-kotlin \
    health-check-swift \
    health-checks-ready \
    c-product-build \
    c-product-build-windows \
    es-product-build \
    kotlin-product-build \
    swift-product-build \
    swift-deployment-contract \
    swift-test-build \
    swift-test \
    kotlin-test-build \
    kotlin-test \
    kotlin-android-test-build \
    kotlin-android-test \
    es-test-build \
    es-test \
    c-test-build \
    c-test-build-windows \
    c-sanitizer-test-build \
    c-test \
    c-sanitizer-test \
    builds-ready \
    build-tests-ready \
    tests-ready; do
    search "^    ${job}:$" "$ci"
done
search 'actions/upload-artifact@' "$ci"
search 'actions/download-artifact@' "$ci"
search 'test-without-building' scripts/run-swift-test-artifact.sh
search -- '--skip-build' scripts/run-swift-test-artifact.sh
grep -Fq -- '--scratch-path "$root_scratch"' scripts/build-swift-test-artifact.sh
grep -Fq -- '--scratch-path "$root_scratch"' scripts/run-swift-test-artifact.sh
grep -Fq 'remove_directory "$root/$build_dir"' scripts/build-c-test-artifact.sh
grep -Fq 'rm -rf "$project_build/ci-test-artifact"' scripts/build-kotlin-host-test-artifact.sh
for producer in scripts/build-c-test-artifact.sh scripts/build-kotlin-host-test-artifact.sh scripts/build-swift-test-artifact.sh; do
    grep -Eq 'benchmark (payload|executable)' "$producer"
done
if grep -Eq 'node-benchmark|macos-benchmark|stageJvmBenchmarkArtifact|jvm-benchmark|ci-benchmark' \
    scripts/build-swift-test-artifact.sh \
    scripts/run-swift-test-artifact.sh \
    scripts/build-kotlin-host-test-artifact.sh \
    scripts/run-kotlin-host-test-artifact.sh \
    scripts/run-es-test-artifact.sh \
    scripts/run-c-test-artifact.sh; then
    echo "CI test artifacts still route a removed benchmark payload" >&2
    exit 1
fi
grep -Fq -- "-destination 'generic/platform=iOS Simulator'" scripts/build-swift-test-artifact.sh
grep -Fq 'prepare-swift-ios-simulator.sh' scripts/run-swift-test-artifact.sh
if grep -Eq 'name=iPhone|OS=latest' scripts/build-swift-test-artifact.sh scripts/run-swift-test-artifact.sh \
    scripts/run-swift-ios-tests.sh package.json; then
    echo "Swift CI or the pnpm entry points hard-code a simulator model or moving runtime alias" >&2
    exit 1
fi
search -- '--skip-build' packages/es-markdown-core/scripts/run-tests.mjs
search 'artifact_verify ' scripts/run-kotlin-android-test-artifact.sh
if search 'gradle\.sh|cmake --build|swift build|xcodebuild build-for-testing|build\.mjs|\bemcc\b' \
    scripts/run-*-test-artifact.sh; then
    echo "test artifact consumer contains a compiler/build invocation" >&2
    exit 1
fi

android_test_job=$(job_body kotlin-android-test "$ci")
for forbidden in \
    'setup-java' \
    'setup-node' \
    'pnpm/action-setup' \
    'publishKotlinToMavenLocal' \
    'run-kotlin-android-emulator-tests.sh' \
    'cmake;3.22.1' \
    'ndk;28.2.13676358'; do
    if grep -Fq "$forbidden" <<<"$android_test_job"; then
        echo "Android test consumer contains build dependency: $forbidden" >&2
        exit 1
    fi
done
if [ "$(grep -c -E '^[[:space:]]*suite:' <<<"$android_test_job")" -ne 4 ]; then
    echo "Android correctness/conformance and 4K/16K must be four independent consumers" >&2
    exit 1
fi
for consumer in \
    swift-test \
    kotlin-test \
    kotlin-android-test \
    es-test \
    c-test \
    c-test-windows \
    c-sanitizer-test; do
    consumer_job=$(job_body "$consumer" "$ci")
    if ! grep -Fq '        needs: build-tests-ready' <<<"$consumer_job"; then
        echo "test consumer bypasses the global build-test barrier: $consumer" >&2
        exit 1
    fi
done

for producer in \
    c-product-build \
    c-product-build-windows \
    es-product-build \
    kotlin-product-build \
    swift-product-build \
    swift-deployment-contract; do
    producer_job=$(job_body "$producer" "$ci")
    if ! grep -Fq '        needs: health-checks-ready' <<<"$producer_job"; then
        echo "build producer bypasses the global health-check barrier: $producer" >&2
        exit 1
    fi
done

for contract in \
    package-audit \
    kotlin-consumers \
    swift-test-build \
    kotlin-test-build \
    kotlin-android-test-build \
    es-test-build \
    c-test-build \
    c-test-build-windows \
    c-sanitizer-test-build; do
    contract_job=$(job_body "$contract" "$ci")
    if ! grep -Fq '        needs: builds-ready' <<<"$contract_job"; then
        echo "build test bypasses the global build barrier: $contract" >&2
        exit 1
    fi
done

search '^        name: Health Check - C$' "$ci"
search '^        name: Health Check - ES$' "$ci"
search '^        name: Health Check - Kotlin$' "$ci"
search '^        name: Health Check - Swift$' "$ci"
search '^        name: Build - C ' "$ci"
search '^        name: Build - ES / WASM Package$' "$ci"
search '^        name: Build - Kotlin ' "$ci"
search '^        name: Build - Swift / Product$' "$ci"
search '^        name: Build Test - C ' "$ci"
search '^        name: Build Test - ES / Test Bundle$' "$ci"
search '^        name: Build Test - Kotlin ' "$ci"
search '^        name: Build Test - Swift / Test Products$' "$ci"
search '^        name: Test - C Sanitizer ' "$ci"
if search '^        name:.*matrix\.(os|suite|compiler|shared|sanitizer|platform|version|target-id|artifact-label)' "$ci"; then
    echo "matrix implementation fields leaked into a visible CI job name" >&2
    exit 1
fi

oracle_job=$(job_body upstream-parity "$ci")
grep -Fq 'scripts/init-environment.sh --install oracle-cmark oracle-cmark-gfm' <<<"$oracle_job"
grep -Fq 'pnpm check:commonmark-parity' <<<"$oracle_job"
grep -Fq 'pnpm check:gfm-parity' <<<"$oracle_job"
grep -Fq 'pnpm check:mdast-parity' <<<"$oracle_job"
grep -Fq 'pnpm fuzz:parity -- --oracle commonmark' <<<"$oracle_job"
grep -Fq 'pnpm fuzz:parity -- --oracle gfm' <<<"$oracle_job"
grep -Fq 'pnpm fuzz:parity -- --oracle remark' <<<"$oracle_job"
if grep -Eq 'check:upstream-parity|--oracle (upstream|mdast)' <<<"$oracle_job"; then
    echo "external parity job uses a retired ambiguous oracle name" >&2
    exit 1
fi
required_gate_job=$(job_body required-gates "$ci")
grep -Fq '            - tests-ready' <<<"$required_gate_job"
grep -Fq '            - upstream-parity' <<<"$required_gate_job"
if grep -Eq 'benchmark|pr-metrics|collect-pr-metrics|binary\.size|coverage' <<<"$required_gate_job"; then
    echo "measurement-only work leaked into the required gate" >&2
    exit 1
fi
if grep -Eq '^    coverage(-ready)?:' "$ci"; then
    echo "execution coverage must not be a CI job; use semantic contract tests" >&2
    exit 1
fi
if [ -d specs/coverage ] || [ -e scripts/check-coverage.mjs ] || \
    find scripts -maxdepth 1 -type f -name 'coverage-*' -print -quit | grep -q . || \
    grep -Eq '"coverage:[^"]+"' package.json; then
    echo "retired execution-coverage infrastructure returned" >&2
    exit 1
fi

search '^    push:$' "$release"
search '^        tags:$' "$release"
search '^    workflow_dispatch:$' "$release"
if search '^    pull_request:$' "$release"; then
    echo "formal release workflow may not accept pull requests" >&2
    exit 1
fi
search '^    contents: read$' "$release"
search '^        environment: release$' "$release"
search '^    quality:$' "$release"
search '^        name: Quality Gate - Release$' "$release"
search '^        uses: \./\.github/workflows/ci\.yml$' "$release"
# The ban is on consulting historical check results; the tag-ancestry
# guard (git merge-base --is-ancestor against origin/main) is tag-local
# validation and stays allowed.
if search 'GITHUB_SHA|check-runs|CodeQL gate|Required gates|Development branch gates' "$release"; then
    echo "formal release must run tag-local quality gates instead of querying historical checks" >&2
    exit 1
fi
search 'merge-base --is-ancestor HEAD origin/main' "$release"
if [ "$(grep -c '^        needs: quality$' "$release")" -ne 5 ]; then
    echo "every initial release artifact job must wait for tag-local quality gates" >&2
    exit 1
fi
for release_name in \
    'Health Check - Release / Tag and Versions' \
    'Build Release - C / ${{ matrix.label }}' \
    'Build Release - Swift / Product Source' \
    'Build Release - ES / npm Package' \
    'Build Release - Kotlin / Linux Publications' \
    'Build Release - Kotlin / macOS Publications' \
    'Assemble Release - Maven Central' \
    'Release Artifacts - Ready' \
    'Publish Release - Maven Central / Stage' \
    'Publish Release - ES / npm' \
    'Publish Release - Maven Central / Commit' \
    'Publish Release - GitHub'; do
    grep -Fq "        name: $release_name" "$release"
done
release_ready_job=$(job_body release-artifacts-ready "$release")
grep -Fq "if: \${{ github.event_name == 'push' && always() }}" <<<"$release_ready_job"
for dependency in c-artifacts swift-source npm-package maven-assemble; do
    grep -Fq "$dependency" <<<"$release_ready_job"
done
maven_stage_job=$(job_body maven-stage "$release")
grep -Fq '        needs: release-artifacts-ready' <<<"$maven_stage_job"
grep -Fq 'central-portal.sh upload build/markdown-core-maven-central.zip' <<<"$maven_stage_job"
if search 'central-portal\.sh upload' <(job_body maven-assemble "$release"); then
    echo "Maven assembly phase may not publish externally" >&2
    exit 1
fi
search '^            id-token: write$' "$release"
search '^            attestations: write$' "$release"
search 'actions/attest-build-provenance@' "$release"
search 'npm publish \./release-npm/\*\.tgz --access public' "$release"
search '^    resume-publish:$' "$release"
search "if: github.event_name == 'workflow_dispatch'" "$release"
search 'gh run download "\$SOURCE_RUN_ID" --name release-npm-package' "$release"
# Tag publication and manual resume for the same release must share one
# concurrency lock: the group derives from the effective release tag for
# both events, never from the dispatch branch ref.
grep -Fq "group: release-\${{ github.event_name == 'workflow_dispatch' && format('refs/tags/{0}', inputs.release-tag) || github.ref }}" "$release"
# A resume must reject a source run that is still publishing.
grep -Fq "test \"\$(jq -r '.status' <<<\"\$run_info\")\" = \"completed\"" "$release"
# The Central deployment id is bound to the source run, never operator
# input: the stage records it as a run artifact and the resume downloads
# and cross-checks it against the protected tag and version.
if search 'central-deployment-id' "$release"; then
    echo "release resume must not accept a free-form Central deployment id" >&2
    exit 1
fi
grep -Fq 'name: release-central-deployment' "$release"
search 'gh run download "\$SOURCE_RUN_ID" --name release-central-deployment' "$release"
grep -Fq 'test "$bound_tag" = "$RELEASE_TAG"' "$release"
grep -Fq 'test "$bound_version" = "$(cat VERSION)"' "$release"
grep -Fq 'test -s "docs/releases/$(cat VERSION).md"' "$release"
grep -Fq -- '--notes-file "docs/releases/$(cat VERSION).md"' "$release"
if search -- '--generate-notes' "$release"; then
    echo "formal release workflow must use curated release notes" >&2
    exit 1
fi
search 'publishingType=USER_MANAGED' scripts/central-portal.sh
for secret in \
    MAVEN_CENTRAL_USERNAME \
    MAVEN_CENTRAL_PASSWORD \
    MAVEN_SIGNING_KEY \
    MAVEN_SIGNING_PASSWORD; do
    search "secrets\.$secret" "$release"
done
if search 'NODE_AUTH_TOKEN|NPM_TOKEN|secrets\.NPM' "$release"; then
    echo "npm release job must use OIDC rather than a registry token" >&2
    exit 1
fi

search '^    pull_request:$' "$release_dry_run"
search '^    merge_group:$' "$release_dry_run"
search '^    workflow_dispatch:$' "$release_dry_run"
search '^    contents: read$' "$release_dry_run"
grep -Fq "name: \${{ (github.event_name == 'pull_request' || github.event_name == 'merge_group') && 'Release Dry Run - Ready' || 'Manual Release Dry Run - Ready' }}" "$release_dry_run"
search 'sign-maven-publications\.sh build/release-maven-central --ephemeral' "$release_dry_run"
search 'audit-maven-publications\.mjs' "$release_dry_run"
search 'build/release-maven-central --full --signed' "$release_dry_run"
if search 'secrets\.|environment: release|contents: write|id-token: write' "$release_dry_run"; then
    echo "release dry run may not read secrets or request publish permissions" >&2
    exit 1
fi

for workflow in "$ci" "$codeql" "$release_dry_run"; do
    if ! search '^    merge_group:$' "$workflow"; then
        echo "blocking workflow lacks merge_group support: $workflow" >&2
        exit 1
    fi
done

# The stable release-readiness context is a fail-closed projection of the
# complete artifact graph. Keep every leaf producer explicit here: an omitted
# or skipped result must fail when full validation is required. Documentation
# skips and classification failures are covered by the behavioral tests above.
dry_run_gate=$(job_body dry-run-gate "$release_dry_run")
grep -Fq '        needs: [changes, validate, c-artifacts, swift-source, npm-package, maven-linux, maven-macos, maven-aggregate]' <<<"$dry_run_gate"
for result in \
    'needs.changes.result' \
    'needs.validate.result' \
    "needs['c-artifacts'].result" \
    "needs['swift-source'].result" \
    "needs['npm-package'].result" \
    "needs['maven-linux'].result" \
    "needs['maven-macos'].result" \
    "needs['maven-aggregate'].result"; do
    grep -Fq "$result" <<<"$dry_run_gate"
done
grep -Fq 'test "$result" = success' <<<"$dry_run_gate"

maven_aggregate=$(job_body maven-aggregate "$release_dry_run")
grep -Fq '        needs: [maven-linux, maven-macos]' <<<"$maven_aggregate"
search '^    workflow_call:$' "$ci"

ci_push_trigger=$(job_body push "$ci")
if ! grep -Fqx '        branches:' <<<"$ci_push_trigger" ||
    ! grep -Fqx '            - main' <<<"$ci_push_trigger"; then
    echo "blocking CI push trigger must cover only the default branch" >&2
    exit 1
fi
if [ "$(grep -c '^            - ' <<<"$ci_push_trigger")" -ne 1 ]; then
    echo "blocking CI push trigger must not duplicate pull-request CI on feature branches" >&2
    exit 1
fi
if search '^        tags(-ignore)?:' <<<"$ci_push_trigger"; then
    echo "blocking CI push trigger must not run on release tags" >&2
    exit 1
fi

search '^    required-gates:$' "$ci"
grep -Fq "name: \${{ (github.event_name == 'pull_request' || github.event_name == 'merge_group') && 'Required gates' || 'Development branch gates' }}" "$ci"
grep -Fq 'group: ci-${{ github.event_name }}-${{ github.event.pull_request.number || github.ref }}' "$ci"
search '^    cancel-in-progress: true$' "$ci"
search '^    codeql-gate:$' "$codeql"
search '^        name: CodeQL gate$' "$codeql"
grep -Fq '        name: Security Scan - ${{ matrix.label }}' "$codeql"

for workflow in "$ci" "$codeql" "$release" "$release_dry_run"; do
    if search '^        name:.*matrix\.(os|suite|compiler|shared|sanitizer|platform|version|target-id|artifact-label|language)' "$workflow"; then
        echo "matrix implementation fields leaked into a visible job name: $workflow" >&2
        exit 1
    fi
done

if search '^    (benchmark-[A-Za-z0-9_-]+|benchmarks-ready):|collect-pr-metrics|pr-metrics|node-benchmark|macos-benchmark|stageJvmBenchmarkArtifact' "$ci"; then
    echo "hosted-runner performance measurement is forbidden in required CI" >&2
    exit 1
fi

node --input-type=module - "$ruleset" "$owner_review_ruleset" <<'NODE'
import fs from "node:fs";

const ruleset = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const ownerReviewRuleset = JSON.parse(fs.readFileSync(process.argv[3], "utf8"));
const required = ruleset.rules.find((rule) => rule.type === "required_status_checks");
const contexts = required?.parameters?.required_status_checks?.map((check) => check.context).sort();
const expected = ["CodeQL gate", "Release Dry Run - Ready", "Required gates"];
if (JSON.stringify(contexts) !== JSON.stringify(expected)) {
    throw new Error(`ruleset required checks changed: ${JSON.stringify(contexts)}`);
}
if (ruleset.conditions?.ref_name?.include?.join(",") !== "~DEFAULT_BRANCH") {
    throw new Error("ruleset must target only the default branch");
}
const mainPullRequest = ruleset.rules.find((rule) => rule.type === "pull_request");
if (mainPullRequest?.parameters?.required_reviewers?.length) {
    throw new Error("owner reviewers must not share the main CI ruleset");
}
if (mainPullRequest?.parameters?.required_review_thread_resolution !== true) {
    throw new Error("all pull-request review conversations must be resolved before merge");
}
if (
    ownerReviewRuleset.target !== "branch" ||
    ownerReviewRuleset.enforcement !== "active" ||
    ownerReviewRuleset.conditions?.ref_name?.include?.join(",") !== "~DEFAULT_BRANCH"
) {
    throw new Error("owner approval gate must be active on the default branch");
}
if (
    ownerReviewRuleset.rules?.length !== 1 ||
    ownerReviewRuleset.rules[0]?.type !== "pull_request"
) {
    throw new Error("owner approval gate must contain only the pull-request review rule");
}
const reviewers = ownerReviewRuleset.rules[0]?.parameters?.required_reviewers;
if (
    reviewers?.length !== 1 ||
    reviewers[0]?.file_patterns?.join(",") !== "*" ||
    reviewers[0]?.minimum_approvals !== 1 ||
    reviewers[0]?.reviewer?.id !== 18548697 ||
    reviewers[0]?.reviewer?.type !== "Team"
) {
    throw new Error("all pull requests must require approval from nouprax-core");
}
if (
    ownerReviewRuleset.bypass_actors?.length !== 1 ||
    ownerReviewRuleset.bypass_actors[0]?.actor_id !== 8455725 ||
    ownerReviewRuleset.bypass_actors[0]?.actor_type !== "User" ||
    ownerReviewRuleset.bypass_actors[0]?.bypass_mode !== "pull_request"
) {
    throw new Error("only DongyuZhao may bypass the owner approval gate on pull requests");
}
NODE

node --input-type=module - "$release_ruleset" "$release_environment" "$release_environment_policy" <<'NODE'
import fs from "node:fs";

const releaseRuleset = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const environment = JSON.parse(fs.readFileSync(process.argv[3], "utf8"));
const deploymentPolicy = JSON.parse(fs.readFileSync(process.argv[4], "utf8"));

if (releaseRuleset.target !== "tag" || releaseRuleset.enforcement !== "active") {
    throw new Error("release tag ruleset must be active and target tags");
}
if (releaseRuleset.conditions?.ref_name?.include?.join(",") !== "refs/tags/v*.*.*") {
    throw new Error("release tag ruleset must target only v*.*.* tags");
}
const releaseRuleTypes = releaseRuleset.rules.map((rule) => rule.type).sort();
if (JSON.stringify(releaseRuleTypes) !== JSON.stringify(["creation", "deletion", "update"])) {
    throw new Error(`release tag rules changed: ${JSON.stringify(releaseRuleTypes)}`);
}
if (
    releaseRuleset.bypass_actors?.length !== 1 ||
    releaseRuleset.bypass_actors[0]?.actor_id !== 8455725 ||
    releaseRuleset.bypass_actors[0]?.actor_type !== "User" ||
    releaseRuleset.bypass_actors[0]?.bypass_mode !== "always"
) {
    throw new Error("release tag ruleset bypass must remain scoped to DongyuZhao");
}
if (
    environment.wait_timer !== 0 ||
    environment.prevent_self_review !== false ||
    environment.reviewers?.length !== 1 ||
    environment.reviewers[0]?.type !== "User" ||
    environment.reviewers[0]?.id !== 8455725
) {
    throw new Error("release environment reviewer policy changed");
}
if (
    environment.deployment_branch_policy?.protected_branches !== false ||
    environment.deployment_branch_policy?.custom_branch_policies !== true
) {
    throw new Error("release environment must use a custom deployment policy");
}
if (deploymentPolicy.name !== "v*.*.*" || deploymentPolicy.type !== "tag") {
    throw new Error("release environment must accept only v*.*.* tags");
}
NODE

echo "CI policy audit passed"
