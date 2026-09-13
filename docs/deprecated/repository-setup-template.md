# Cross-platform Repository Setup Template

> Scope: repositories that maintain one core implementation and multiple platform bindings/packages,
> with unified PR quality gates, default-branch CI, a release dry run without credentials, and
> coordinated releases driven by protected SemVer tags.
>
> This document defines only repository control-plane and delivery contracts. It does not depend on
> product names, source contents, language combinations, package coordinates, or registries.
> Replace every `<...>` in the target repository; `scripts/repo` is the sole adapter it must implement.

## 1. Template Goals

After migration, the target repository should meet these goals:

1. Every binding consumes the core from the same commit; combining versions is prohibited.
2. Correctness, cross-platform conformance, consumer, and package-content verification remain
   independent, and all block PRs.
3. Rulesets depend on only two stable aggregate checks: `Required gates` and `CodeQL gate`.
4. Push, pull request, and merge queue events use separate concurrency lanes; pushes do not preempt
   required PR checks.
5. Benchmarks, coverage, binary size, and PR metrics provide observations only, without becoming
   required checks.
6. Release dry runs do not read environments, repository secrets, or long-lived signing keys.
7. Production releases are triggered only by immutable `vX.Y.Z` tags and rerun full CI on the tag snapshot.
8. Artifacts are built, audited, and tested by staged consumers before publishing permissions are
   granted; publishing jobs use minimum permissions.
9. All ecosystem packages and the GitHub Release come from the same tag and version. Published bytes
   are not assembled from historical CI runs.
10. The bootstrap script in this document can configure the GitHub control plane repeatedly and
    idempotently.

This template derives from the following structures already operating in this repository:

- [CI workflow](../.github/workflows/ci.yml)
- [CodeQL workflow](../.github/workflows/codeql.yml)
- [Release dry run](../.github/workflows/release-dry-run.yml)
- [Tag release](../.github/workflows/release.yml)
- [PR metrics producer](../.github/workflows/pr-metrics.yml) and
  [privileged commenter](../.github/workflows/pr-metrics-comment.yml)
- [Default-branch ruleset](../.github/rulesets/main.json),
  [release-tag ruleset](../.github/rulesets/release-tags.json), and
  [release environment policy](../.github/environments/release.json)

These files are design evidence, not generic templates to copy verbatim. Their language versions,
runners, package coordinates, actor IDs, artifact paths, and publishing order belong to this
repository's adapter.

## 2. Invariants, Adapter, and Extensions

### 2.1 Required Invariants

| Invariant | Reason |
| --- | --- |
| One commit, one version, coordinated releases | Prevent unverifiable core/binding combinations |
| The same canonical fixtures/spec drive conformance for all bindings | Prevent each platform's tests from proving only their own behavior |
| Every platform has a real consumer | Unit tests cannot establish that package metadata, linking, loaders, or exports work |
| Required checks have stable aggregate names | Changes to matrices, runners, or bindings should not require ruleset changes |
| Aggregate jobs use `if: always()` and explicitly require every dependency to be `success` | Cancellation, skipping, and failure must fail closed |
| Separate concurrency for PR/push/merge queue | Push runner teardown must not cancel or starve PR checks |
| Dry runs and production releases use the same staging/audit adapter | Prevent testing one pipeline and publishing through another |
| Releases verify the tag snapshot directly without querying old check runs | Avoid depending on mutable historical run timing for release correctness |
| Build/stage jobs have no publishing permissions | Compromised build scripts still cannot publish directly |
| The release environment accepts only SemVer tags | Manually triggered branch workflows must not obtain release secrets |

### 2.2 Adapter Required in the Target Repository

All workflows call only the root-level `scripts/repo`. It may delegate to Make, CMake, SwiftPM,
Gradle, pnpm, Cargo, Bazel, or other native commands, but workflows should not duplicate business logic.

```text
scripts/repo doctor --check <capability...>
scripts/repo format --check
scripts/repo lint
scripts/repo test <binding> <target>
scripts/repo conformance <binding> <target>
scripts/repo consumer <binding> <target> [--repository <path>]
scripts/repo audit repository|ci|tests|surface|packages
scripts/repo release check-version [--tag vX.Y.Z]
scripts/repo release stage <artifact-id> <output-dir>
scripts/repo release sign <input-dir> [--ephemeral]
scripts/repo release verify <input-dir> [--signed]
scripts/repo release publish <channel> <artifact-path>
scripts/repo release checksums <release-dir>
```

General adapter contract:

- All commands run noninteractively from the repository root.
- `--check`, test, conformance, consumer, audit, and stage commands return `0` on success and nonzero on failure.
- Normal CI, PRs, and dry runs must not read publishing credentials.
- Staging writes only to the specified output/build directory and does not modify tracked files.
- Staged artifacts must be consumable offline; consumers must not fall back to workspace sources.
- `release check-version --tag` must accept only `v<exact VERSION>`.
- Version checks cover the root version, every package manifest, consumer fixtures, release notes, and
  artifact metadata. Any drift fails the check.
- `release sign --ephemeral` may generate only a disposable key, destroyed afterward. It proves the
  signing mechanics and bundle structure, not the identity of the production key.
- Publishing consumes bytes that have already been staged, verified, and consumer-tested. Publishing
  jobs must not rebuild them.
- Every adapter command should run locally. GitHub Actions is the scheduler, not the sole implementation.

### 2.3 Optional Extensions

Platforms, registries, and sanitizers that do not apply may be omitted, but declared platform support
must not be removed from verification. Typical extensions include:

- ASan, UBSan, TSan, fuzzing, and ABI/API compatibility.
- iOS Simulator, Android Emulator, Windows, Linux, and macOS deployment target matrices.
- npm, Maven Central, PyPI, crates.io, NuGet, and GitHub Packages.
- Source archives, binary archives, XCFrameworks, WASM, and JNI/native payloads.
- Benchmarks, coverage, binary size, and performance regression comments.

## 3. Recommended Repository Layout

