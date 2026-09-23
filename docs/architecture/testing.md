# Testing architecture

[Documentation](../specs/README.md) · [Syntax conformance](syntax-conformance.md)

The repository separates platform correctness tests, public-contract
conformance, and performance measurement. Each platform's native
runner owns its suite graph, discovery, filters, timeouts, and expectations.
Root package scripts route to those runners without defining another test graph.

## Run tests

Install the [pinned toolchain](../toolchains.md) and dependencies,
then select a platform available on the current host:

| Platform | Correctness | Conformance |
| --- | --- | --- |
| C host | `pnpm test:c-host` | `pnpm conformance:c-host` |
| Swift macOS | `pnpm test:swift-macos` | `pnpm conformance:swift-macos` |
| Swift iOS Simulator | `pnpm test:swift-ios-simulator` | `pnpm conformance:swift-ios-simulator` |
| Kotlin JVM | `pnpm test:kotlin-jvm` | `pnpm conformance:kotlin-jvm` |
| Kotlin Android host | `pnpm test:kotlin-android-host` | `pnpm conformance:kotlin-android-host` |
| Kotlin Android emulator | `pnpm test:kotlin-android-emulator` | `pnpm conformance:kotlin-android-emulator` |
| Kotlin macOS arm64 | `pnpm test:kotlin-macos-arm64` | `pnpm conformance:kotlin-macos-arm64` |
| Kotlin Linux x64 | `pnpm test:kotlin-linux-x64` | `pnpm conformance:kotlin-linux-x64` |
| ECMAScript Node | `pnpm test:es-node` | `pnpm conformance:es-node` |
| ECMAScript browser | `pnpm test:es-browser` | Shared ES contract checked by the Node conformance target |

Each target runs its complete declared family; it must not silently become a
build-only or empty task. Missing required tools and unsupported hosts fail
explicitly. There is no cross-host aggregate and no public stress task.
`pnpm verify` collects repository checks, not platform correctness runs.

## Native runner ownership

| Platform | Owner | Discovery and focused runs |
| --- | --- | --- |
| C | One CMake/CTest graph with presets and labels | `ctest --test-dir build/cmake -N`; `ctest --preset correctness -L spec`; `ctest --preset correctness -R pathological_backticks` |
| Swift | SwiftPM correctness and conformance test targets; xcodebuild on iOS | `swift test list`; `swift test --filter`; xcodebuild `-only-testing` |
| Kotlin | Gradle/KMP named platform tasks; instrumentation class selection on Android | `scripts/tooling/run-gradle.sh :packages:kotlin-markdown-core:tasks --group verification`; native `--tests` or runner arguments |
| ECMAScript | Package-native Node/browser correctness runner and separate conformance runner | `node packages/es-markdown-core/scripts/run-tests.mjs --list`; `--target` and `--suite`; Node `--test-name-pattern` |

Root `pnpm` tasks map task families to execution platforms in one step. Do not
add suite-specific aliases, `:full` variants, a second case registry, or a general
family router. Diagnostic sharding uses the native filters.

C data-driven runners also offer `spec_runner --list/--example/--section`,
`pathological_runner --list/--case`, and `concurrency_runner --case`.
`scripts/audit/check-test-topology.sh` compares discovery with CTest registration.
The stage benchmark's runners are not in the test graph at all.

## Correctness and conformance

Correctness verifies behavior, failure boundaries, ownership, Unicode,
robustness, consumers, packaging, and adversarial inputs. Shared concepts keep
consistent suite names across platforms: `api`, `ast`, `consumer`, `errors`,
`ownership`, `unicode`, `robustness`, `pathological`, and `packaging`, with
platform-specific categories where needed.

C labels are `api`, `facade`, `consumer`, `spec`, `elements`, `regression`,
`pathological`, `fuzz`, and `packaging`. Each registered test has one label.
The independent `conformance` label is excluded from correctness presets.
The [C test graph](../../packages/markdown-core/tests/CMakeLists.txt) owns exact
case names and timeouts; documentation does not duplicate that registry.

Swift's `MarkdownCoreConformanceTests` target is separate from
`MarkdownCoreTests`. Kotlin's named conformance tasks select `AstTest`
independently of correctness. ES has a separate `run-conformance.mjs` entry.
Conformance checks field shapes, nullability, scopes, binding mappings, and
reviewed canonical dumps. It is required even when correctness passes.

Each binding tests its public API and native ownership boundary. JVM/Android
JNI decoder tests stay in the applicable source sets; they do not become
Kotlin/Native payload tests. ES type and runtime consumers install the actual
`npm pack` tarball and resolve declarations through package exports. Browser
checks use real headless Chrome/Chromium over HTTP ESM/Wasm loading, rather than
substituting a Node run. The C++ installed consumer and Swift consumer package
exercise their actual public integration paths.

