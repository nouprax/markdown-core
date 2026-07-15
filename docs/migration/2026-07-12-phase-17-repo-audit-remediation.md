# Phase 17: repository audit remediation

## Status and boundary

Status: complete. The single-library C delivery boundary, Phase 7 platform
routing correction, repository configuration, formatter/linter, consumer, CI
wiring, and audit automation gaps are remediated. Remote required-CI evidence
follows the Phase 18 shared canonical AST contract work and belongs to Phase 19;
release execution belongs to Phase 20, and final Git/physical-worktree/
clean-checkout closure belongs to Phase 21. None is a Phase 17 implementation
blocker.

> Phase 18 completed the handed-off shared-contract work at
> `specs/canonical-ast/`, including fail-closed topology/package audits. Remote
> required-CI evidence remains Phase 19 scope; this does not reopen Phase 17.

The post-Phase 16 repository audit was read-only with respect to source and
configuration. It confirmed the section 13.1 package layout and the removal of
the inherited root `src/`, `extensions/`, `test/`, `tests/`, `api_test/`,
`bench/`, `fuzz/`, `android/`, and `scripts/make_entities_inc.py` paths. It also
confirmed that the product decision is one public C library:

```text
libmarkdown-core
```

Parser core and all supported extensions are implementation components of
that library. `libmarkdown-core-extensions` is not a public artifact and must
not be installed, exported, advertised, or linked by consumers.

## Reproduced release blockers

1. A clean shared-only install generated `markdown-core.pc` with both
   `-lmarkdown-core-extensions` and `-lmarkdown-core`. Linking an independent
   consumer failed because the install correctly did not contain a separate
   extensions library. The fix is to make metadata follow the one-library
   contract, not to install the inherited extensions artifact.
2. The installed CMake exports did not provide a standard package config or a
   single reviewed public imported target with install include requirements.
   Internal targets and the CLI were represented in exports instead of a
   consumer-ready one-library package.
3. `scripts/format-cmake.sh --check` failed for
   `packages/markdown-core/extensions/CMakeLists.txt`.

## Audit findings to close

The authoritative, checkable remediation task list is Phase 17 in
`docs/specs/2026-07-11-repo-setup.md`; test-routing ownership was the explicit
exception moved back to Phase 7 and is now implemented there. Together they cover:

- one-library pkg-config/CMake install metadata and real installed consumers;
- package-audit regression coverage for shared-only and static delivery;
- required CI wiring for package and public-surface audits;
- the Phase 7 test-routing correction: root pnpm exposes three task families
  directly keyed by real execution platform, while suite discovery remains exclusively
  package-native; Phase 17 depends on that acceptance but does not own a
  duplicate routing task;
- CodeQL alignment with C/C++, Kotlin, ES/TypeScript, and Swift;
- the missing `COPYING-CMAKE-SCRIPTS` attribution;
- the repo-owned Gradle dependency-notation deprecation;
- JDK/Android SDK/project-path drift in `docs/toolchains.md`;
- stale `.gitattributes`, `.prettierignore`, ESLint, workspace, and build-path
  entries, including missing final newlines;
- inherited TODO/FIXME triage;
- the isolated Maven-repository consumer validation gap;
- review of all tracked deletions and untracked package sources;
- reusable automation for later clean-snapshot, physical-worktree, structure,
  secret, link, file-mode, package, public-surface, and license closure audits.

## Passing baseline captured by the audit

The following baseline passed before remediation and must remain green:

- C default, static-only, and shared-only builds and 59 correctness tests;
- 59/59 tests under ASan, UBSan, and TSan;
- C format/lint/public-surface/test-topology/package-content checks other than
  the recorded CMake formatting failure and installed-consumer blind spots;
- Swift tests, independent SwiftPM consumer, SwiftFormat, SwiftLint, and iOS
  18/26 plus macOS 15/26 deployment builds;
- Kotlin JVM, Android host, macOS Native, four-ABI AAR packaging, isolated
  publications, Gradle KMP/JVM/Android consumers, ktlint, and model smoke;
- ES Node, browser, API, AST, Unicode, error, ownership, robustness, conformance,
  packed npm consumer, Prettier, and ESLint checks; and
- canonical AST fixture and Kotlin naming contracts plus `git diff --check`.

Passing this baseline did not by itself close Phase 17. Each implementation
task required a regression that would have failed on the audited state,
especially installed pkg-config and CMake consumers.

## Remediation ledger

### P17-01: single installed C library and consumer metadata

- **Reproduction:** the inherited `markdown-core.pc` emitted
  `-lmarkdown-core-extensions -lmarkdown-core`; CMake installed two unrelated
  export sets containing the CLI and private engine targets.
- **Root cause:** internal core/extensions static targets were reused as
  public install targets, so build organization leaked into package metadata.
- **Fix:** internal engine and extensions archives remain build-only. The
  shared facade and a complete static archive are both installed as the single
  physical `libmarkdown-core`; the latter compiles the core and supported
  extension sources into one archive. The standard config package exports
  exactly `markdown-core::markdown-core` with its install include directory.
  `markdown-core.pc` now emits only `-lmarkdown-core` and describes the parser
  plus immutable AST facade.