```text
.
├── .github/
│   ├── environments/
│   │   ├── release.json                 # Auditable recipe; no secret values
│   │   └── release-tag-policy.json
│   ├── rulesets/
│   │   ├── main.json
│   │   └── release-tags.json
│   └── workflows/
│       ├── ci.yml                       # reusable + PR/push/merge_group
│       ├── codeql.yml                   # Independent security gate
│       ├── pr-metrics.yml               # Untrusted producer; optional
│       ├── pr-metrics-comment.yml       # Privileged workflow_run consumer; optional
│       ├── benchmark.yml                # Scheduled/manual; optional
│       ├── release-dry-run.yml           # PR/manual; no secrets
│       └── release.yml                   # protected tag only
├── docs/
│   ├── releasing.md                     # People, key rotation, recovery runbook
│   └── toolchains.md                    # Exact toolchains and runner matrix
├── packages/
│   ├── core/
│   ├── <binding-a>/
│   ├── <binding-b>/
│   └── <binding-c>/
├── specs/                               # canonical public contract
├── scripts/
│   ├── repo                             # Sole workflow adapter
│   ├── audit-ci-policy                  # Checks the invariants in this document
│   └── <internal-implementation-scripts>
├── tests/
│   └── consumers/                       # Consumes staged artifacts as published
├── VERSION                              # Single canonical SemVer; recommended
└── <root-package/build-manifests>
```

If an ecosystem requires a root manifest, such as a SwiftPM tag package, retain it there. Directory
layout is not the goal; invariants and command contracts are.

## 4. Cross-platform Binding Verification Model

### 4.1 Binding Inventory

Complete this table before migration. Targets not listed must not be claimed as supported:

| Binding | Host/build target | Correctness | Conformance | Consumer | Release artifact |
| --- | --- | --- | --- | --- | --- |
| `<core>` | `<linux/macos/windows>` | Required | canonical source | install/link/run | `<archive/package>` |
| `<binding-a>` | `<targets>` | Required | shared fixtures | clean external project | `<registry/source>` |
| `<binding-b>` | `<targets>` | Required | shared fixtures | clean external project | `<registry/binary>` |
| `<binding-c>` | `<targets>` | Required | shared fixtures | pack/install/import | `<registry/binary>` |

### 4.2 Keep Four Kinds of Evidence Separate

1. **Correctness**: implementation behavior, error paths, boundaries, and regressions.
2. **Conformance**: every binding produces the same semantics for the same input, options, schema,
   and expected output.
3. **Consumer**: install/resolve dependencies from staged artifacts, build, link/load/import, and run
   a minimal example.
4. **Package audit**: inspect allowlists/denylists, metadata, licenses, checksums, signatures, and files
   that must not leak into packages.

A `consumer` must not bypass staged artifacts through workspace dependencies, composite builds,
source-relative paths, unpublished targets, or a developer machine's global cache. Run it in a
temporary directory, an isolated dependency cache, or an explicit local registry.

### 4.3 Canonical Conformance

Shared conformance must fix at least:

- Schema/version.
- Input bytes and encoding.
- Parse/config options.
- Node or data-structure order, nullability, defaults, and error semantics.
- Deterministic serialization/dumps.
- Unicode, newline, locale, and platform-path normalization.
- Non-default, empty, null, and boundary fixtures for every field.

Intentional public behavior changes must update the schema, core, every binding, fixtures, goldens,
and consumers in the same reviewed commit. Do not add normalization that silently hides drift.

## 5. PR Quality Gates

### 5.1 `ci.yml` Triggers and Concurrency

```yaml
name: CI

on:
  workflow_call:
  pull_request:
  push:
    branches: ["**"]
  merge_group:
  workflow_dispatch:

permissions:
  contents: read

concurrency:
  group: ci-${{ github.event_name }}-${{ github.event.pull_request.number || github.ref }}
  cancel-in-progress: true
```

Do not deduplicate across events by SHA. A push and a pull request with the same SHA belong to
separate control planes. A canceled push runner or one with slow teardown must not prevent the
required PR run from starting.

### 5.2 Blocking Job Layers

Recommended job layers:

| Layer | Required contents |
| --- | --- |
| Hygiene | Frozen dependency installation, formatting checks, lint, contract audits, repository cleanliness |
| Core | Main host/compiler/build-mode matrix, correctness, conformance |
| Binding | Correctness and conformance for every declared platform |
| Deployment | Minimum/maximum deployment targets, ABI/loader/packaging targets |
| Consumer | Staged/installed consumers for every distribution ecosystem |
| Package audit | Actual packed/archive/publication contents and metadata |
| Runtime safety | Sanitizer, race, emulator, or browser runtime checks appropriate to the project's risks |

Use `fail-fast: false` in matrices so one CI run reveals the full failure surface. Pin runners and
toolchains in `docs/toolchains.md`; separate toolchain upgrades from product behavior changes where possible.

### 5.3 The Single Stable Aggregate Check

```yaml
  required-gates:
    name: ${{ (github.event_name == 'pull_request' || github.event_name == 'merge_group') && 'Required gates' || 'Development branch gates' }}
    if: ${{ always() }}
    needs:
      - hygiene
      - core
      - bindings
      - consumers
      - package-audit
    runs-on: ubuntu-latest
    steps:
      - name: Require every blocking CI job
        env:
          HYGIENE: ${{ needs.hygiene.result }}
          CORE: ${{ needs.core.result }}
          BINDINGS: ${{ needs.bindings.result }}
          CONSUMERS: ${{ needs.consumers.result }}
          PACKAGE_AUDIT: ${{ needs.package-audit.result }}
        run: |
          for result in "$HYGIENE" "$CORE" "$BINDINGS" "$CONSUMERS" "$PACKAGE_AUDIT"
          do
            if [ "$result" != success ]; then
              echo "A required CI dependency concluded: $result" >&2
              exit 1
            fi
          done
```

Rules:

- Every blocking job appears in both `needs` and the explicit result checks.
- Do not use permissive expressions such as `contains(needs.*.result, 'failure')` that miss skipped or
  canceled jobs.
- The push aggregate must be named `Development branch gates`; it must not produce the ruleset's
  required `Required gates` context.
