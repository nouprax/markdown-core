# Phase 18: shared canonical AST conformance spec

Status: implementation and acceptance on locally available platforms are
complete. Remote Linux x64 and passing required-CI evidence remain Phase 19
responsibilities and do not retroactively block closure of this shared contract.

## Boundary

Phase 18 adds root `specs/canonical-ast/` containing only product-level
Markdown/`.ast` contract data, a coverage manifest, and maintenance instructions,
with no runner. The C package retains sole ownership of parser correctness
corpora for CommonMark, extensions, regression, pathological, fuzz, and
robustness. The shared canonical AST spec does not enter ordinary
`test:<platform>` correctness discovery or introduce a `spec:*` task.

Existing `conformance:<platform>` entry points still call native runners
directly. Runners produce text through public `Document.parse`, immutable AST,
Visitor/Walker, and `TreeDumper` APIs, then compare the same `.ast` bytes exactly.
Shared data is a conformance oracle, not a production serialization/transport
format, and must not construct production ASTs.

## Tasks

- [x] Move Markdown/`.ast` pairs, README, and coverage manifest from `packages/markdown-core/tests/canonical-ast/` to root `specs/canonical-ast/`. Their sole owner is the cross-product public AST contract; C retains no private copy.
- [x] Freeze manifest schema and case discovery: explicitly list each case's Markdown input, expected dump, required parse options, and coverage tags. Paths, order, UTF-8, LF, and final newline must be deterministic; runners must not maintain another case list or normalization.
- [x] Cover all 28 `Markup` kinds, behavior-bearing fields, enum/boolean/null states, scope coordinates, escaping, child order, empty/populated children, and table/directive/footnote/formula structures. Coverage audit fails closed on missing or undeclared coverage.
- [x] Make C conformance read every shared case through public C parse/document-dump APIs; keep CommonMark/extension/regression correctness fixtures in C.
- [x] Make Swift conformance call public `Document.parse` and `Markup.dump()`/`TreeDumper.dump(_:)` for every shared case; remove package-local expected-tree literals. macOS and iOS Simulator must consume one physical corpus.
- [x] Connect Kotlin common conformance to every shared case and remove package-local expected-tree literals. JVM, Android host, repository-managed Android emulator, macOS ARM64, and Linux x64 native targets must consume one physical corpus.
- [x] Make TypeScript/ES Node conformance use public npm APIs and `TreeDumper` for every shared case; remove package-local expected-tree literals. Do not call C dump, read another binding's output, or add a JSON bridge.
- [x] Generate derived resources for simulator/emulator/native test bundles from the root spec source. Do not commit Swift/Kotlin/ES fixture copies, use out-of-boundary symlinks, or depend on a developer's current working directory. Clean builds and packaged test bundles must locate the same content.
- [x] Retain each binding's focused API unit tests for behavior shared dumps cannot express: Visitor exhaustiveness, Walker events, errors, ownership, and lifetime. Do not copy parser correctness corpora into binding packages.
- [x] Provide explicit, manually reviewed golden maintenance: rewrite may generate candidate diffs only through the public C canonical dump and must not accept output automatically in tests/CI. Schema or dump-grammar changes must update specifications, manifest, goldens, all four implementations, and acceptance in one change.
- [x] Update topology/public-surface/package audits to allow runner-free root `specs/` contract data. Reject package-local canonical corpora, unlisted cases, spec data in published packages, missing conformance targets, and success through empty discovery or skips.
- [x] Update Phase 5/7/8/11–17, canonical AST/dump, test architecture, and repository setup documentation. Withdraw the old C-only-goldens/package-local-binding-snapshot rule and preserve mutually exclusive correctness/conformance/benchmark entry points.

## Acceptance

