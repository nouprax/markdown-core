#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

ci=.github/workflows/ci.yml
codeql=.github/workflows/codeql.yml
release=.github/workflows/release.yml
release_dry_run=.github/workflows/release-dry-run.yml
pr_benchmark=.github/workflows/pr-benchmark.yml
pr_benchmark_comment=.github/workflows/pr-benchmark-comment.yml
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
# (implementation layout is not CI contract; see test-architecture.md §8).
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
    "$codeql" \
    "$release" \
    "$release_dry_run" \
    "$pr_benchmark" \
    "$pr_benchmark_comment" \
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

# Retire the old cross-runtime metrics pipeline. The replacement is a single C
# parser workload in an independent workflow; it is informational, exact-base
# keyed, and never part of required CI.
for retired in \
    .github/workflows/benchmark.yml \
    .github/workflows/pr-metrics.yml \
    .github/workflows/pr-metrics-comment.yml \
    scripts/collect-pr-metrics.mjs; do
    if [ -e "$retired" ]; then
        echo "retired hosted-runner performance pipeline still exists: $retired" >&2
        exit 1
    fi
done
test -x scripts/pr-benchmark-result.mjs
grep -Fq 'name: PR Benchmark' "$pr_benchmark"
grep -Fq 'workflows: [PR Benchmark]' "$pr_benchmark_comment"
grep -Fq 'pr-benchmark-baseline-${{ github.sha }}' "$pr_benchmark"
grep -Fq 'name: pr-benchmark-head' "$pr_benchmark"
grep -Fq 'path: build/pr-benchmark/head.json' "$pr_benchmark"
head_benchmark_job=$(job_body measure-head "$pr_benchmark")
if grep -Eq 'base\.json|baseline-source|pull_request\.base|pr-benchmark-baseline' <<<"$head_benchmark_job"; then
    echo "PR-controlled benchmark job can access or publish trusted baseline data" >&2
    exit 1
fi
grep -Fq 'pr-benchmark-baseline-${{ steps.comparison.outputs.base_sha }}' "$pr_benchmark_comment"
grep -Fq 'github.rest.actions.listArtifactsForRepo' "$pr_benchmark_comment"
grep -Fq 'const artifactName = `pr-benchmark-baseline-${process.env.BASE_SHA}`' "$pr_benchmark_comment"
grep -Fq 'const trustedMain =' "$pr_benchmark_comment"
grep -Fq 'const trustedFallback =' "$pr_benchmark_comment"
grep -Fq 'run.path === ".github/workflows/pr-benchmark-comment.yml"' "$pr_benchmark_comment"
grep -Fq 'steps.baseline.outputs.usable != '"'"'true'"'"'' "$pr_benchmark_comment"
grep -Fq 'ref: ${{ steps.comparison.outputs.base_sha }}' "$pr_benchmark_comment"
grep -Fq 'persist-credentials: false' "$pr_benchmark_comment"
grep -Fq 'the privileged workflow will build and publish it' "$pr_benchmark_comment"
if [ "$(grep -c 'uses: actions/checkout@' "$pr_benchmark_comment")" -ne 1 ]; then
    echo "privileged benchmark workflow must checkout exactly one tree: the exact PR base" >&2
    exit 1
fi
if grep -Eq '^[[:space:]]+issues: write$' "$pr_benchmark_comment"; then
    echo "privileged benchmark workflow has unnecessary issue-wide write permission" >&2
    exit 1
fi
grep -Fq '"boundary" in metric' "$pr_benchmark_comment"
if grep -Eq '\|[^\n]*Boundary[^\n]*\|' "$pr_benchmark_comment"; then
    echo "PR benchmark comment still renders a boundary column" >&2
    exit 1
fi
if grep -Eq 'workflows: \[CI\]|run\.name === "CI"|successful main CI' \
    "$pr_benchmark" "$pr_benchmark_comment"; then
    echo "PR benchmark still depends on the normal main CI pipeline" >&2
    exit 1
fi
for retired in \
    packages/es-markdown-core/scripts/benchmark.mjs \
    packages/kotlin-markdown-core/src/jvmBenchmark/kotlin/com/nouprax/markdown/core/benchmark/Benchmark.kt \
    packages/swift-markdown-core/Benchmarks/MarkdownCoreBenchmarks/main.swift; do
    if [ -e "$retired" ]; then
        echo "retired binding wall-clock diagnostic still exists: $retired" >&2
        exit 1
    fi
done
if grep -Eq 'MarkdownCoreBenchmarks|kotlinBenchmark|jvmBenchmark|scripts/benchmark\.mjs|benchmark:(swift|kotlin|es)' \
    Package.swift \
    package.json \
    packages/kotlin-markdown-core/build.gradle.kts \
    packages/es-markdown-core/package.json; then
    echo "a retired binding wall-clock diagnostic is still routed by a package graph" >&2
    exit 1
fi
if grep -R -nE 'START_TIMING|END_TIMING|TIMING[[:space:]]*[<>]=?|takes less than [0-9]+ms' \
    packages/markdown-core/tests --exclude=bench_runner.c; then
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

tests_ready_job=$(job_body tests-ready "$ci")
grep -Fq '        if: ${{ always() }}' <<<"$tests_ready_job"
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
search '^    workflow_dispatch:$' "$release_dry_run"
search '^    contents: read$' "$release_dry_run"
search '^        name: Release Dry Run - Ready$' "$release_dry_run"
search 'sign-maven-publications\.sh build/release-maven-central --ephemeral' "$release_dry_run"
search 'audit-maven-publications\.mjs' "$release_dry_run"
search 'build/release-maven-central --full --signed' "$release_dry_run"
if search 'secrets\.|environment: release|contents: write|id-token: write' "$release_dry_run"; then
    echo "release dry run may not read secrets or request publish permissions" >&2
    exit 1
fi

for workflow in "$ci" "$codeql"; do
    if ! search '^    merge_group:$' "$workflow"; then
        echo "blocking workflow lacks merge_group support: $workflow" >&2
        exit 1
    fi
done
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
const expected = ["CodeQL gate", "Required gates"];
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