- Rulesets do not reference expanded matrix job names directly.

### 5.4 CodeQL

CodeQL runs independently on `main` PRs, `main` pushes, merge queues, and schedules. Choose the correct
build mode for every product language and use the same fail-closed aggregation:

```yaml
  codeql-gate:
    name: CodeQL gate
    if: ${{ always() }}
    needs: analyze
    runs-on: ubuntu-latest
    permissions:
      contents: read
    steps:
      - env:
          RESULT: ${{ needs.analyze.result }}
        run: test "$RESULT" = success
```

Only CodeQL analysis jobs receive `security-events: write`. If the repository or GitHub plan does not
support a language, resolve that before activating the ruleset. Do not leave the required
`CodeQL gate` as a context that can never appear.

### 5.5 Default-branch Ruleset

`main quality gates` requires only:

- `Required gates`
- `CodeQL gate`

Also enable:

- No default-branch deletion.
- No non-fast-forward updates.
- PRs for every change.
- Strict required status checks: PRs must be based on the latest default branch.
- Approvals, CODEOWNERS, last-push approval, and thread resolution as required by team policy.

Do not add benchmarks, metrics, coverage, release dry runs, or unstable matrix job names to the ruleset.

## 6. Main CI and Nonblocking Observations

The same `ci.yml` serves PRs, merge queues, all development-branch pushes, default-branch pushes, and
release `workflow_call` events. This prevents local commands, PRs, main, and tags from developing four
independently drifting test definitions.

Main CI establishes default-branch health and traceable evidence. Production releases do not query or
reuse that run.

Place the following in separate scheduled/manual workflows:

- Benchmarks and long-running fuzzing.
- Coverage trends and binary size.
- Dependency freshness.
- Cross-version compatibility scans.
- Unstable or expensive platforms that are not yet declared as supported for releases.

### 6.1 Fork-safe PR Metrics

PR comments require two separate workflows:

1. A `pull_request` producer with `contents: read` runs untrusted PR code and uploads only a small
   JSON artifact.
2. A `workflow_run` consumer may have `pull-requests: write`, but **does not check out or execute PR
   code**. It handles artifacts strictly as data.

Before downloading, the privileged consumer validates artifact names, count, individual and total
size, and expiry. After parsing, it also validates the schema, PR association, head SHA, allowed
platform/metric enums, and numeric ranges. A hidden marker identifies one comment to update instead
of repeatedly posting. Missing or invalid artifacts produce only notices/warnings and cannot block PRs.

## 7. Release Dry Run

`release-dry-run.yml` runs on PRs and manual triggers. Its top level grants only `contents: read`, and
it must not declare an `environment`.

Recommended order:

1. Validate the coordinated version, manifests, and tag namespace without requiring a tag to exist yet.
2. Explicitly prove that expected release-secret environment variables are empty.
3. Stage real release artifacts independently on each runner.
4. Upload artifacts. The aggregate job downloads only those artifacts, without taking existing build
   outputs from the workspace.
5. For ecosystems requiring signatures, use disposable keys for detached signatures, checksums, and
   bundle audits.
6. Run every consumer against staged artifacts.
7. `Release dry-run gate` uses `if: always()` and requires every stage/aggregate job to be `success`.

A dry run proves the artifact graph, signing mechanics, consumers, and content audits, but cannot prove:

- Production registry credentials are valid.
- Protected-environment reviewer workflows work.
- Trusted publishers for npm/PyPI or other registries are configured correctly.
- The production PGP identity or public key is retrievable.
- External ownership of package names/namespaces is established.

## 8. Tag-based Releases

### 8.1 Tags, Versions, and Snapshots

The workflow trigger may use GitHub's `v*.*.*` glob, but a glob is not a SemVer regex. The validation
job must therefore perform strict checks again:

```text
tag == "v" + VERSION
VERSION == strict SemVer accepted by the repository
all package manifests == VERSION
all consumer fixtures == VERSION
docs/releases/VERSION.md exists and is non-empty
tag commit is reachable from origin/<default-branch>
```

Tags must prohibit updates and deletion. A failed tag remains an immutable release attempt. Any
byte-level fix requires a new SemVer; the original tag must not be moved or reused.

### 8.2 Release DAG

```text
protected vX.Y.Z tag
        │
        ▼
validate exact tag/version/notes
        │
        ▼
reusable full CI on tag snapshot
        │
        ├──────────────┬──────────────┐
        ▼              ▼              ▼
 stage platform A  stage platform B  stage registry packages
        └──────────────┴──────────────┘
                       │
                       ▼
          aggregate/sign/audit/consumers
                       │
            protected release environment
                       │
                       ▼
          registry publish in explicit order
                       │
                       ▼
       checksums + provenance + GitHub Release
```

Key rules:

- `quality` runs the full suite on the tag checkout using `uses: ./.github/workflows/ci.yml`.
- CodeQL remains a PR/default-branch gate. Releases do not wait for or query historical CodeQL runs.
- Staging jobs have only `contents: read` and do not enter the release environment.
- The first job that needs signing/publishing credentials enters the `release` environment.
- Prefer OIDC trusted publishing for supported ecosystems such as npm/PyPI, without storing
  long-lived write tokens.
- Only the npm publishing job receives `id-token: write`; only the GitHub Release job receives
  `contents: write` and `attestations: write`.
- Release jobs download staged artifacts and cannot rebuild them.
- Validate the inputs for every irreversible operation before the first publish operation.
- Multiple registries cannot commit atomically, so document ordering, partial failures, and recovery.

### 8.3 Recommended Publishing Order

1. Build/stage/audit every artifact.
2. Aggregate outputs across hosts, sign them, generate ecosystem-required checksums, and run staged consumers.
3. For registries supporting upload-and-validate before publication, upload first and wait for `VALIDATED`.
4. Publish packages to OIDC registries.
5. Publish validated deployments that are not yet public, and wait for `PUBLISHED`.
6. Generate aggregate release checksums and provenance attestations.
7. Create the GitHub Release with manually maintained release notes.

