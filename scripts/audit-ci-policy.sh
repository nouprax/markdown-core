#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

ci=.github/workflows/ci.yml
codeql=.github/workflows/codeql.yml
comment=.github/workflows/pr-metrics-comment.yml
release=.github/workflows/release.yml
release_dry_run=.github/workflows/release-dry-run.yml
ruleset=.github/rulesets/main.json
owner_review_ruleset=.github/rulesets/owner-review.json
release_ruleset=.github/rulesets/release-tags.json
release_environment=.github/environments/release.json
release_environment_policy=.github/environments/release-tag-policy.json

if command -v rg >/dev/null 2>&1; then
    search() {
        rg -q "$@"
    }
else
    search() {
        grep -Eq "$@"
    }
fi

for required in \
    "$ci" \
    "$codeql" \
    "$comment" \
    "$release" \
    "$release_dry_run" \
    "$ruleset" \
    "$release_ruleset" \
    "$release_environment" \
    "$release_environment_policy"; do
    if [ ! -f "$required" ]; then
        echo "missing CI policy file: $required" >&2
        exit 1
    fi
done

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
    benchmark-c \
    benchmark-kotlin \
    benchmark-es \
    benchmark-swift \
    benchmarks-ready \
    builds-ready \
    build-tests-ready \
    tests-ready; do
    search "^    ${job}:$" "$ci"
done
search 'actions/upload-artifact@' "$ci"
search 'actions/download-artifact@' "$ci"
search 'test-without-building' scripts/run-swift-test-artifact.sh
search -- '--skip-build' scripts/run-swift-test-artifact.sh
grep -Fq 'macos-benchmark' scripts/run-swift-test-artifact.sh
grep -Fq 'stageJvmBenchmarkArtifact' scripts/build-kotlin-host-test-artifact.sh
grep -Fq 'node-benchmark' scripts/run-es-test-artifact.sh
grep -Fq 'benchmark' scripts/run-c-test-artifact.sh
grep -Fq -- "-destination 'generic/platform=iOS Simulator'" scripts/build-swift-test-artifact.sh
grep -Fq 'prepare-swift-ios-simulator.sh' scripts/run-swift-test-artifact.sh
if grep -Eq 'name=iPhone|OS=latest' scripts/build-swift-test-artifact.sh scripts/run-swift-test-artifact.sh; then
    echo "Swift CI hard-codes a simulator model or moving runtime alias" >&2
    exit 1
fi
search -- '--skip-build' packages/es-markdown-core/scripts/run-tests.mjs
search 'sha256sum --check SHA256SUMS' scripts/run-kotlin-android-test-artifact.sh
if search 'gradle\.sh|cmake --build|swift build|xcodebuild build-for-testing|build\.mjs|\bemcc\b' \
    scripts/run-*-test-artifact.sh; then
    echo "test artifact consumer contains a compiler/build invocation" >&2
    exit 1
fi

android_test_job=$(sed -n '/^    kotlin-android-test:$/,/^    es-test-build:$/p' "$ci")
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
if [ "$(grep -c '^                      suite:' <<<"$android_test_job")" -ne 4 ]; then
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
    consumer_job=$(sed -n "/^    ${consumer}:$/,/^    [a-z].*:$/p" "$ci")
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
    producer_job=$(sed -n "/^    ${producer}:$/,/^    [a-z].*:$/p" "$ci")
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
    contract_job=$(sed -n "/^    ${contract}:$/,/^    [a-z].*:$/p" "$ci")
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

tests_ready_job=$(sed -n '/^    tests-ready:$/,/^    required-gates:$/p' "$ci")
grep -Fq '        if: ${{ always() }}' <<<"$tests_ready_job"
required_gate_job=$(sed -n '/^    required-gates:/,$p' "$ci")
grep -Fq '        needs: tests-ready' <<<"$required_gate_job"

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
if search 'GITHUB_SHA|check-runs|merge-base|CodeQL gate|Required gates|Development branch gates' "$release"; then
    echo "formal release must run tag-local quality gates instead of querying historical checks" >&2
    exit 1
