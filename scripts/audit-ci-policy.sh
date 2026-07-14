#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

ci=.github/workflows/ci.yml
codeql=.github/workflows/codeql.yml
metrics=.github/workflows/pr-metrics.yml
comment=.github/workflows/pr-metrics-comment.yml
ruleset=.github/rulesets/main.json
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
    "$ruleset" \
    "$maven_wrapper" \
    "$maven_consumer" \
    mvnw \
    mvnw.cmd; do
    if [ ! -f "$required" ]; then
        echo "missing CI policy file: $required" >&2
        exit 1
    fi
done

for workflow in "$ci" "$codeql"; do
    if ! search '^    merge_group:$' "$workflow"; then
        echo "blocking workflow lacks merge_group support: $workflow" >&2
        exit 1
    fi
done

if ! search '^        branches: \[main\]$' <(
    sed -n '/^    push:$/,/^    merge_group:$/p' "$ci"
); then
    echo "blocking CI push trigger must be limited to main" >&2
    exit 1
fi

search '^    required-gates:$' "$ci"
search '^        name: Required gates$' "$ci"
search '^                  pnpm audit:repository:clean$' "$ci"
search '^            - name: Verify Kotlin publication consumers$' "$ci"
search '^              run: pnpm check:kotlin-consumers$' "$ci"
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

if search '^    pull_request:$' .github/workflows/benchmark.yml; then
    echo "scheduled benchmark workflow must not become a pull-request gate" >&2
    exit 1
fi

echo "CI policy audit passed"