Do not automatically generate user-facing release notes or include internal phases, acceptance logs,
or CI transcripts in them.

### 8.4 Recovery

If a release can fail after succeeding in some registries, the workflow may provide manual recovery,
but it must require all of the following:

- An existing, protected `release-tag`.
- Prefer rerunning failed jobs in the original tag workflow when the remaining operations are provably
  idempotent. Use dispatch only when the original run cannot be recovered safely.
- With dispatch, the workflow's triggering ref itself must be `refs/tags/<release-tag>`, explicitly
  checked in the job. Checking out a tag in a step does not change the triggering ref seen by the
  environment policy.
- The `source-run-id` that produced the verified artifacts.
- Matching source-run workflow, event, head SHA, tag, and artifact allowlist. Its overall conclusion
  must match the documented failure stage, and every artifact-producing job must have succeeded.
- Unexpired artifacts whose names, count, sizes, and digests satisfy policy.
- Recovery checks out the tag, not the default branch.
- Already published registries are skipped after querying and confirming the same version/digest.
  They must not be republished.
- Every resumed publishing action still passes through the `release` environment reviewer.

Changes to artifact bytes, signatures, or version metadata are not recovery and require a new SemVer.

## 9. Permissions and Secret Boundaries

### 9.1 Default Permissions

Set repository Actions defaults to:

```json
{
  "default_workflow_permissions": "read",
  "can_approve_pull_request_reviews": false
}
```

Each job elevates only the permissions it needs:

| Job | Minimum permissions |
| --- | --- |
| build/test/stage | `contents: read` |
| CodeQL analyze | `contents: read`, `actions: read`, `security-events: write` |
| PR metrics commenter | `actions: read`, `contents: read`, `issues: write`, `pull-requests: write` |
| OIDC registry publish | `contents: read`, `id-token: write` |
| GitHub Release/attestation | `contents: write`, `id-token: write`, `attestations: write` |

### 9.2 Secret Categories

| Type | Location | Rules |
| --- | --- | --- |
| OIDC trusted publisher | External registry configuration | GitHub stores no write token |
| Registry token | `release` environment secret | Minimum scope, expiration, documented owner and rotation date |
| PGP/private signing key | `release` environment secret | Passphrase, offline backup, revocation certificate |
| Public key/certificate | Public key server/repository documentation | Verify retrievability before publishing |
| GitHub Release token | Workflow `GITHUB_TOKEN` | Do not create a long-lived PAT |

Never put secret values in recipe JSON, repository variables, Gradle properties, `.npmrc`, logs,
artifacts, or documentation. Fork PRs, normal CI, dry runs, and staging jobs must never receive release
environment secrets.

## 10. One-command GitHub Control-plane Bootstrap

### 10.1 Prerequisites

- The target repository exists, and its workflows and adapter have passed at least one manual/PR run.
- The operator has repository admin access, an installed and authenticated `gh`, and local `jq`.
- A release reviewer has been chosen. If there is only one release operator, self-review prevention
  cannot also be enabled.
- The script below configures only the GitHub control plane. It does not upload secrets or configure
  external registry ownership/trusted publishers.
- Replace the `User` actor in the script when organization rulesets, team reviewers, or enterprise
  policies require it.

### 10.2 Bootstrap Script

Save this script as `scripts/bootstrap-repository.sh` in the target repository and review it before
running it. It idempotently updates rulesets with matching names, creates/updates the `release`
environment, and ensures there is exactly one release-tag deployment policy.

