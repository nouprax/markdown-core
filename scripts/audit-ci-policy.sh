#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

ci=.github/workflows/ci.yml
codeql=.github/workflows/codeql.yml
metrics=.github/workflows/pr-metrics.yml
comment=.github/workflows/pr-metrics-comment.yml
release=.github/workflows/release.yml
release_dry_run=.github/workflows/release-dry-run.yml
ruleset=.github/rulesets/main.json
release_ruleset=.github/rulesets/release-tags.json
release_environment=.github/environments/release.json
release_environment_policy=.github/environments/release-tag-policy.json
maven_wrapper=.mvn/wrapper/maven-wrapper.properties
maven_consumer=packages/kotlin-markdown-core/consumers/jvm-maven/pom.xml

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
    "$metrics" \
    "$comment" \
    "$release" \
    "$release_dry_run" \
    "$ruleset" \
    "$release_ruleset" \
    "$release_environment" \
    "$release_environment_policy" \
    "$maven_wrapper" \
    "$maven_consumer" \
    mvnw \
    mvnw.cmd; do
    if [ ! -f "$required" ]; then
        echo "missing CI policy file: $required" >&2
        exit 1
    fi
done

search '^    push:$' "$release"
search '^        tags:$' "$release"
if search '^    (pull_request|workflow_dispatch):$' "$release"; then
    echo "formal release workflow must only accept a protected tag event" >&2
    exit 1
fi
search '^    contents: read$' "$release"
search '^        environment: release$' "$release"
search 'check-release-quality-gates\.mjs' "$release"
search '^            id-token: write$' "$release"
search '^            attestations: write$' "$release"
search 'actions/attest-build-provenance@v3' "$release"
search 'npm publish release-npm/\*\.tgz --access public' "$release"
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
search '^        name: Release dry-run gate$' "$release_dry_run"
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

if search '^        branches:' <(
    sed -n '/^    push:$/,/^    merge_group:$/p' "$ci"
); then
    echo "blocking CI push trigger must cover development branches" >&2
    exit 1
fi

search '^    required-gates:$' "$ci"
grep -Fq "name: \${{ (github.event_name == 'pull_request' || github.event_name == 'merge_group') && 'Required gates' || 'Development branch gates' }}" "$ci"
grep -Fq 'group: ci-${{ github.event_name }}-${{ github.event.pull_request.number || github.ref }}' "$ci"
search '^    cancel-in-progress: true$' "$ci"
search '^                  pnpm audit:repository:clean$' "$ci"
search '^            - name: Verify Kotlin publication consumers$' "$ci"
search '^              run: pnpm check:kotlin-consumers$' "$ci"
search '^        name: Kotlin Android emulator \(x86_64\)$' "$ci"
grep -Fq '"system-images;android-36;google_apis;x86_64"' "$ci"
grep -Fq '"system-images;android-36;google_apis_ps16k;x86_64"' "$ci"
if search 'ubuntu-24\.04-arm|system-images;android-36;[^;]+;arm64-v8a' "$ci"; then
    echo "blocking CI requests an Android Emulator package unavailable on Linux ARM64" >&2
    exit 1
fi
search '^            - name: Verify runner architecture and emulator acceleration$' "$ci"
search '^    codeql-gate:$' "$codeql"
search '^        name: CodeQL gate$' "$codeql"
search '^distributionUrl=https://repo.maven.apache.org/maven2/org/apache/maven/apache-maven/3.9.16/apache-maven-3.9.16-bin.zip$' "$maven_wrapper"
search '^distributionSha256Sum=5af3b743dd8b876b5c45da33b676251e5f1687712644abb4ee519ca56e1d89ce$' "$maven_wrapper"
search '^distributionType=only-script$' "$maven_wrapper"
search '<artifactId>kotlin-markdown-core-jvm</artifactId>' "$maven_consumer"
search '<phase>verify</phase>' "$maven_consumer"
search 'Document.Companion.parse' packages/kotlin-markdown-core/consumers/jvm-maven/src/main/java/consumer/Main.java
search 'MAVEN_USER_HOME="\$root/build/maven-user-home"' scripts/check-kotlin-consumers.sh
search '"\$root/mvnw" --batch-mode --no-transfer-progress' scripts/check-kotlin-consumers.sh
search -- '-Dmaven.repo.local="\$repository"' scripts/check-kotlin-consumers.sh

if search 'pr-metrics|benchmark|binary.size' <(
    sed -n '/^    required-gates:/,$p' "$ci"
); then
    echo "non-blocking metrics leaked into the required gate" >&2
    exit 1
fi

if search '^    (issues|pull-requests|checks|statuses): write$' "$metrics"; then
    echo "the untrusted pull_request metrics workflow has write permission" >&2
    exit 1
fi
search '^    contents: read$' "$metrics"
search '^    workflow_run:$' "$comment"
search '^    issues: write$' "$comment"
search '^    pull-requests: write$' "$comment"
search 'listWorkflowRunArtifacts' "$comment"
search 'artifact\.size_in_bytes <= 65536' "$comment"
if search 'actions/checkout|github\.event\.pull_request\.head|gh pr checkout|git fetch' "$comment"; then
    echo "the privileged metrics commenter may not fetch or execute PR code" >&2
    exit 1
fi

node --input-type=module - "$ruleset" <<'NODE'
import fs from "node:fs";

const ruleset = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const required = ruleset.rules.find((rule) => rule.type === "required_status_checks");
const contexts = required?.parameters?.required_status_checks?.map((check) => check.context).sort();
const expected = ["CodeQL gate", "Required gates"];
if (JSON.stringify(contexts) !== JSON.stringify(expected)) {
    throw new Error(`ruleset required checks changed: ${JSON.stringify(contexts)}`);
}
if (ruleset.conditions?.ref_name?.include?.join(",") !== "~DEFAULT_BRANCH") {
    throw new Error("ruleset must target only the default branch");
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

if search '^    pull_request:$' .github/workflows/benchmark.yml; then
    echo "scheduled benchmark workflow must not become a pull-request gate" >&2
    exit 1
fi

echo "CI policy audit passed"
