# Testing architecture

[Documentation](../specs/README.md) · [Syntax conformance](syntax-conformance.md)

The repository separates platform correctness tests, public-contract
conformance, and optional performance measurement. Each platform's native
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
| Kotlin | Gradle/KMP named platform tasks; instrumentation class selection on Android | `scripts/gradle.sh :packages:kotlin-markdown-core:tasks --group verification`; native `--tests` or runner arguments |
| ECMAScript | Package-native Node/browser correctness runner and separate conformance runner | `node packages/es-markdown-core/scripts/run-tests.mjs --list`; `--target` and `--suite`; Node `--test-name-pattern` |

Root `pnpm` tasks map task families to execution platforms in one step. Do not
add suite-specific aliases, `:full` variants, a second case registry, or a general
family router. Diagnostic sharding uses the native filters.

C data-driven runners also offer `spec_runner --list/--example/--section`,
`pathological_runner --list/--case`, and `concurrency_runner --case`.
`scripts/audit-test-topology.sh` compares discovery with CTest registration.
The optional benchmark runner's workload list is separate from test discovery.

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

`pnpm benchmark:c-host` explicitly configures an isolated C measurement build.
The `benchmark` label/runner exists only with `MARKDOWN_CORE_BENCHMARKS=ON` and
is absent from default, sanitizer, required-CI, and release test artifacts.
Measurements cover representative documents and adversarial shapes, using
tracked samples or deterministic generation without runtime downloads.

The separate PR benchmark measures a versioned parser workload and library
size against the exact base SHA. The untrusted PR producer builds only the
head and uploads its result. A privileged default-branch workflow uses a
trusted exact-SHA baseline or builds that base itself with persisted credentials
disabled. It never checks out or executes PR-head code. Both JSON inputs are
validated for origin, SHA, schema, workload, and numeric bounds before a
comparison comment is written.

Timing and RSS on hosted runners are informational and do not set pass/fail
thresholds. Binary-size comparisons require matching toolchain inputs. A future
performance gate requires a controlled environment, statistical design,
versioned workloads, and an explicit false-positive budget. A timing concern
becomes a correctness gate only when expressed as a reproducible semantic,
operation-count, or resource invariant. Bindings do not maintain additional
short wall-clock/RSS benchmark loops.

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

Every CI, CodeQL, release dry-run, and PR benchmark run first uses the shared
`changes.yml` preflight. Repository integrity, documentation contracts, test
topology, and documented release coordinates are checked even for
documentation-only changes. Required workflows always start; their gates may
skip only after a successful preflight explicitly permits it. A failed preflight
or missing decision fails the gates rather than leaving
required contexts pending or treating an unexpected skip as success.

`scripts/ci-changes.mjs` identifies prose through a conservative path allowlist.
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

Each workflow records its actual inputs in a small `ci-inputs` artifact retained
for 30 days. PR bases come from the tested merge's parents, because historical
run API responses can contain updated PR metadata. Evidence is accepted only
from a successful run for the same repository, event, ref, and PR. Re-running a
workflow replaces its record; failed-job retries can retain the original record
for that fixed snapshot. Missing, expired, incomplete, or unreadable evidence
falls back to full validation. Manual, scheduled, and formal release runs always
execute fully. A skipped PR benchmark produces no comparison comment.

`pnpm audit:tests` checks contracts without compiling. After an existing C build,
`scripts/audit-test-topology.sh build/cmake` additionally checks dynamic discovery,
nonempty labels, and disjoint correctness/conformance selection. Audits must not
rebuild C or Swift just to inspect their topology.

CI policy audits enforce required execution, release/test artifact boundaries,
and benchmark trust boundaries. Source layout, router implementation text,
managed-device orchestration details, and Action major versions are not CI
contracts to enforce through static string matching. Element inventories compare
actual descriptor identities and attachment tables, detecting missing, duplicate,
and unreadable declarations without hardcoding a count. Contract projections,
source manifests, public surfaces, and package contents are checked separately.