```bash
#!/usr/bin/env bash
set -euo pipefail

: "${GH_REPO:?set GH_REPO=owner/repository}"
: "${RELEASE_REVIEWER:?set RELEASE_REVIEWER to a GitHub login}"

DEFAULT_BRANCH=${DEFAULT_BRANCH:-main}
RELEASE_ENVIRONMENT=${RELEASE_ENVIRONMENT:-release}
MAIN_RULESET_NAME=${MAIN_RULESET_NAME:-main quality gates}
TAG_RULESET_NAME=${TAG_RULESET_NAME:-release tag protection}
TAG_PATTERN=${TAG_PATTERN:-v*.*.*}
RULESET_ENFORCEMENT=${RULESET_ENFORCEMENT:-evaluate}
PREVENT_SELF_REVIEW=${PREVENT_SELF_REVIEW:-false}

case "$RULESET_ENFORCEMENT" in
  disabled|evaluate|active) ;;
  *) echo "RULESET_ENFORCEMENT must be disabled, evaluate, or active" >&2; exit 2 ;;
esac

case "$PREVENT_SELF_REVIEW" in
  true|false) ;;
  *) echo "PREVENT_SELF_REVIEW must be true or false" >&2; exit 2 ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

reviewer_id=$(gh api "users/$RELEASE_REVIEWER" --jq .id)
encoded_environment=$(jq -rn --arg value "$RELEASE_ENVIRONMENT" '$value | @uri')
actual_default=$(gh api "repos/$GH_REPO" --jq .default_branch)
if [ "$actual_default" != "$DEFAULT_BRANCH" ]; then
  echo "default branch is '$actual_default', expected '$DEFAULT_BRANCH'" >&2
  exit 1
fi

upsert_ruleset() {
  local name=$1
  local payload=$2
  local ids
  local count

  ids=$(gh api "repos/$GH_REPO/rulesets" --paginate \
    --jq ".[] | select(.name == \"$name\") | .id")
  count=$(printf '%s\n' "$ids" | sed '/^$/d' | wc -l | tr -d ' ')

  if [ "$count" -gt 1 ]; then
    echo "multiple rulesets named '$name'; refusing ambiguous update" >&2
    exit 1
  elif [ "$count" -eq 1 ]; then
    gh api --method PUT "repos/$GH_REPO/rulesets/$ids" --input "$payload" >/dev/null
  else
    gh api --method POST "repos/$GH_REPO/rulesets" --input "$payload" >/dev/null
  fi
}

jq -n '{
  default_workflow_permissions: "read",
  can_approve_pull_request_reviews: false
}' >"$tmp/workflow-permissions.json"
gh api --method PUT "repos/$GH_REPO/actions/permissions/workflow" \
  --input "$tmp/workflow-permissions.json" >/dev/null

jq -n \
  --argjson reviewer_id "$reviewer_id" \
  --argjson prevent_self_review "$PREVENT_SELF_REVIEW" '{
    wait_timer: 0,
    prevent_self_review: $prevent_self_review,
    reviewers: [{type: "User", id: $reviewer_id}],
    deployment_branch_policy: {
      protected_branches: false,
      custom_branch_policies: true
    }
  }' >"$tmp/release-environment.json"
gh api --method PUT \
  "repos/$GH_REPO/environments/$encoded_environment" \
  --input "$tmp/release-environment.json" >/dev/null

matching_policies=$(gh api \
  "repos/$GH_REPO/environments/$encoded_environment/deployment-branch-policies" \
  --jq ".branch_policies[] | select(.name == \"$TAG_PATTERN\" and .type == \"tag\") | .id")
matching_count=$(printf '%s\n' "$matching_policies" | sed '/^$/d' | wc -l | tr -d ' ')
if [ "$matching_count" -eq 0 ]; then
  jq -n --arg name "$TAG_PATTERN" '{name: $name, type: "tag"}' \
    >"$tmp/release-tag-policy.json"
  gh api --method POST \
    "repos/$GH_REPO/environments/$encoded_environment/deployment-branch-policies" \
    --input "$tmp/release-tag-policy.json" >/dev/null
elif [ "$matching_count" -gt 1 ]; then
  echo "multiple identical release tag policies; clean them up before continuing" >&2
  exit 1
fi

policy_count=$(gh api \
  "repos/$GH_REPO/environments/$encoded_environment/deployment-branch-policies" \
  --jq '.branch_policies | length')
if [ "$policy_count" -ne 1 ]; then
  echo "release environment has $policy_count deployment policies; expected exactly one" >&2
  echo "remove unrelated branch/tag policies before activation" >&2
  exit 1
fi

jq -n \
  --arg name "$MAIN_RULESET_NAME" \
  --arg enforcement "$RULESET_ENFORCEMENT" '{
    name: $name,
    target: "branch",
    enforcement: $enforcement,
    conditions: {ref_name: {exclude: [], include: ["~DEFAULT_BRANCH"]}},
    rules: [
      {type: "deletion"},
      {type: "non_fast_forward"},
      {type: "pull_request", parameters: {
        require_code_owner_review: false,
        require_last_push_approval: false,
        dismiss_stale_reviews_on_push: false,
        required_approving_review_count: 0,
        required_review_thread_resolution: false
      }},
      {type: "required_status_checks", parameters: {
        do_not_enforce_on_create: true,
        strict_required_status_checks_policy: true,
        required_status_checks: [
          {context: "Required gates"},
          {context: "CodeQL gate"}
        ]
      }}
    ],
    bypass_actors: []
  }' >"$tmp/main-ruleset.json"
upsert_ruleset "$MAIN_RULESET_NAME" "$tmp/main-ruleset.json"

jq -n \
  --arg name "$TAG_RULESET_NAME" \
  --arg enforcement "$RULESET_ENFORCEMENT" \
  --arg pattern "refs/tags/$TAG_PATTERN" \
  --argjson reviewer_id "$reviewer_id" '{
    name: $name,
    target: "tag",
    enforcement: $enforcement,
    conditions: {ref_name: {exclude: [], include: [$pattern]}},
    rules: [
      {type: "creation"},
      {type: "update"},
      {type: "deletion"}
    ],
    bypass_actors: [{
      actor_id: $reviewer_id,
      actor_type: "User",
      bypass_mode: "always"
    }]
  }' >"$tmp/release-tag-ruleset.json"
upsert_ruleset "$TAG_RULESET_NAME" "$tmp/release-tag-ruleset.json"

echo "Repository control plane reconciled with enforcement=$RULESET_ENFORCEMENT"
```

Use evaluate mode for the initial migration:

```sh
GH_REPO=<owner/repo> \
RELEASE_REVIEWER=<login> \
RULESET_ENFORCEMENT=evaluate \
scripts/bootstrap-repository.sh
```

After completing the remote verification in Section 12, use the same command to switch both rulesets
to the target active state:

```sh
GH_REPO=<owner/repo> \
RELEASE_REVIEWER=<login> \
RULESET_ENFORCEMENT=active \
scripts/bootstrap-repository.sh
```

If the GitHub plan does not offer evaluate mode, use `RULESET_ENFORCEMENT=disabled` initially and
switch directly to active after verification. If only the reviewer can publish,
`PREVENT_SELF_REVIEW=true` creates a deadlock; enable it only after adding an independent reviewer/team.
The script does not delete unknown deployment policies; it fails closed when extra policies exist.

## 11. One-time External Registry Setup

GitHub repository bootstrap cannot safely perform this part on your behalf.

### 11.1 Record for Every Registry

- Namespace/package owner and proof of ownership.
- Exact publishing workflow filename, repository, and environment.
- Credential/trusted-publisher permissions and expiration.
- Signing-key UID, fingerprint, expiry, and public-key URL.
- Owners of the offline backup and revocation certificate.
- Token/key rotation and compromise-response runbooks.
- Whether the initial bootstrap publish requires manual 2FA, and how to revoke the temporary
  session/token afterward.

### 11.2 Environment Secrets

Set secrets only for registries without OIDC support, using names chosen for the target repository:

```sh
gh secret set <REGISTRY_USERNAME> --repo <owner/repo> --env release
gh secret set <REGISTRY_PASSWORD> --repo <owner/repo> --env release
gh secret set <SIGNING_PRIVATE_KEY> --repo <owner/repo> --env release
gh secret set <SIGNING_PASSWORD> --repo <owner/repo> --env release
gh secret list --repo <owner/repo> --env release
```

Do not expand secrets directly on the shell command line. Let `gh secret set` read interactive input
or stdin. Documentation records secret names, never values.

## 12. Migration and Acceptance Runbook

### 12.1 Local Migration

