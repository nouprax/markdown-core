# Phase 19: quality gates and PR observability

Status: closed. Repository contracts for blocking/non-blocking workflows, a
real Maven Wrapper consumer, and a dependency-verification keyring are in place.
The complete PR matrix, CodeQL, and metrics have remote execution evidence. The
default-branch ruleset is active and requires only `Required gates` and
`CodeQL gate`. Strict latest-base policy, blocking on missing gates, one updated
metrics comment, and the privileged commenter's security boundary are verified.

## Boundary

Phase 19 owns quality control: required checks that decide whether a PR can
merge, and performance/size information that does not affect that decision.
Versions, registries, signing, OIDC, release environments, publication dry runs,
and production publishing workflows belong to Phase 20.
The final Git snapshot, physical workspace cleanup, environment onboarding, and
complete clean-checkout verification belong to Phase 21 and do not block
activating quality gates or building the release workflow.

## Tasks

- [x] Execute the complete platform matrix in required CI, with passing Kotlin Linux x64 and repository-managed Android emulator correctness/conformance evidence. Missing hosts, simulators, browsers, or emulators must fail rather than silently skip.
- [x] Obtain passing required-CI public-surface/package-content/pkg-config/CMake consumer audits and CodeQL evidence. Earlier phases may wire workflows; this phase accepts remote execution results.
- [x] Add a repository-owned Maven Wrapper and minimal JVM Maven consumer that runs `verify` from the same isolated local Maven repository and actually calls `Document.parse`. Required CI must run this real Maven smoke without requiring globally installed `mvn`.
- [x] Give blocking CI and CodeQL stable, unique aggregate checks, `Required gates` and `CodeQL gate`. An aggregate must fail if any dependency fails, is cancelled, or is skipped. Rulesets must not depend directly on changing matrix job names.
- [x] Make all blocking workflows listen to both `pull_request` and `merge_group`, so the same required-check contract can support GitHub merge queue safely.
- [x] Commit an importable default-branch ruleset recipe requiring only `Required gates` and `CodeQL gate`. Benchmarks, binary size, coverage trends, and other informational pipelines must not become required status checks.
- [x] Import and activate the ruleset in GitHub, verify failing gates block PR merges and latest-base policy applies, and record minimal bypass ownership. Committed JSON alone does not prove external settings are active.
- [x] Add a non-blocking PR metrics workflow for C, Swift, Kotlin/JVM, and ES/WASM benchmarks and C shared-library, Kotlin/JVM JAR, and ES/WASM binary sizes. Missing metrics or regressions remain advisory and cannot change required-gate results.
- [x] Use fork-safe two-stage PR comments: a read-only `pull_request` workflow executes untrusted code and uploads a numerical artifact; a write-enabled `workflow_run` commenter neither checks out nor executes artifact/PR code, validates allowlisted numbers, and creates or updates one PR comment.
- [x] Add a CI policy audit for stable gate names, ruleset contexts, `merge_group`, non-blocking metrics, commenter permission separation, and exclusion of scheduled benchmarks from PR gates.

## Blocking gates

- `.github/workflows/ci.yml` exposes stable `Required gates` checks on
  `pull_request` and `merge_group`. The aggregate depends on hygiene, package
  audit, Swift deployment/tests, Kotlin platforms/consumers/managed emulator,
  ES Node/browser, C compiler/OS matrix, ASan, UBSan, and TSan. Any dependency
  result other than `success` fails the aggregate.
- Development-branch `push` still runs the complete matrix, but its aggregate
  is named `Development branch gates`. Push, pull requests, and merge queue use
  separate concurrency lanes. A newer run cancels an older one for the same
  PR/ref, but events do not cancel one another, so runner teardown cannot block
  the ruleset's required `Required gates` context.
- `.github/workflows/codeql.yml` exposes stable `CodeQL gate`, requiring success
  across all product languages.
- Both blocking workflows listen to `pull_request` and `merge_group`. The
  ruleset references only the two stable aggregates, not evolving matrix leaves.
- `.github/rulesets/main.json` is an importable default-branch recipe. Committing
  it does not activate it remotely. Phase 19 must import it in repository
  Settings → Rules → Rulesets and verify actual merge blocking.