Every C and binding correctness suite parses and inspects a 10,000-level list
nest to protect stack-safe parser finalization, transport, and materialization.
Stress is an input shape, not a separate task family. Tests assert semantics,
lifecycle, and bounded resource behavior; timings do not substitute for those
assertions. See [syntax conformance](syntax-conformance.md) for fixture and
size-doubling rules.

## Shared fixture ownership

The [canonical manifest](../../specs/canonical-ast/manifest.json) is the only
cross-platform case list. Each native conformance runner parses its inputs
through the public API and dumps the resulting typed AST independently.
Bindings never call the C dumper, read another binding's output, or construct
production AST values from dump text. Walking visitors are checked separately.

Swift generates test resources in its plugin work directory. Kotlin generates
build-only data with a cacheable Gradle task. ES generates package-local build
output through its conformance lifecycle. Tests consume these artifacts without
depending on repository working directories, runtime network access, external
symlinks, or tracked platform fixture copies.

All other correctness fixtures, consumers, compile contracts, and packaging
checks belong to their owning package. There is no root `tests/` harness or
cross-language test bridge. The [syntax conformance guide](syntax-conformance.md)
distinguishes package goldens from external oracle inputs and policies.

## Platform and IDE execution

CMake presets provide the single C graph for builds, tests, and IDEs. `make test`
delegates to CTest. SwiftPM indexes production parser/facade sources, excluding
CLI, tests, fuzzers, and benchmarks from the production target. Gradle imports
actual Kotlin/KMP and Android runtime modules; it does not mirror the C package
as an IDE-only module or under an Android `cpp` tree.

Kotlin/Native IDE import can generate cinterop declarations from public headers
without building CMake. Actual product/test KLIB tasks build and embed their
static library. The developer-only `allKotlinTests` Gradle task runs applicable
host tasks and managed-device tests, but does not replace platform-specific
CI gates or duplicate native suite discovery. Personal IDE state is not required.

Android instrumentation uses Gradle Managed Devices for fixed Pixel 10 Pro XL
API 36 Google APIs images with 4 KB and 16 KB page sizes. Correctness and
conformance use mutually exclusive native selection and run the two devices
sequentially through separate Gradle invocations. Gradle owns provisioning,
startup, snapshots, and shutdown; existing developer AVDs are not selected.

Reusable managed-device caches remain after tests. Use
`pnpm clean:kotlin-android-emulator` explicitly to delegate cleanup to
`cleanManagedDevices`; tests do not automatically delete caches or SDK images.
Local and CI entries are the same and require a supported SDK and hardware
virtualization.

Kotlin Linux x64 acceptance runs on the supported Linux CI host. A macOS
container or translated local run is not replacement evidence. macOS arm64 and
Linux x64 native jobs together cover the published native runtimes.

## Sanitizers and deterministic failures

The C `asan`, `ubsan`, and `tsan` presets reuse the production test graph:

```sh
cmake --preset asan
cmake --build --preset asan --parallel
ctest --preset correctness-asan
```

Use the corresponding preset names for other sanitizers. Where TSan is
unsupported, default builds still run the native concurrency regressions.

## The node integrity check