fi
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
release_ready_job=$(sed -n '/^    release-artifacts-ready:$/,/^    maven-stage:$/p' "$release")
grep -Fq "if: \${{ github.event_name == 'push' && always() }}" <<<"$release_ready_job"
for dependency in c-artifacts swift-source npm-package maven-assemble; do
    grep -Fq "$dependency" <<<"$release_ready_job"
done
maven_stage_job=$(sed -n '/^    maven-stage:$/,/^    npm-publish:$/p' "$release")
grep -Fq '        needs: release-artifacts-ready' <<<"$maven_stage_job"
grep -Fq 'central-portal.sh upload build/markdown-core-maven-central.zip' <<<"$maven_stage_job"
if search 'central-portal\.sh upload' <(sed -n '/^    maven-assemble:$/,/^    release-artifacts-ready:$/p' "$release"); then
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

ci_push_trigger=$(sed -n '/^    push:$/,/^    merge_group:$/p' "$ci")
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

for workflow in "$ci" "$codeql" "$release" "$release_dry_run" "$comment"; do
    if search '^        name:.*matrix\.(os|suite|compiler|shared|sanitizer|platform|version|target-id|artifact-label|language)' "$workflow"; then
        echo "matrix implementation fields leaked into a visible job name: $workflow" >&2
        exit 1
    fi
done

for benchmark_name in \
    'Test - C / Linux · Benchmark' \
    'Test - Kotlin / JVM · Benchmark' \
    'Test - ES / Node · Benchmark' \
    'Test - Swift / macOS · Benchmark' \
    'Benchmarks - Ready'; do
    grep -Fq "        name: $benchmark_name" "$ci"
done
grep -Fq '        name: Report - PR Metrics / Comment' "$comment"

linux_benchmark_job=$(sed -n '/^    benchmark-c:$/,/^    benchmark-kotlin:$/p' "$ci")
if grep -Eq 'setup-java|setup-android|setup-emsdk|benchmark:kotlin|benchmark:es' <<<"$linux_benchmark_job"; then
    echo "C benchmark runner contains an unrelated platform workload" >&2
    exit 1
fi
for benchmark_job in benchmark-c benchmark-kotlin benchmark-es benchmark-swift; do
    benchmark_job_body=$(sed -n "/^    ${benchmark_job}:$/,/^    [a-z].*:$/p" "$ci")
    grep -Fq '        needs: build-tests-ready' <<<"$benchmark_job_body"
    case "$benchmark_job" in
        benchmark-c) forbidden='run-kotlin|run-es|run-swift' ;;
        benchmark-kotlin) forbidden='run-c|run-es|run-swift' ;;
        benchmark-es) forbidden='run-c|run-kotlin|run-swift' ;;
        benchmark-swift) forbidden='run-c|run-kotlin|run-es' ;;
    esac
    if grep -Eq "$forbidden" <<<"$benchmark_job_body"; then
        echo "benchmark runner contains an unrelated platform workload: $benchmark_job" >&2
        exit 1
    fi
done
if [ "$(grep -Fc 'SOURCE_SHA: ${{ github.event.pull_request.head.sha || github.sha }}' "$ci")" -ne 4 ]; then
    echo "benchmark metrics must identify PR head SHA and default-branch commit SHA explicitly" >&2
    exit 1
fi

if search 'pr-metrics|benchmark|binary.size' <(
    sed -n '/^    required-gates:/,$p' "$ci"
); then
    echo "non-blocking metrics leaked into the required gate" >&2
    exit 1
fi

search '^    workflow_run:$' "$comment"
grep -Fq '        workflows: [CI]' "$comment"
search '^    issues: write$' "$comment"
search '^    pull-requests: write$' "$comment"
search 'listWorkflowRunArtifacts' "$comment"
search 'listWorkflowRunsForRepo' "$comment"
grep -Fq 'run.head_sha === baseSha' "$comment"
grep -Fq 'document?.sourceSha !== expectedSha' "$comment"
search 'artifact\.size_in_bytes <= 65536' "$comment"
if search 'actions/checkout|github\.event\.pull_request\.head|gh pr checkout|git fetch' "$comment"; then
    echo "the privileged metrics commenter may not fetch or execute PR code" >&2
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