Kotlin publications continue to use official Gradle/KMP generation and publishing
flows. Because the product promises consumption by real Maven projects,
required CI must run a minimal Maven consumer's `verify` and call
`Document.parse` through a repository-owned Wrapper with a pinned Maven version
and distribution checksum. Neither developers nor runners need global `mvn`.
Kotlin's official guidance uses Gradle publish tasks; Apache Maven recommends
pinning and launching Maven through Wrapper:
[KMP Maven Central publication](https://kotlinlang.org/docs/multiplatform/multiplatform-publish-libraries-to-maven.html),
[Apache Maven Wrapper](https://maven.apache.org/tools/mavenwrapper.html).

The repository uses Maven Wrapper 3.3.4's official `only-script` launcher.
`.mvn/wrapper/maven-wrapper.properties` pins the Maven 3.9.16 distribution URL
and SHA-256. `scripts/check-kotlin-consumers.sh` first publishes KMP/JVM/Android
artifacts to `build/kotlin-consumer-repository`, then uses that path as
`maven.repo.local` for the `verify` lifecycle of
`packages/kotlin-markdown-core/consumers/jvm-maven/pom.xml`. The consumer directly
depends on `com.nouprax:kotlin-markdown-core-jvm:1.0.0` and calls
`Document.Companion.parse` from Java, verifying Maven's effective model, POM
resolution, compile/package/verify lifecycle, and current desktop JNI payload.
Wrapper distributions are cached in ignored `build/maven-user-home`, without
global `mvn` or write permission to a user's Maven directory.

Remote activation order is fixed: merge workflows into the default branch;
run a PR so both stable check names exist in the repository; import
`.github/rulesets/main.json` and verify default-branch targeting, active
enforcement, and only the two required gates; then use a deliberately failing
test PR to verify blocking. GitHub documents JSON ruleset imports and required
status checks that must pass before protected refs can be updated:
[importing rulesets](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/managing-rulesets-for-a-repository#importing-a-ruleset),
[required status checks](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/available-rules-for-rulesets#require-status-checks-to-pass-before-merging).

## Non-blocking PR metrics

`.github/workflows/pr-metrics.yml` runs C, Swift, Kotlin/JVM, and ES/WASM
benchmarks in a read-only `pull_request` context, collects standardized
median/memory values, and reports C shared-library, Kotlin/JVM JAR, and ES/WASM
byte sizes. Every workload, collection step, and artifact upload is
informational; none is referenced by `Required gates`, `CodeQL gate`, or the
ruleset.

GitHub's issue-comment API supports ordinary PR timeline comments. To support
fork PRs without exposing a write token to untrusted code,
`.github/workflows/pr-metrics-comment.yml` reads artifacts only after
`workflow_run`. It does not checkout, fetch, or execute PR code or artifacts.
Before download, it uses the Actions API to bound artifact name/count/size;
after download, it bounds JSON file size again and accepts only nonnegative
integers from a fixed platform/runtime/workload/artifact allowlist. A stable
marker lets later commits update the same comment rather than create spam.

Ordinary PR timeline comments use the GitHub issue-comment API, where PRs are
addressed by issue number. The security boundary follows GitHub's explicit
warnings for `pull_request_target`/`workflow_run`: privileged workflows must
not checkout or execute fork/PR code, and downloaded artifacts are untrusted
data. See
[secure `pull_request_target`](https://docs.github.com/en/actions/reference/security/securely-using-pull_request_target)
and the [issue comments API](https://docs.github.com/en/rest/issues/comments).

The report is a single hosted-runner snapshot for review context, not a stable
performance baseline, and has no threshold. Future base/head deltas, historical
trends, or regression budgets remain non-blocking by default. Making them gates
requires a separate revision of the Phase 19 contract.

## Machine audit

`pnpm audit:ci` checks that:

- stable gate names exactly match ruleset required contexts;
- blocking workflows include `merge_group`;
- PR metrics workflows have no write permission;
- privileged commenters do not checkout/fetch PR code;
- metrics/benchmark/binary-size jobs do not enter required gates;
- scheduled benchmark workflows do not listen to `pull_request`.

## Acceptance

- [x] Obtain passing required-CI results for the complete matrix, package audits, and CodeQL.
- [x] Commit and run a real Maven Wrapper consumer proving the effective model, resolution, lifecycle, and JVM native payload.
- [x] Import and activate the ruleset; confirm failed or missing gates block merges.
- [x] Confirm metrics artifacts update one comment, and audit that fork-origin runs use the same read-only producer, allowlisted artifact, and privileged commenter that does not execute PR code.
- [x] Merge queue is not currently enabled. Policy audits verify both blocking workflows' `merge_group` wiring and fail-closed aggregation. An actual queue smoke test belongs to future queue activation, not current phase closure.

## Local evidence

Completed on 2026-07-13 with macOS arm64 and JDK 26.0.1:

- `./mvnw --version` downloaded and verified the pinned distribution, reporting
  Apache Maven 3.9.16;
- `scripts/check-kotlin-consumers.sh` passed all four consumers: KMP Gradle,
  JVM Gradle, Android AAR, and real JVM Maven. Maven `exec:java` successfully
  called `Document.parse` during `verify`;
- the official Gradle dependency-verification flow exported and committed the
  70-key ASCII-armored `gradle/verification-keyring.keys`, removed every
  `ignored-key`, and set `<key-servers enabled="false"/>`. Kotlin/JVM compilation,
  Unified Test Platform, and Android device-test classpaths passed with a fresh
  `GRADLE_USER_HOME`;
- the Kotlin gate passed JVM/Android host correctness, conformance, and
  `ktlintCheck` with `--offline --no-build-cache --rerun-tasks`;
- C complexity tests used 4 KiB → 16 MiB endpoint scaling, a 4096-fold range,
  accumulating at least 25 ms for short samples. The complete suite, ASan, and
  UBSan passed;
- `pnpm audit:ci`, `pnpm audit:repository`, shell syntax, and
  `git diff --check` passed.

The exported Gradle keyring and local-only keyserver configuration follow the
committed-keyring approach in
[Gradle dependency verification](https://docs.gradle.org/current/userguide/dependency_verification.html).
Local evidence does not replace remote acceptance below.

## Remote evidence

Completed on 2026-07-14 at commit
[`a43cbae`](https://github.com/nouprax/markdown-core/commit/a43cbae0e1d88b4325e8c364ca232269a1058f2d):

- [Required CI run 29305643974](https://github.com/nouprax/markdown-core/actions/runs/29305643974)
  passed 20/20 jobs, with `Required gates` reporting `success`. Coverage included
  Linux/macOS/Windows C, ASan/UBSan/TSan, Swift and deployment targets, Kotlin
  Linux x64/macOS arm64/Android host, repository-managed Android emulator
  correctness/conformance, Kotlin publication consumers, ES Node/browser,
  package audit, and hygiene;
- [CodeQL run 29305644011](https://github.com/nouprax/markdown-core/actions/runs/29305644011)
  passed 5/5 jobs, including C/C++, Java/Kotlin, JavaScript/TypeScript, and Swift
  analysis, with `CodeQL gate` reporting `success`. Kotlin disables build cache
  and forces recompilation. Swift retains `swift build --target MarkdownCore`;
  CodeQL initialization took 23 seconds and traced target compilation took
  12 minutes 01 second;
- macOS and Windows hosted runners passed the 4096-fold endpoint complexity
  gate. A discarded intermediate approach comparing adjacent scales was
  affected by scheduler pauses. The final gate judges asymptotic growth only
  from full-range endpoints, retaining intermediate scales as diagnostics;
- the push response confirmed an active default-branch ruleset prohibiting
  force pushes, requiring PR updates, and expecting `Required gates` and
  `CodeQL gate`. This maintenance used an authorized bypass.

Validation PR #1 passed push CI, PR CI, CodeQL, and PR metrics at final
implementation commit `2e3800b8`. The later review-documentation commit
`847c20c2` also passed `Required gates`. While its Swift CodeQL was still running,
required-check queries lacked `CodeQL gate`; the PR also remained draft, so its
`BLOCKED` state alone was not treated as proof. Authoritative evidence comes
from remote ruleset `main quality gates`: default-branch target, `active`
enforcement, only `Required gates` and `CodeQL gate` contexts, and
`strict_required_status_checks_policy=true`. The sole bypass actor is repository
role 5, retained for administrative recovery, with no additional team,
integration, or deploy-key bypass.

Later PR metrics runs updated the same stable-marker comment without adding a
second one; metrics checks are absent from the ruleset. Fork safety follows
from permissions and data flow: the untrusted producer has read permission only;
the privileged commenter neither checks out nor executes PR code or artifacts,
and parses only size-limited numerical JSON with allowlisted fields.
`pnpm audit:ci` enforces this boundary fail-closed. A real fork PR would repeat
the same path and is no longer an external state that must be manufactured
before closure.

## Closure decision

Phase 19 used PR #1 to verify the real pull-request pipeline. `Required gates`,
`CodeQL gate`, the complete platform matrix, and the non-blocking metrics
comment all ran. The ruleset API directly establishes enforcement, required
contexts, strict latest-base policy, and bypass scope.

Closure does not create additional deliberately failing PRs, fork PRs, or an
inactive merge queue. A failing PR would not add a product guarantee beyond the
active required-status-check rule, `BLOCKED` state with missing checks, and
fail-closed aggregate. Fork-commenter safety depends on token permissions, no
checkout/execution, artifact schema/size allowlists, and machine audits. A real
`merge_group` event cannot occur while merge queue is disabled. Future queue
activation should include an operational smoke test as configuration acceptance,
without reopening Phase 19.

The 2026-07-14 validation PR tightened the complexity gate to 4 KiB → 128 MiB,
a 32768-fold range, comparing endpoint time per byte directly. The first local
run passed 5 of 6 cases. `many_duplicate_attributes` showed 4.442-fold normalized
slowdown, establishing real superlinear growth in duplicate-attribute
normalization; relaxing the threshold could not close Phase 19. This wall-clock
gate runs only in ordinary builds. Sanitizer presets exclude the `complexity`
label to avoid mistaking instrumentation cost for product complexity.

The inherited cmark-gfm reference/footnote map and directive duplicate
normalization subsequently moved to a shared byte-key open-addressing hash
index. Ordinary inputs use expected-linear lookup. Excessive probe depth falls
back to the existing pointer sort, bounding deliberate hash-collision
degradation at O(n log n) rather than O(n²). Directives also remove HTML-style
`#id`/`.class` shortcuts and special handling of id/class; all ordinary keys use
last-wins semantics. Complexity tests add unique/duplicate references for a
total of 8 cases. The independent review of maps, directive duplicate semantics,
and consecutive-backslash batching is in
[`docs/specs/map-and-backslash-performance.md`](../specs/map-and-backslash-performance.md).

The first remote rerun exposed two constant-factor problems. Duplicate-heavy
directive input has only 64 unique keys, yet the index preallocated for every
source occurrence. The shared index now grows incrementally. Directives sample
at most 1024 keys: inputs with high uniqueness preallocate for the total count;
low-uniqueness inputs start from the sample's unique count. After an unclosed
directive falls back to CommonMark inline parsing, consecutive `\\` pairs
previously created tens of millions of Text nodes one pair at a time, only to
merge them in `parser_finish`. When no extension handles backslash, core now
decodes consecutive pairs in a batch into the same single Text node that would
ultimately result.

Local reruns passed after the fixes: about 60.4 MB of unique attributes showed
1.077× normalized slowdown, 32.7 MB of duplicate attributes 1.112×, 61.8 MB of
unique references 0.969×, and 41.3 MB of duplicate references 0.966×. The
128 MiB unclosed-backslash case fell from about 2.49 seconds to 0.189 seconds,
with 0.979× normalized slowdown.
Local Release correctness passed 59/59, C conformance 2/2, ASan/UBSan/TSan each
51/51, plus Swift, Kotlin JVM/Android host, ES Node correctness/conformance,
and repository verify. PR required checks still needed rerunning at that point.

The remote macOS runner further showed that 2.0× is not a reliable discriminator
for n log n growth: the same expected-linear unique-attributes hash path produced
3.318× and 2.753× in two runs because millions of parsed nodes crossed
allocator/cache levels absent from the 4 KiB sample. The wall-clock rejection
threshold was therefore calibrated to 4.0×, still below the measured old qsort
path's 4.442× and the first remote unclosed-backslash result of 9.850×.
Expected-linear behavior on the ordinary path is established by the shared hash
implementation, the 64-probe bound, collision-fallback tests, and code review;
the timing gate detects actual end-to-end degradation.

The same PR exposed two problems with push/PR SHA deduplication: cancelled push
runs still ran `always()` aggregates and left failed `Required gates` checks,
and two macOS runner teardowns stayed `in_progress` without an active step,
blocking PR runs in the same concurrency group. After the fix, only
`pull_request`/`merge_group` own `Required gates`; push aggregates are named
`Development branch gates`, with separate concurrency lanes for push/PR/merge
group. Final remote acceptance must confirm that required checks contain no
push context and PR runs no longer wait for push teardown.

The next validation showed that a group task with `maxConcurrentDevices=1` was
not truly serial: 4 KB and 16 KB setup still started together, one snapshot
creation timed out, and the other failed after waiting 600 seconds for a device
lock. The root Android emulator entry point therefore uses two separate Gradle
invocations, completing the 4 KB task before starting 16 KB. Host-derived
`testedAbi` is explicitly overridden through `configureEach` after all device
properties are configured.

Successful remote logs then showed that AGP 9.2.1's KMP setup-task
`CreationAction` did not copy public DSL `testedAbi` into its task input: both
devices still printed unspecified-ABI warnings. The repository retains the
public DSL declaration and explicitly sets the same host-derived input on the
pinned AGP `ManagedDeviceInstrumentationTestSetupTask`. Removing this
compatibility layer after an AGP upgrade requires remote logs proving the
upstream fix.