`markdown_core_node_check` walks a finished root and reports every parent,
sibling and containment relation that disagrees with its counterpart. It is the
only structural self-check this tree has, and it is compiled in by
`MARKDOWN_CORE_DEBUG_NODES`, which the `debug` preset alone defines:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset correctness-debug
```

The engine runs it once per owned root when that root's finish walk completes
-- inline completion, text consolidation and every element finish step run
from inside that one walk, so a break one of them introduced is attributed to
the root's walk -- and
again after each global postprocess pass, so a break a pass introduced is
attributed to that pass. A configuration
that does not define the macro is indistinguishable from one where the check
passes, which is how it stayed dead for the whole of its life: the define was
set in `core/`, where `set()` cannot reach the sibling directory that builds the
archive the test runners link, and no preset configured `Debug` at all.
`pnpm audit:node-integrity-check` holds both halves against the generated
compiler flags and the CI graph rather than against the source text.
Tests must not silently skip. Swift time limits belong to Swift Testing traits;
other timeouts belong to their native runner declarations.

Golden comparisons are exact UTF-8 bytes with localizable diffs. Text artifacts
use LF and one final newline. Diagnostics exclude unstable pointers, timestamps,
locale effects, and wall-clock measurements. Temporary files stay in build
output, and runners clean up their processes. Allocation-failure tests verify
that failed parses publish no partial document.

Build-once/run-elsewhere CI artifacts come from clean staging/build trees, so
deleted targets cannot leak stale executables into an archive. Swift uses
`build/ci-swift-tests`, Kotlin cleans its test-artifact staging, and C rebuilds
its artifact-specific CTest tree.

## Benchmarks and external corpora

`pnpm benchmark` runs Markdown Core and the pinned cmark/cmark-gfm
on the grammar-certified corpus under Callgrind and reports what
each engine spends on the two parse paths: source bytes into block buffers,
and those buffers into an AST. cmark splits exactly those two paths across
`cmark_parser_feed` and `cmark_parser_finish`, so the boundary is a real one on
both sides. Parser allocation, dialect attachment, element discovery, and tree
release are outside both stages: they are fixed cost that no document-size
argument applies to. The contract is in the
[benchmark README](../../packages/markdown-core/benchmarks/README.md).

Instruction and data-reference counts do not depend on how fast the machine was
or what else was running on it, so a hosted runner is as good a place to measure
as a quiet laptop. That is why no wall-clock pipeline remains: every one this
repository has had measured a shared runner as much as the parser.

They are not independent of the machine, though, and the report says so rather
than implying otherwise. Every report heads with an identity table — resolved
compiler, C library and valgrind versions, the compiler's full code generation
target, both engines' real compile options, what the C library dispatched on,
and a digest of the corpus. Comparisons require matching toolchain, environment,
corpus and compile-option identities. Source inventories and binary hashes are
provenance: they can differ because the code under comparison changed. The base
and current reports each retain their own object counts, source paths and option
digests. Adding, removing or renaming a translation unit is allowed when the
distinct compile-option sets match; surviving source paths must also retain
their own options. A hosted runner and a developer's machine reproduce each
other only as far as the measurement identities match.

The counts are still not time: they do not price cache misses, branch misses,
or stalls. Reference ratios remain diagnostic evidence. The source stage also
has a required regression budget: CI rebuilds its event base with the current
harness and corpus in the same job, verifies identical compile options and
runtime libraries, and allows at most 2% more `source_to_buffer` instructions
for each document/scale. A cheaper AST stage cannot offset a source regression.
Both reports and raw dumps are published even when that budget fails.

The runners exist only with `MARKDOWN_CORE_BENCHMARKS=ON`; CTest owns
correctness, while the reusable `Benchmark` workflow supplies the source budget
and attribute/lexbor measurement to `Required gates`. CI invokes it only when
its shared preflight requires execution, so Benchmark does not repeat preflight
or overwrite CI's input evidence. Manual Benchmark runs always measure. A
required run must finish both measurements and artifact uploads successfully.

After a PR's CI run completes, `Benchmark Comment` publishes validated numeric
results to one ordinary PR comment and updates it on later runs. It creates no
review thread to resolve. The comment includes base/current source, AST and
complete-parse counts, the source budget, attribute results and a link to all
reports and raw profiles. Missing or invalid reports are explicitly unavailable;
failed measurements do not silently retain an older result as current. A
documentation-only follow-up that reuses validation publishes the original
measurement for the current commit, identifying both the measured commit and
its run. Publication does not depend on whether the original run's comment
arrived before the follow-up push. A wholly documentation-only PR has no
measurement to publish.

Measurement runs have read-only permissions, including on fork PRs. The
`workflow_run` publisher checks out only its default-branch commit, reads
bounded JSON members without extracting or executing PR artifacts, and renders
only validated counts and identifiers. Candidate PRs come from GitHub's commit
association API, restricted to the triggering run's PR identities when present.
A PR created after the run cannot receive its results, including for fork runs
whose PR list is empty. The existing `ci-inputs` snapshot then narrows these
candidates to the recorded PR number, merge ref, head and tested base; an
artifact cannot nominate an unrelated PR. Missing or invalid input evidence
prevents publication. This snapshot is necessary because historical run API
responses can contain a PR's updated base rather than the base actually tested.
The stage report's baseline must also equal that recorded base.

For reused validation, the publisher follows the original CI run and attempt
recorded by preflight. That run must still be successful, belong to the same
PR, head repository and branch, and contain a full-validation snapshot with
identical execution inputs and tested base. It never searches for an older
green measurement. Missing, expired or inconsistent source evidence produces
an explicitly unavailable result. The comment's ordering belongs to the current
run, so an old publisher cannot overwrite a newer reused result.

Immediately before writing, the publisher rechecks the PR identity, current
head and base, head repository, branch, open state and run attempt, plus the
original run's attempt and successful completion when reusing validation. It will
not overwrite a newer run's bot-owned comment. The publisher must first exist
on the default branch before GitHub can trigger it.

Artifacts are scoped to each measurement's run attempt. On failed-job retries,
the publisher uses GitHub's latest job records to retain earlier successful
measurements. A new failed measurement without a report cannot fall back to its
previous attempt's artifact.

The engine has no measurement mode: it keeps one
parse entry with no feed/finish lifecycle, and the stage split is read out of
the recorded call graph afterwards. The profiling flavour differs from Release by
debug information, by keeping the single-call-site stage boundary out of line,
and by `-fvisibility=hidden` — which cmark sets for its own build and Markdown
Core's static objects did not, worth 2.1% of the source stage and 5.9% of the AST
stage until both engines got it. The driver verifies both boundaries survived the
build rather than reporting a folded-away stage as a cheap one.

Each case is also measured at twice the size, because a stage whose cost stops
being linear in the input is a complexity finding rather than a tuning one. A
timing concern becomes a correctness gate only when expressed as a reproducible
semantic, operation-count, or resource invariant. Bindings do not maintain
wall-clock or RSS benchmark loops.

External test corpora follow the owning package's
[manifest, license, and hash policy](../../packages/markdown-core/tests/corpora/README.md).
Deterministic fuzz smoke belongs to correctness. Long fuzz campaigns are
explicit tasks using the native harness and corpus, not default tests.

## CI and repository audits

Pull requests and merge-group snapshots must produce the fail-closed
`Required gates`, `CodeQL gate`, and `Release Dry Run - Ready` contexts.
Release readiness includes coordinated versions and all C, Swift, npm, and
cross-host Maven artifact producers. When full validation is required, a
failed, cancelled, skipped, or missing producer blocks readiness; a manual
dry run has a different context.

Every CI, CodeQL, and release dry-run run first uses the
shared `changes.yml` preflight; CI's Benchmark call uses that same decision.
Repository integrity, documentation contracts, test
topology, and documented release coordinates are checked even for
documentation-only changes. Required workflows and their final gates always
run. The gates share `scripts/shared/ci-gate.mjs`: a successful preflight with an
explicit `false` accepts successful or skipped dependencies and reports success.
An explicit `true` requires every dependency to succeed. Failed or cancelled
dependencies, failed preflight, and missing or invalid decisions always fail;
an unexpected skip is never treated as successful full validation.

`scripts/shared/ci-changes.mjs` identifies prose through a conservative path allowlist.
Markdown fixtures, machine-readable specifications, source, build configuration,
lockfiles, and workflows remain execution inputs. The fingerprint covers Git
paths, modes, and blob identities; executable Markdown, symlinks, and submodules
also remain inputs. Renaming source into a documentation path changes the
fingerprint because the original source disappears.

A PR or merge-group snapshot containing only documentation relative to its
integration base skips the build, test, security scan, release artifact, and
benchmark jobs. A documentation follow-up on a PR containing code may instead
reuse validation from the immediately preceding push, provided the latest CI,
CodeQL, and Release Dry Run runs all completed successfully with identical
execution inputs and the same tested merge base. Main-branch pushes apply the
same rule to CI and CodeQL. Older successes cannot bypass a newer failure or
unfinished run. Code-changing merge groups always run fully.

Each preflight records its actual inputs and execution decision in a small
version-2 `ci-inputs` artifact retained for 30 days. Reused validation records
each required workflow's original full-validation run and attempt. Successive
documentation pushes carry these direct references forward, keeping provenance
bounded without walking a chain of skipped runs. A wholly documentation-only
snapshot has no validation source. Missing or invalid provenance, including
older schema versions, requires full validation before reuse.

PR bases come from the tested merge's parents, because historical
run API responses can contain updated PR metadata. Evidence is accepted only
from a successful run for the same repository, event, ref, and PR. Re-running a
workflow replaces its record; failed-job retries can retain the original record
for that fixed snapshot. Missing, expired, incomplete, or unreadable evidence
falls back to full validation. Manual, scheduled, and formal release runs always
execute fully. A skipped stage benchmark produces no report.

`pnpm audit:tests` checks contracts without compiling. After an existing C build,
`scripts/audit/check-test-topology.sh build/cmake` additionally checks dynamic discovery,
nonempty labels, and disjoint correctness/conformance selection. Audits must not
rebuild C or Swift just to inspect their topology.

CI policy audits enforce required execution, release/test artifact boundaries,
and the stage benchmark's measurement contract: its stage boundaries, its
pinned profile flags, the absence of profiler instrumentation or a benchmark
parse lifecycle in the product sources, and the absence of any retired
wall-clock pipeline. Source layout, router implementation text,
managed-device orchestration details, and Action major versions are not CI
contracts to enforce through static string matching. Element inventories compare
actual descriptor identities and attachment tables, detecting missing, duplicate,
and unreadable declarations without hardcoding a count. Contract projections,
source manifests, public surfaces, and package contents are checked separately.
