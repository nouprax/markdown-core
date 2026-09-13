# Phase 21: final repository closure

Status: closed. The final Git snapshot, dependency-free physical checkout,
unified environment bootstrap, repository-wide local verification, and Phase
19/20 remote evidence are complete.

## Boundary

Phase 21 handles final closure across phases: review the final Git snapshot,
remove local generated/cache/IDE state, verify from a physical checkout without
dependencies, complete development-environment onboarding, and consolidate
closure evidence for Phases 0–20. It does not redesign Phase 17's
package/test/public-surface contract or block implementation and remote
validation of the Phase 19 quality workflow or Phase 20 release workflow.

## Tasks

- [x] Review all tracked deletions, modified files, and untracked package sources to ensure the final snapshot contains only intentional changes.
- [x] Remove build/cache/dependency/IDE output before installing any dependency and run `scripts/audit-repository.sh --physical`.
- [x] Run `pnpm audit:repository:clean` after recording the final snapshot.
- [x] Use `scripts/init-environment.sh` to install or verify the pinned dependencies documented for a clean host.
- [x] Rerun root `verify`, platform consumers, package/public-surface/security checks, and the release dry run; consolidate remote ruleset, CI, and registry evidence in the final report.

## Acceptance

- [x] Phase 19 quality gates and Phase 20 release support are complete.
- [x] The final Git snapshot has been reviewed, and the physical checkout contains no generated, cache, dependency, package-manager, or IDE residue before dependency installation.
- [x] `audit:repository`, `audit:repository:clean`, and `scripts/audit-repository.sh --physical` retain their distinct boundaries; routine `verify` does not weaken the final mode.
- [x] The unified environment entry point reproduces the quality/release toolchain from a clean host.
- [x] Repository-wide verify, consumer, package, security, and release dry-run checks pass after installing pinned dependencies.
- [x] The README and closure report serve as the single entry point for new contributors and release maintainers.
- [x] Local ignored output is not an implementation blocker for Phase 19 or Phase 20.

## Final snapshot and bootstrap evidence

On 2026-07-15, `git clean -fdX` ran on macOS arm64, removing `.build/`,
`.gradle/`, `.kotlin/`, `.pnpm-store/`, `.swiftpm/`, `.tools/`, `node_modules/`,
root/package build and dist output, and local IDE state. Before installing
dependencies, `scripts/audit-repository.sh --physical` passed. Final review found
no tracked deletions or unrecorded package sources. The repository permits only
the tracked shared `.idea/runConfigurations/All_Kotlin_tests.xml`;
untracked/ignored `.idea` content remains rejected by the physical audit.

The README now links `docs/development-environment.md`.
`scripts/init-environment.sh` provides read-only `--check`, idempotent
`--install`, and component checks selected per job. A complete install and an
immediate second install both passed; the second reused pinned
Android/Emscripten/dependencies and repository-local tools. Bootstrap also
exposed old macOS ARM daemon coordinates pointing to an `OpenJDK21U-jdk`
archive without JDK tools. Regenerating all platform coordinates with Gradle
`updateDaemonJvm --jvm-version=26` produced macOS ARM coordinates verified to
point to a complete `OpenJDK26U-jdk` archive. Quality, release dry-run, and
production release workflows reuse the same `--check` entry point after their
official setup actions. That entry point reads no release secrets, does not
install Xcode, and requires neither global Gradle nor global Maven.

## Final local verification

After pinned dependencies were installed and the Git snapshot was clean, these
entry points passed:

- `pnpm audit:repository:clean` and complete
  `scripts/init-environment.sh --check`;
- root `pnpm verify`, covering formatters/linters, the Gradle model, version
  contract, repository, and CI/test/public-surface/package audits;
- `pnpm check:kotlin-consumers`, covering KMP/JVM Gradle, Android AAR, and the
  repository-owned Maven Wrapper consumer. This phase fixed a macOS Bash 3
  portability issue involving expansion of an empty array for the default
  Maven repository path;
- `pnpm release:dry-run`, covering the C install archive, Swift source/product
  consumer, npm tarball/types/runtime consumer, Maven/KMP staging, disposable
  PGP signing, checksums, staged consumers, and Central bundle audit, without
  reading publication credentials.

The Swift formatter continued to emit the project's existing public-documentation
diagnostics, but exited successfully; SwiftLint reported 0 violations. No lint
failure was added or hidden. Final TODO/FIXME review found only historical
explanations already classified in Phase 17, random encoding in the PGP keyring,
and the standard WASI `wasi_snapshot_preview1` import name. There were no
unassigned implementation items, preview toolchains, or placeholder credentials.

## Inherited remote and registry evidence

- Phase 19 required CI
  [run 29305643974](https://github.com/nouprax/markdown-core/actions/runs/29305643974)
  and CodeQL
  [run 29305644011](https://github.com/nouprax/markdown-core/actions/runs/29305644011)
  establish the complete hosted-runner matrix. The active `main quality gates`
  ruleset requires only the stable aggregate checks `Required gates` and
  `CodeQL gate`.
- Phase 20 protected-tag release
  [run 29444753606](https://github.com/nouprax/markdown-core/actions/runs/29444753606)
  and recovery release
  [run 29447029321](https://github.com/nouprax/markdown-core/actions/runs/29447029321)
  completed artifact staging, signing, consumer, registry, and provenance
  verification for the same `1.0.2` release lineage. Recovery reused verified
  artifacts without rerunning the build/test matrix.
- Final public coordinates are
  [`@nouprax/es-markdown-core@1.0.2`](https://www.npmjs.com/package/@nouprax/es-markdown-core/v/1.0.2),
  [`com.nouprax:kotlin-markdown-core:1.0.2`](https://repo1.maven.org/maven2/com/nouprax/kotlin-markdown-core/1.0.2/),
  and [GitHub Release v1.0.2](https://github.com/nouprax/markdown-core/releases/tag/v1.0.2).
  The release environment again accepts only `v*.*.*` tags; the failed Central
  deployment and temporary main policy were deleted.

The final Phase 21 PR validates only this phase's onboarding/JDK/portability
changes. It does not reestablish Phase 19/20 evidence for a new commit SHA or
republish `1.0.2`.