- [ ] Complete the binding/target/artifact inventory.
- [ ] Implement every applicable `scripts/repo` command.
- [ ] Pin toolchains, wrappers, lockfiles, and runners.
- [ ] Run formatting, lint, correctness, conformance, and consumers from a clean checkout.
- [ ] Run consumers against staged artifacts and prove there is no workspace fallback.
- [ ] Run package audits for licenses, metadata, and allowlists/denylists.
- [ ] Run a release dry run, proving release secrets are empty and disposable signing succeeds.
- [ ] Audit workflow permissions, triggers, concurrency, stable gate names, and the release DAG.

### 12.2 Remote Verification in Evaluate Mode

- [ ] Run bootstrap with `RULESET_ENFORCEMENT=evaluate`.
- [ ] Create a test PR and confirm only the PR run produces `Required gates`.
- [ ] Push to the same branch concurrently and confirm its aggregate is `Development branch gates`.
- [ ] Confirm `Required gates` covers every blocking job and fails when a job is canceled/skipped.
- [ ] Confirm `CodeQL gate` turns green only after every CodeQL language succeeds.
- [ ] If using a merge queue, trigger a real `merge_group` and confirm both required contexts.
- [ ] Confirm metrics/benchmarks/dry runs are absent from the ruleset.
- [ ] Confirm fork PRs have no write token, environment secrets, or privileged code execution.
- [ ] Run the remote release dry run, then download and independently inspect its artifacts.

### 12.3 Activation and Release Rehearsal

- [ ] Run bootstrap again with `RULESET_ENFORCEMENT=active`.
- [ ] Prove PR bypasses, missing required checks, outdated branches, and force pushes are blocked.
- [ ] Prove non-SemVer tags fail validation even when they match the permissive glob.
- [ ] Prove the release environment has exactly one `v*.*.*` tag policy.
- [ ] Prove ordinary branch/manual workflows cannot access the release environment.
- [ ] Complete an end-to-end rehearsal with an unpublished test version or disposable registry.
- [ ] Verify registry provenance, PGP signatures, SHA-256/SHA-512, and GitHub attestations.
- [ ] Verify the GitHub Release, every registry, and source tag refer to the same commit/version/digest.
- [ ] Rehearse partial-failure recovery. Confirm recovery uses a protected tag as its triggering ref
      without temporarily allowing the default branch into the environment, moving tags, overwriting
      versions, or rebuilding artifacts.

### 12.4 Live Policy Queries

```sh
gh api repos/<owner>/<repo>/actions/permissions/workflow
gh api repos/<owner>/<repo>/rulesets
gh api repos/<owner>/<repo>/environments/release
gh api repos/<owner>/<repo>/environments/release/deployment-branch-policies
gh secret list --repo <owner/repo> --env release
```

Checked-in JSON is a recipe; GitHub's live API reports actual enforcement. Whenever rulesets, reviewers,
tag policies, workflow permissions, or secret names change, update the recipes, CI policy audit, and
this runbook together.

## 13. Policies CI Must Audit

`scripts/repo audit ci` checks at least:

- `ci.yml` declares `workflow_call`, `pull_request`, push, and `merge_group`.
- Concurrency includes the event name and PR number/ref, with `cancel-in-progress: true`.
- PR/merge queue aggregates use `Required gates`; pushes use `Development branch gates`.
- Aggregate jobs use `if: always()` and cover every blocking dependency.
- `CodeQL gate` exists and fails closed.
- Ruleset required contexts are exactly `Required gates` and `CodeQL gate`.
- Release dry runs have no environment, write permissions, or secret references.
- Releases accept only tag pushes; if recovery dispatch remains, its jobs are strictly isolated from
  the normal publishing DAG.
- Release validation strictly checks tags, versions, notes, and default-branch ancestry.
- Releases run local reusable CI on the tag snapshot without querying historical check runs.
- Build/stage jobs stay outside the release environment; publishing jobs enter it.
- OIDC, contents-write, and attestations-write permissions appear only where needed.
- Tag rulesets restrict creation/update/deletion, and the environment accepts only one SemVer tag policy.
- Workflow action references follow organization-approved pinning policy and are maintained by
  dependency-update tooling.

Policy audits should verify security outcomes and data flow without freezing irrelevant YAML layout,
job ordering, or the current Action major version.

## 14. Lessons and Design Rationale

This section records causal lessons from real setup, PR verification, and release rehearsals. Teams
may replace specific toolchains and platforms, but should not remove these constraints without
equivalent evidence.

New lessons should record the date/phase, observable symptoms, root cause, rejected superficial fixes,
permanent rule, and regression verification. Detailed runs/PRs may remain in each project's migration
or ADR documents; this template keeps only conclusions reusable across repositories. Never copy
tokens, private keys, internal credential commands, or other secrets into these records.

### 14.1 Share Semantic Contracts without Forcing Symmetric Binding Shapes

**Failed approach**: copying one platform's directories, APIs, builds, exceptions, memory model, or
publishing conventions to other platforms to make bindings look uniform. Workspace unit tests may
pass quickly, but IDE imports, deployment targets, package metadata, native loaders, external
consumers, or registry publication then fail.

**Rule**: unify public semantics, type meanings, nullability, error categories, fixtures, and versions.
Each platform's public API, build, and publishing shape must follow its ecosystem's best practices.

| Platform family | Native practices to follow | Inadequate substitutes |
| --- | --- | --- |
| C/C++ | CMake configure/build/install/export, CTest suites, shared/static builds, compiler/OS matrices, sanitizers, real linking consumers | Linking only private targets inside the source tree |
| Swift/Apple | SwiftPM package/product identity, declared deployment targets, native Swift value/error/concurrency models, macOS/iOS Simulator, external package consumers | Exposing C pointers/lifetimes to users or testing only on a macOS host |
| Kotlin/KMP | Repository-owned Gradle Wrapper, Java toolchains, KMP target publications, Gradle Module Metadata, Android/JVM/Native consumers, IDE model smoke tests, task-lazy signing/publishing configuration | Proving only JVM unit tests or reading publishing credentials during IDE sync |
| ES/TypeScript/WASM | Install after `npm pack`, strict `exports`/types/files, Node and browser runtimes, WASM loaders, ESM semantics, OIDC trusted publishing | Workspace links, running sources directly, or TypeScript compilation alone |