- **Regression:** package-owned consumer source lives under
  `packages/markdown-core/tests/consumers/`. The package audit performs clean
  shared-only and static-only installs, rejects any installed
  `markdown-core-extensions` reference, and configures/builds/runs independent
  pkg-config and CMake consumers outside both prefixes.
- **Evidence:** on macOS, shared-only and static-only installs contained one
  header, one library identity, one `.pc`, and one config-package directory;
  both independent CMake consumers ran successfully. The default C graph then
  passed 59/59 correctness tests. The local host has no `pkg-config`; the
  required Ubuntu package-audit job installs it explicitly. Its actual green
  execution is Phase 19 CI evidence and does not keep this implementation open.

### P17-02: CI and CodeQL enforcement

- **Reproduction:** package/public-surface audit commands existed only in the
  root manifest; required CI did not call them. CodeQL still analyzed inherited
  Python/Ruby and relied on autobuild.
- **Fix:** CI now calls topology and public-surface audits directly and has a
  dedicated package-audit job with pkg-config, Emscripten, JDK 26, and Android
  37/NDK dependencies. CodeQL uses the current product language identifiers
  `c-cpp`, `java-kotlin`, `javascript-typescript`, and `swift`; compiled
  languages use explicit reproducible build commands and the interpreted ES
  job uses build mode `none`.
- **Evidence:** workflow YAML passes repository Prettier formatting. Local
  topology and public-surface audits pass. Required workflow execution is
  deferred to Phase 19 CI evidence.

### P17-03: attribution and migrated configuration

- **CMake attribution:** `CheckFileOffsetBits.cmake` and its probe were dead:
  the result was never configured into a header, definition, or target. Both
  inherited files and their call site were removed, eliminating the unshipped
  license reference rather than copying unused third-party code.
- **Toolchains:** documentation now matches JDK 26 and Android compile/target
  37, uses the current Kotlin project path, and removes `:android:dependencies`.
- **Workspace/config:** the empty `samples/*` workspace glob and stale root
  tests, Android, SwiftPM, XSL, and obsolete build-path entries were removed;
  canonical C package fixtures are named explicitly and touched text files end
  with a newline.
- **Inherited annotations:** the three remaining TODO/FIXME markers were
  triaged. `linebuf` is the partial-feed accumulator while `curline` is the
  normalized active line; the unexplained list-copy marker was removed; the
  mutable-engine ownership limitation is now documented as outside the public
  immutable facade.

### P17-04: repository Gradle dependency notation closed

- **Repository fix:** Android main now declares its project dependency through
  `DependencyHandler.project(String)` rather than passing a `Project` object.
- **Remaining reproduction:** Gradle 9.6.1 with `--warning-mode=fail` still
  fails during KMP Android host-test component creation. The captured stack is
  entirely inside AGP 9.2.1
  `VariantDependencies.createForKotlinMultiplatform`; no repository build-file
  frame performs the deprecated conversion.
