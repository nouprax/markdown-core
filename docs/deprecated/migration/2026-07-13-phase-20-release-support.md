# Phase 20: release support and release CI

Status: complete. The repository version contract, AGP 9.3 upgrade, staging for
all four products, Maven aggregation/signing audits, secret-free dry runs,
production release workflow, release manual, protected GitHub release
environment/tag ruleset, Maven secrets, external PGP configuration, npm registry
bootstrap/trusted publisher, and JetBrains Gradle/KMP clean import are in place.
Protected `v1.0.1` failed closed before artifact construction and published
nothing. Coordinated version `1.0.2` completed end-to-end publication through npm
OIDC, Maven Central, and GitHub Release.

## Boundary

Phase 20 owns building, validating, and coordinating publication of C, SwiftPM,
Maven/KMP, and npm artifacts from one release commit. It does not redefine
Phase 18's shared AST contract or Phase 19's required/non-blocking quality gates.
The final Git snapshot, physical checkout cleanup, development-environment
onboarding, and cross-phase verification belong to Phase 21.

## Tasks

- [x] Establish the new release lineage with npm `1.0.0` bootstrap; retain protected `v1.0.1`, which failed before publication, and prepare the first coordinated four-product release, `1.0.2`. Confirm the new repository contains no old tags and release documentation makes no C ABI compatibility promise.
- [x] Align C, SwiftPM, Maven, and npm versions and reject any artifact version drift.
- [x] Upgrade after AGP 9.3 stable is released and revalidate the Gradle/Kotlin/Android compatibility matrix. Cold-cache model, Android host/device tests, publication, and consumer checks must pass with `--warning-mode=fail`; do not launch on a preview toolchain.
- [x] Verify the SwiftPM source URL, repository-derived identity `markdown-core`, and product/module `MarkdownCore`.
- [x] Extract `swift package archive-source` outside the repository and verify the root canonical contract, SwiftPM build-tool plugin, provider conformance, and an independent consumer depending only on `MarkdownCore`. Source archives must retain test contracts, but consumer builds must neither run test plugins nor include derived fixtures.
- [x] Verify Maven Central `com.nouprax` namespace ownership through a `nouprax.com` DNS TXT record.
- [x] Verify Maven coordinate `com.nouprax:kotlin-markdown-core:<version>` and include KMP root, JVM, Android, and every Native target publication in one Central bundle. Remote dry-run evidence covers Linux/macOS staging and aggregation.
- [x] Verify consistency and completeness of POMs, Gradle Module Metadata, sources/javadoc artifacts, checksums, signatures, target-specific coordinates, and native payloads. Remote dry runs verify complete disposable PGP signing audits.
- [x] Run KMP Gradle, JVM Gradle Module Metadata, repository-owned Maven Wrapper JVM Maven, and Android AAR consumers from staged/local Maven repositories.
- [x] Perform a release clean-import smoke test in Android Studio Quail 2 2026.1.2 or equivalent IntelliJ IDEA 2026.1. Beyond successful sync, confirm visible KMP source sets and IDE execution of root Gradle `allKotlinTests`. Shared `All Kotlin tests` is a platform-independent shortcut for that task, not a sample app. Do not require duplicate checks in two IDEs using the same JetBrains Gradle/KMP importer.
- [x] Create an expiring Maven Central Portal user token and provide `MAVEN_CENTRAL_USERNAME`/`MAVEN_CENTRAL_PASSWORD` through the protected `release` GitHub environment.
- [x] Create a passphrase-protected PGP signing key, publish its public key, and provide `MAVEN_SIGNING_KEY`/`MAVEN_SIGNING_PASSWORD` through the protected environment. Complete key creation, publication to two keyservers, environment secrets, and offline private-key/revocation-certificate backups.
- [x] Verify public scoped-publish access in npm organization `nouprax`, perform the initial bootstrap publish, bind the exact release workflow/environment as trusted publisher, and revoke the bootstrap CLI session/token.
- [x] Give npm publish jobs minimal `id-token: write`/`contents: read` permissions. Prohibit traditional npm tokens in workflow policy and require registry 2FA with tokens disallowed. `1.0.2` is published by GitHub Actions OIDC with SLSA provenance.
- [x] Create a protected `release` environment with required reviewer, tag/branch restrictions, and Maven-only secrets. Complete the environment, tag-only policy, active ruleset, minimal GitHub Release permissions, and four Maven environment secrets.
- [x] Add release dry runs that read no release secrets and verify artifact contents, versions, metadata, checksums, signatures, and provenance inputs.
- [x] Create `docs/releasing.md` covering authentication, secret names, rotation, revocation, trusted publishing, offline signing-key backup, exposure response, changelog, release notes, and pre-release checks.
- [x] Run complete builds, correctness, shared-spec conformance, consumer, package/public-surface/security, and release dry-run checks from a clean checkout.
- [x] Verify that C install, Maven, npm, and Swift compiled public products exclude shared spec corpora, unexpected public headers/symbols, renderers, private extensions libraries, native handles, and runtime implementation files. SwiftPM source archives must retain repository-owned tests/specs, while external consumers build only product targets.