**Verification**: every declared platform provides at least one clean consumer using its standard
package manager/build system. Consumers can see only staged/public artifacts, not private repository
targets or source-relative paths.

### 14.2 SHA Is an Audit Identity, Not a Scheduling Identity

**Failed approach**: the source repository for this template tried deduplicating `push`,
`pull_request`, `merge_group`, and tag events by commit SHA, and having releases query historical
PR/main checks by tag SHA. This created two cycles:

1. A canceled push run with the same SHA remained in `always()` aggregation or runner teardown. Its
   shared concurrency group prevented the PR run from starting while the ruleset waited for the
   required PR check.
2. A release required an existing branch/PR check for its tag SHA, but the tag event did not produce
   that context. The tag was also needed to start release verification, leaving validation dependent
   on a historical run that might not exist, might have expired, or might belong to another event.

**Rules**:

- Record SHA in artifact metadata, provenance, attestations, and logs to identify the verified bytes.
- Never use SHA as a cross-event concurrency key; use `event_name + PR number/ref`.
- Only PRs/merge queues own `Required gates`; pushes use `Development branch gates`.
- Tag releases invoke reusable CI directly from the immutable tag to reverify the current snapshot.
- Merge-time gates such as CodeQL may remain on PR/main. Releases do not query or await their
  historical runs by SHA.
- Recovery may reference a source run, but must verify its workflow, event, tag, head SHA, conclusion,
  artifact digests, and allowlist. Matching SHA alone does not establish trust.

**Verification**: trigger push and PR runs for the same commit and deliberately cancel one. The PR
gate must still start and finish independently. Hiding or deleting historical branch checks when
creating a release candidate must not affect the tag snapshot's full build/test run, while the source
tag and artifact provenance must still identify the exact SHA.

### 14.3 Stable Gate Names Are a Repository Control-plane API

**Failed approach**: rulesets directly referenced matrix leaf jobs, or both push and PR runs produced
the same required context. Adding runners, renaming bindings, matrix fail-fast behavior, or push
cancellation could permanently block merging or let a green check from the wrong event serve as PR evidence.

**Rule**: rulesets know only `Required gates` and `CodeQL gate`. Leaf jobs may evolve, but aggregate
jobs must use `if: always()` and require every blocking dependency to be exactly `success`. Stable
check names form an API between workflows and GitHub rulesets; renaming them is a breaking
control-plane change.

**Verification**: make a leaf job fail, cancel, and skip. All three cases must leave a failed stable
gate. Adding a matrix entry must not require changing the live ruleset.

### 14.4 A Successful Build Does Not Prove a Package Is Consumable

**Failed approach**: running only workspace builds/unit tests and assuming packages, archives, or
binaries must work. This misses problems in exports, POM/module metadata, source archives, headers,
pkg-config/CMake exports, native payloads, loaders, licenses, and accidentally packaged private files.

**Rule**: first stage/pack/publish each ecosystem's artifacts to an isolated local repository. Then
install them from a clean consumer, build/link/load/import, and execute a minimal public API example.
Package-content audits and consumers are blocking evidence that unit tests cannot replace.

**Verification**: temporarily remove workspace/composite dependencies and developer caches. The check
passes only if the consumer still runs successfully from staged artifacts and the package audit finds
only allowlisted contents.

### 14.5 IDE/Model Loading Is Part of Delivery

**Failed approach**: treating green command-line compilation as proof that Gradle/KMP, Xcode/SwiftPM,
or other IDE projects work. Eager native builds, configuration-time signing, missing SDKs, preview
toolchain warnings, or release-secret reads may surface only during clean imports/model loading.

**Rule**: IDE sync/model loading must succeed without credentials, prebuilt native binaries, or
executing publishing tasks. Expensive packaging, signing, and upload operations are configured and
run only within explicit execution tasks. Before release, perform a clean import in a supported
stable IDE/toolchain and retain headless model smoke tests in CI.

### 14.6 Dry Runs Must Be Realistic without Production Permissions

**Failed approach**: entering production environments or reading real secrets during a dry run, or
merely invoking a registry's `--dry-run` without real staging, signing, aggregation, and consumers.
The former expands PR permissions; the latter cannot prove the bytes to be published.

**Rule**: dry runs and releases share staging/audit adapters. Disposable signing keys produce the full
artifacts, checksums, signatures, and consumer evidence, but top-level permissions remain
`contents: read`, with no environments or secrets. Rehearse registry ownership, production key
identity, and credentials separately.

### 14.7 Separate Untrusted Computation from Write Permissions

**Failed approach**: checking out or executing PR code in a workflow with a write token to post
benchmark/size comments, or directly trusting artifacts uploaded by PRs.

**Rule**: a read-only `pull_request` producer executes untrusted code. The `workflow_run` consumer
only parses data validated against name/count/size/schema/SHA/PR-association allowlists. It does not
check out or execute PR or artifact contents. Metrics never enter required gates.

### 14.8 Failed Releases Are Immutable History, Not Overwritable Drafts

**Failed approach**: rebuilding after some registries succeeded, moving tags, overwriting the same
version, or recovering with new scripts/bytes from the default branch. Another observed trap was
triggering `workflow_dispatch` from `main`, then checking out a release tag in a step. GitHub evaluates
environment policy against the dispatch ref, not the checkout ref. Temporarily allowing `main` into
the release environment to unblock recovery expands credential exposure. These approaches break the
shared identity of provenance, checksums, permission boundaries, and registry contents.

**Rule**: retain failed tags as immutable release attempts. Recovery reuses verified artifacts only;
check version/digest before skipping registries that already published. Prefer rerunning the original
tag run when remaining jobs are provably idempotent. If dispatch is necessary, use a protected tag as
the dispatch ref and check `github.ref == refs/tags/<release-tag>`. Do not temporarily open the release
environment to the default branch. Any byte, metadata, or signature change requires a new SemVer.
Rehearse multi-registry ordering and irreversible steps before the first production release.