- **Upstream status:** Android's AGP 9.3 release notes list the migration away
  from Project dependency notation as fixed from 9.3.0-alpha01, while the
  current supported release remains 9.2.1 and 9.3 is still preview:
  [AGP API versions](https://developer.android.com/reference/tools/gradle-api),
  [AGP 9.3 fixed issues](https://developer.android.com/build/releases/agp-9-3-0-release-notes).
- **Closure decision:** Phase 17 owns repository notation and precise warning
  attribution, both complete. It does not replace the supported toolchain with
  an RC to suppress an upstream warning. Phase 20 now requires AGP 9.3 stable
  and cache-cold `--warning-mode=fail` model, Android host/device, publication,
  and consumer evidence before the first release.

### P17-05: routing implementation closed

- **Reproduction:** the root manifest exposed only flat `test:<lang>` entries;
  Swift tests ran on macOS only, Kotlin `kotlinTest` selected only the current
  host native target and Android host, and ES treated browser as a suite rather
  than an execution target. `failure` and `errors` also coexisted as public
  suite names.
- **Review correction:** the first remediation exposed the missing platforms
  but over-corrected by freezing a language → platform → suite matrix plus
  `:full` aliases. That duplicates native-runner ownership and is not the final
  contract. The authoritative task now lives in Phase 7: `test`, `conformance`,
  and `benchmark` are independent task families keyed directly by platform;
  there is no intermediate language layer, suite filters remain native, and no public
  `stress` task exists. `errors` remains
  the canonical replacement for the duplicate `failure` name inside native
  suite taxonomies.
- **Repository implementation:** the generic router, root suite aliases,
  language aggregates, and `:full` entries were removed. CTest presets, SwiftPM
  test targets, Gradle/KMP tasks, Android instrumentation selection, and ES
  scripts are separately named native targets. Large/deep input correctness
  checks are named robustness, while C, Swift, Kotlin, and ES benchmark runners
  own separately timed large-document/deep-nesting workloads.
- **Execution targets:** C `host`; Swift `macos` and `ios-simulator`; Kotlin
  `jvm`, `android-host`, `android-emulator`, `macos-arm64`, and `linux-x64`; ES
  `node` and `browser`. Kotlin conformance moved from `jvmTest` to `commonTest`,
  and Android instrumentation tests opt into the same `test` source-set tree.
- **Required regression:** topology audit rejects suite-level pnpm tasks,
  `:full` aliases, public stress tasks, and root suite matrices; proves
  correctness/conformance discovery is disjoint; derives required platform
  leaves; verifies native discovery remains nonempty; and matches every
  platform to a real required-CI host, simulator, browser, or device
  destination.
- **Reusable evidence:** macOS Swift full passed 7 shared tests plus the consumer; the
  same 7 shared tests passed on an iPhone 17 Pro simulator. Kotlin JVM
  conformance, Android-host errors, and macOS ARM64 conformance passed. Moving
  conformance to Native exposed and fixed missing structural
  `equals`/`hashCode`/`toString` behavior in the immutable `ReadOnlyList`.
  Android device-test APK assembly, dependency locks/verification, Kotlin
  ktlint, ES Node full, ES browser full, and topology audit passed.
- **Revised local evidence:** C exposes disjoint 57-test correctness and 2-test
  conformance presets. Swift macOS/iOS Simulator select separate 5-test
  correctness and 2-test conformance targets. Kotlin JVM, Android host, and
  macOS ARM64 paired tasks pass with disjoint reports. A repo-declared Pixel 10
  Pro XL/64-bit Google APIs Gradle Managed Devices group covers API 36/4 KB and
  API 37/16 KB without an existing host AVD; each emulator passes 10 correctness
  tests plus 2 conformance tests. ES Node correctness, Node conformance,
  browser correctness, and large/deep benchmarks pass. The revised topology
  audit passes with no generic router or suite-level tasks.
- **Closure audit:** the two Phase 7 routing implementation tasks and Phase 7
  are complete. Linux x64 and Gradle Managed Device correctness/conformance
  execution belongs to Phase 19 required-CI acceptance; Linux x64 remains
  intentionally CI-only, while the local managed-device run validates the
  repo-owned environment contract.

### P17-06: isolated Maven-repository consumers closed

- **Correction:** publishing to a Maven repository and proving Maven
  effective-model/lifecycle compatibility are separate checks. Gradle remains
  the publication orchestrator; because the product promises a real Maven
  consumer, Phase 19 also runs that consumer with a repo-owned Maven Wrapper.
- **Implementation:** `check:kotlin-consumers` publishes with the repo Gradle
  Wrapper to an isolated `maven.repo.local`, then runs KMP, JVM Gradle, and
  Android AAR consumers.
- **Evidence:** all three consumers passed locally. The JVM consumer executes
  `Document.parse`, so resolution is followed by actual JNI native-payload load;
  the Android consumer assembles against the root coordinate. Remote execution
  and the additional real-Maven wrapper smoke remain Phase 19 quality-gate
  evidence, not a Phase 17 implementation blocker.

### P17-07: formatter, migrated configuration, and final-audit automation

- **Formatter regression:** the recorded
  `packages/markdown-core/extensions/CMakeLists.txt` cmake-format failure is
  fixed. C, CMake, Swift, Kotlin, and ES formatting now pass together.
- **Lint/config follow-up:** the Phase 19 metrics collector exposed missing
  Node globals in the root ESLint scope. Root scripts now receive an explicit
  `process` global, and obsolete build-phase, local Gradle-home, and retired
  fixture/golden exclusions were removed instead of widening ignores.
- **Automation:** `audit:repository` is part of root `verify`; required CI uses
  `audit:repository:clean`. They reject credential-shaped files/content, files
  over the reviewed size limit, broken/absolute symlinks, bad script modes,
  missing final newlines, coordinate/license drift, and required attribution
  loss. Clean mode additionally rejects any tracked/untracked source change;
  `--physical` also rejects build/cache/dependency/IDE directories and empty
  directories before dependency installation.
- **Transferred evidence:** Phase 21 runs `--physical` after all intended source
  has been reviewed and recorded, then installs dependencies and runs the full
  functional validation from that clean checkout. This is final repository
  closure, not unfinished Phase 17 implementation.

## Transferred final-closure work

- Phase 18 owns the root shared canonical AST corpus, manifest/coverage contract,
  and C/Swift/Kotlin/ES conformance adoption through public TreeDumpers.
- Phase 19 owns required-CI, ruleset, and PR-observability execution evidence.
- Phase 20 owns release toolchain, registry, signing, provenance, and staged
  consumer evidence.
- Phase 21 reviews the final Git snapshot, removes ignored outputs, runs
  physical/clean-checkout audits, completes environment onboarding, and records
  the cross-phase closure report.

## Exit rule

Phase 17 closes when every repository-owned remediation has an implemented
regression and a documented owner for later execution evidence. It is now
closed. Phase 18, Phase 19, and Phase 20 are not blocked by local
physical-worktree state; Phase 21 is the only phase that requires the final
clean Git snapshot and generated-output-free physical checkout.