## 2026-07-14 implementation evidence

- `VERSION` is the single CMake/Gradle/npm version contract.
  `release:check-version` also checks SwiftPM identity/product, consumer
  examples, tag namespace, and `v<VERSION>`.
- AGP is upgraded to 9.3.0 and Gradle to 9.6.1. With a fresh
  `GRADLE_USER_HOME`, `projects`, Android host correctness/conformance,
  publication, and staged consumers pass with `--warning-mode=fail`. CI run
  [29387109813](https://github.com/nouprax/markdown-core/actions/runs/29387109813)
  also passes Android managed-emulator correctness/conformance. Android Studio
  Quail 1 2026.1.1 supports only AGP 9.2; upgrading to AGP-9.3-compatible Quail 2
  2026.1.2 enabled a real Gradle sync. Gradle distribution sources and dependency
  source artifacts requested by the Kotlin MPP importer enter strict dependency
  verification. IDE logs record `onSuccess(RESOLVE_PROJECT:2)` and
  `onImportFinished`; final sync completes in 5.559 seconds. A subsequent
  Quail 2 clean import without `.idea` or project cache verifies visible
  `commonMain`, `commonTest`, `jvmMain`, Android, and Native source sets and their
  Kotlin roots. Root Gradle `allKotlinTests` aggregates JVM, Android host, and
  current-host Native correctness/conformance plus complete API 36 4 KB/16 KB
  managed-device tests. Shared `.idea/runConfigurations/All_Kotlin_tests.xml`
  invokes only that task, without creating a sample app. Android Studio actually
  runs `allKotlinTests` through the shared entry: 42 host tests pass, each of two
  managed devices passes 14 tests, and the run ends with
  `BUILD SUCCESSFUL in 1m 39s`. This is a developer aggregate only; CI/release
  still execute named platform correctness/conformance gates separately.
- Independent scripts cover C install/tarballs, Swift source archives, npm
  tarballs, Linux/macOS Maven staging, JNI aggregation, Central bundles,
  disposable dry-run PGP signing, checksums, and staged consumers. Production
  signing runs only after host aggregation, avoiding sidecars for obsolete bytes.
- `.github/workflows/release-dry-run.yml` has no environment, secrets, or write
  permission. `.github/workflows/release.yml` accepts only exact release tags,
  confines Maven secrets to `release` environment jobs, and limits npm jobs to
  OIDC permissions.
- GitHub environment `release` (id `18166863298`) requires reviewer
  `DongyuZhao`; deployment policy `54675963` accepts only `v*.*.*` tags.
  Active `release tag protection` ruleset `18962304` restricts matching-tag
  create/update/delete bypass to `DongyuZhao`. There is no reviewer team yet,
  so the sole operator may temporarily self-review. Adding a second release
  owner must switch to a team reviewer and prohibit self-review. Actions tokens
  default to read permission. Four Maven environment secrets are configured
  without placeholder credentials. GitHub's default admin bypass remains
  enabled and should be disabled when an independent reviewer team is added.
- Local `pnpm verify`, CI/repository/public-surface/package-content audits,
  C/npm/Swift staging, macOS Maven audit, and KMP, JVM Gradle, JVM Maven Wrapper,
  and Android staged consumers pass.
- On 2026-07-15, after removing Gradle outputs, 3 managed devices, and temporary
  clean-import backups, the IDE/KMP revision passes root `pnpm verify` again.
  Gradle model, CI policy, test topology, repository, public surface, and package
  contents checks all pass. A clean Android runtime `assembleRelease` actually
  runs all four CMake builds for arm64-v8a, armeabi-v7a, x86, and x86_64,
  demonstrating that `idea.sync.active` does not affect real builds. Final
  `allKotlinTests --dry-run --warning-mode=fail` resolves completely without
  triggering a remote workflow.
- `1.0.2` fixes exit cleanup order in the JVM bundled-native loader, uses JVM
  platform library-name mapping, and adds a justified scoped lint suppression
  for JAR extraction that requires absolute paths. JVM correctness/conformance,
  root `pnpm verify`, and host release dry-run pass. The dry run generates and
  revalidates C, Swift source, npm, Maven/KMP staged artifacts, disposable PGP
  signatures, staged consumers, and final SHA-256/SHA-512 values without reading
  release secrets or triggering remote workflows.
- After the release candidate advances to `1.0.1`, host release dry-run passes
  again: C and Swift artifacts, npm tarball, macOS Maven publications,
  disposable PGP signing/checksum audits, and KMP, JVM Gradle, Android, and
  Maven consumers all use `1.0.1`. Public GitHub Release notes read exactly
  `docs/releases/1.0.1.md`; CI policy rejects autogenerated notes and internal
  phase/acceptance records.
- The first protected-tag validation fails closed before building, exposing
  coupling between historical check-run/SHA queries and PR/main execution
  timing. The production workflow now invokes the complete reusable CI
  build/test suite directly from the immutable tag, without querying old checks
  or depending on CodeQL. Ordinary CI matches branches/PRs only, and releases
  are driven solely by `v*.*.*` tags, so concurrent review/merge cannot change
  the selected release snapshot. The `v1.0.1` attempt built, uploaded, and
  published no artifacts; its tag remains immutable, and the next coordinated
  version is `1.0.2`.
- Draft PR [#2](https://github.com/nouprax/markdown-core/pull/2) release dry-run
  [29386638494](https://github.com/nouprax/markdown-core/actions/runs/29386638494)
  passes at commit `757060ec02f6e48d810ee4be9dc01a3d0333ffa6`: Linux/macOS C
  artifacts, Swift source archive/product consumer, npm tarball consumer,
  Linux/macOS Maven staging, cross-host aggregation, disposable PGP signing and
  audit, KMP/JVM Gradle/JVM Maven/Android staged consumers, independently
  verifiable Central bundle, and final fail-closed gate. The dry run reads
  neither release environments nor secrets.
- On 2026-07-15, npm CLI web authentication and security-key 2FA complete the
  initial public bootstrap publish of
  [`@nouprax/es-markdown-core@1.0.0`](https://www.npmjs.com/package/@nouprax/es-markdown-core).
  Tarball consumer, `exports.types`, content inventory, and
  `npm publish --dry-run` checks pass first. The trusted publisher is bound
  exactly to `nouprax/markdown-core`, `release.yml`, and the `release`
  environment, with only `npm publish` permission. Package publishing access
  requires 2FA and disallows tokens. `npm logout` then revokes the bootstrap CLI
  session, and `npm whoami` returns `ENEEDAUTH`. The first production OIDC
  provenance attestation is tracked independently under Acceptance. Public
  `npm view` after logout confirms version `1.0.0`, repository metadata, and
  registry shasum `969853cf63edce7975ec185d73784d6c62e11d06`.
- GitHub
  [CI](https://github.com/nouprax/markdown-core/actions/workflows/ci.yml?query=branch%3Amain),
  [CodeQL](https://github.com/nouprax/markdown-core/actions/workflows/codeql.yml?query=branch%3Amain),
  and [release dry-run](https://github.com/nouprax/markdown-core/actions/workflows/release-dry-run.yml?query=branch%3Amain)
  establish Phase 19 review gates, the four-product
  build/conformance/consumer/package/security matrix, artifact staging,
  disposable dry-run signing, checksums, and provenance inputs. Production
  release does not reuse these runs' SHAs; it reruns required build/test gates
  on the tag snapshot.
- Protected `v1.0.2`
  [release run 29444753606](https://github.com/nouprax/markdown-core/actions/runs/29444753606)
  passes complete quality gates, four-product artifact staging, Maven signing,
  staged consumers, and final Central bundle audit on the tag snapshot. The
  final bundle contains only `com/nouprax/**`, and all 10/10 Central components
  validate. Recovery
  [run 29447029321](https://github.com/nouprax/markdown-core/actions/runs/29447029321)
  reuses only that run's verified artifacts without rerunning the build/test
  matrix, publishes
  [`@nouprax/es-markdown-core@1.0.2`](https://www.npmjs.com/package/@nouprax/es-markdown-core/v/1.0.2)
  with SLSA provenance through the trusted publisher, and then creates
  [GitHub Release v1.0.2](https://github.com/nouprax/markdown-core/releases/tag/v1.0.2)
  with checksums and artifact attestations. Maven deployment
  `7b529f7f-156a-481a-9245-367ebb97fba1` reaches `PUBLISHED`, exposing
  [`com.nouprax:kotlin-markdown-core:1.0.2`](https://repo1.maven.org/maven2/com/nouprax/kotlin-markdown-core/1.0.2/).
  The contaminated failed deployment is deleted, the temporary recovery `main`
  environment policy is revoked, and `release` again accepts only `v*.*.*` tags.

## Acceptance

- [x] Phase 19 required gates pass and the ruleset is active. The release workflow accepts only protected tags/environments and generates coordinated versions for all four products from one commit.
- [x] Contents, metadata, checksums, signatures, and provenance inputs of staged C, SwiftPM, Maven/KMP, and npm artifacts are independently verifiable. Every declared consumer actually runs from staged artifacts.
- [x] npm uses OIDC trusted publishing; Maven uses a minimally scoped, expiring Portal token and PGP signing. There are no long-lived, undocumented, or unprotected publication credentials.
- [x] Release dry runs read no secrets, production permissions are minimized per job, and GitHub Release publishes only artifacts verified at the same commit.
- [x] Binary/install distributions exclude root shared specs, test corpora, private implementation targets, renderers, and files/symbols not approved by the Phase 16 public-surface allowlist. SwiftPM source distribution retains test-contract source as an exception and proves it stays outside consumer product graphs.