- [x] Root `specs/canonical-ast/` is the sole canonical Markdown/`.ast` corpus. Its coverage manifest covers all 28 Markup kinds and frozen fields; no platform copy or second case list exists.
- [x] Existing native conformance targets for C, Swift, Kotlin, and ES enumerate the same nonempty manifest, use their public parse/AST/Visitor/Walker/TreeDumper paths, and pass byte-for-byte comparisons for every case.
- [x] Swift iOS Simulator and Kotlin Android emulator test bundles derive resources from the same source; clean CI needs no repository cwd, local fixtures, downloads, or manual copies.
- [x] Ordinary correctness targets do not discover shared spec cases, and benchmarks do not read them. The root manifest supplies neither a runner nor a new public task route.
- [x] Deliberately breaking any binding field mapping, Visitor dispatch, Walker hierarchy/order, scope/escaping, or TreeDumper grammar fails the corresponding platform conformance target.
- [x] Format, lint, canonical coverage, test topology, public surface, package contents, and root `verify` pass. Spec data does not enter C install, Swift public product, Maven, or npm binary releases; SwiftPM source archives retain test-contract source.

## Implementation

`specs/canonical-ast/manifest.json` is the sole discovery source. Schema v1 pins
six cases' inputs, expected outputs, 11 parse options, manifest order, UTF-8,
LF, final newline, and kind/state/order coverage tags.
`check-canonical-ast-fixtures.mjs` derives 28 kinds and 47 behavior-bearing
fields from the AST contract and fails closed on missing/unknown coverage,
unlisted files, incorrect field order, and empty discovery.

Consumption on all four platforms:

- CMake parses the manifest at configure time and passes each case and option
  mask to `facade_test`. Public C parse/document dump and CLI dump are compared
  byte for byte.
- The SwiftPM `GenerateCanonicalASTResources` build-tool plugin declares the
  root corpus as inputs, invokes a package-owned Swift executable tool, and
  generates one `canonical-ast-fixtures.json` resource in its work directory.
  macOS and iOS Simulator share a `Bundle.module` loader. The manifest no longer
  declares resources outside the target through `../../..`.
- Gradle's cacheable `GenerateCanonicalAstFixtures` task class parses the
  manifest directly and generates build-only `commonTest` Kotlin data. JVM,
  Android host/device, and Kotlin/Native compile the same generated source.
  Generation supports configuration cache, and test compilation/ktlint depend
  on it explicitly, without calling a Node generator across tools.
- The ES package uses npm/pnpm `preconformance` to generate
  `build/generated/conformance/canonical-ast-fixtures.json` from the root
  manifest. Node tests read only package build output and compare each case
  through public npm `Document.parse`/`TreeDumper`, without depending on `cwd`
  or relative paths across packages.

`generate-canonical-ast-candidates.sh` writes candidate output only through the
public C CLI to `build/canonical-ast-candidates/` and prints diffs. It does not
modify accepted goldens and fails closed on unsupported nondefault parse options.

## Defect closed during native verification

Initial Kotlin/macOS verification found an old `MKC1` bridge archive embedded in
the native executable while the common decoder required `MKC2`. The cinterop
task only declared `dependsOn` the native build, without declaring the archive
directory as an input, so incremental builds reused an old klib. cinterop now
tracks the archive explicitly; after rebuilding, macOS ARM64 conformance
passes. The decoder also reports precise magic-byte diagnostics instead of a
generic error for this failure class.

## Verification evidence

- Manifest/architecture: canonical coverage, test topology, public surface, and
  package contents audits pass. npm/Maven/C install/Swift compiled public
  products exclude spec data; SwiftPM source archives retain reproducible
  test-contract source.
- C host, Swift macOS, Swift iOS Simulator, Kotlin JVM, Android host, macOS
  ARM64, repository-managed Android API 36 4K/API 37 16K, and ES Node
  conformance pass.
- Each Android managed device runs four `AstTest` cases. Swift build logs show
  the build-tool plugin generating JSON before it is included in test bundles.
- Linux x64 task wiring and generated common-test input are frozen; passing
  remote Linux required-CI evidence belongs to Phase 19 under the phase boundary.