### 14.9 Policy Audits Should Freeze Outcomes, Not Incidental Implementations

**Failed approach**: hardcoding Action majors, YAML line order, job layout, or temporary toolchain
names in policy audits. Routine dependency upgrades produce false alarms while actual permission,
trigger, data-flow, or gate drift may go unnoticed.

**Rule**: audit security outcomes: triggers, permissions, environment/secret boundaries, stable gates,
fail-closed aggregation, tag-snapshot verification, artifact flow, and live ruleset contexts. Manage
Action references under organization-approved pinning policy without making the current major an
eternal business rule.

### 14.10 Activate Rulesets Last

**Failed approach**: enabling active rulesets before workflows produce stable remote contexts, or
enabling self-review prevention when there is only one release operator, locking the default branch
or release environment.

**Rule**: land workflows first, run real PRs, merge queues, and dry runs in evaluate/disabled mode,
then activate enforcement. Reviewer, bypass-actor, and self-review policies must leave at least one
reviewed recovery path.

### 14.11 Quick Antipattern Index

| Mistake | Consequence | Fix |
| --- | --- | --- |
| Copying another platform's build/API shape for cross-platform consistency | Packages, IDEs, or runtime behavior violate ecosystem conventions | Share semantic specs; bindings follow platform best practices |
| Requiring matrix job names in rulesets | Platform changes break merging | Require stable aggregate gates only |
| Sharing SHA concurrency between push and PR | Push teardown can starve PRs | Separate lanes by event + PR/ref |
| Treating SHA as authorization for historical check runs | Missing contexts or validation cycles | Rerun reusable CI on the tag snapshot |
| Checking only `failure` in aggregates | Skipped/canceled jobs may pass | Require every result to be exactly `success` |
| Querying main/PR check runs during release | Releases depend on historical timing and mutable state | Invoke reusable CI again on the tag snapshot |
| Using the production environment for dry runs | PRs may access secrets or wait for reviewers | Read-only dry runs with disposable signing |
| Rebuilding in publishing jobs | Published bytes lack consumer verification | Download and publish staged artifacts unchanged |
| Running only workspace consumers | Package/link/loader issues go undetected | Clean temporary projects consume staged artifacts |
| Checking out PRs in privileged `workflow_run` jobs | Untrusted code obtains write tokens | The privileged side parses only strictly validated data |
| Treating `v*.*.*` as a SemVer regex | Invalid tags may trigger workflows | Validate with an exact parser plus `vVERSION` |
| Allowing tags to move or be reused | Provenance and registries lose traceability | Prohibit update/delete through rulesets; new bytes require new versions |
| Dispatching from main, then checking out a tag in a step | Tag-only environments still see the main ref | Rerun the tag run or dispatch with a protected tag ref |
| Activating rulesets before remote rehearsal | New repositories can become permanently blocked | Verify in evaluate mode before activating |
| Sole reviewer + prevent self-review | Releases can never be approved | Add an independent reviewer/team or temporarily disable prevention |

## 15. Definition of Done

Repository setup is complete only when all of the following hold:

- Local adapters, PRs, main, merge queues, and tag releases use the same reproducible command contracts.
- Every declared binding/target has correctness, shared-conformance, and real-consumer evidence.
- `Required gates` and `CodeQL gate` are the only required contexts in the active ruleset.
- Nonblocking observations cannot affect merging, and fork PRs cannot cross trust boundaries.
- Release dry runs read no credentials while fully reproducing the stage/sign/audit/consumer graph.
- Production releases verify immutable tag snapshots and publish only verified bytes.
- Release environments, tag rulesets, OIDC/secrets, signing, and recovery have all been rehearsed.
- GitHub, every registry, checksums, signatures, and attestations trace to the same tag commit.
- Bootstrap is repeatable and never silently deletes unknown policies; CI audits and live API
  verification detect policy drift.

## 16. Mapping This Repository to the Generic Template

Migrate responsibilities rather than copying current project names or job counts:

| Generic responsibility | Current implementation | Migration rule |
| --- | --- | --- |
| Root adapter | `pnpm` scripts in `package.json` plus `scripts/*` | Consolidate behind the target repository's `scripts/repo`, retaining native builders |
| Hygiene | `hygiene` in `ci.yml` | Replace formatters/linters; retain frozen installation and policy audits |
| Package audit | `package-audit` | Inspect real packed/archive/publication contents for target ecosystems |
| Core matrix | Linux/macOS/Windows C, shared/static, GCC/Clang | Replace with the target core's host/compiler/linkage support matrix |
| Binding matrix | Swift, Kotlin/KMP/Android, ES/WASM | Retain only declared bindings and deployment targets |
| Runtime safety | ASan, UBSan, TSan, Android emulator, browser | Select by target-language risk; blocking checks must enter the aggregate gate |
| Stable PR gate | `Required gates` | Keep the name; adapt internal `needs` to the target repository |
| Push summary | `Development branch gates` | Keep its name separate from the required context |
| Security gate | Four-language CodeQL + `CodeQL gate` | Adapt the language matrix; retain the aggregate name |
| PR observability | Metrics producer + `workflow_run` commenter | Optional; preserve isolation between untrusted code and write permissions if retained |
| Dry run | C, Swift source, npm, and Maven staged artifacts | Replace the artifact graph; continue prohibiting secrets/environments |
| Production release | Tag CI → stage → sign/audit/consumer → publish | Registries may vary; retain tag-snapshot verification and publication of verified bytes |
| GitHub policy | Checked-in environment/ruleset JSON | Bootstrap must generate actor IDs, reviewers, repository, and tag patterns |

This repository's specific registry order is: aggregate and sign Maven bundles, run staged consumers,
upload to Central and await validation, publish through npm OIDC, publish the validated Central
deployment, and finally create a GitHub Release with checksums and attestations. Target repositories
may use different registries, but must define equally explicit ordering and partial-failure recovery
for their irreversible operations.
