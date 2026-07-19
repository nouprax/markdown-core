# Changelog

All notable release changes are recorded here. Markdown Core follows Semantic
Versioning for source packages and public API behavior; the C binary ABI is not
promised to remain compatible between releases.

## 2.0.0 - 2026-07-19

- Add incremental parsing sessions on every platform: `MarkupSession` in
  Swift, Kotlin, and ECMAScript and `markdown_core_session_*` in C apply
  byte-range edits and commit, reparsing only the stale region at a
  per-commit cost proportional to that region — for typical documents,
  independent of total document size; non-local shapes degrade gracefully
  per the sessions-and-deltas cost model, never worse than a small multiple
  of one full parse per commit.
- Each commit produces an immutable snapshot that structurally shares
  unchanged nodes, plus a `Delta` of four disjoint stable-id sets — added,
  removed, changed, and ancestors whose revision bubbled because a
  descendant changed — with before/after revisions; `MarkupID` values
  resolve against the latest snapshot for as long as the node survives.
- Guarantee dump-equality with a from-scratch parse after any edit sequence,
  enforced by replay, seeded random-edit, and coverage-guided fuzzing suites
  over the shared conformance fixtures and the CommonMark corpus.
- Answer footnote queries (numbering, resolution, first-use order,
  back-references) directly on sessions with refresh cost bounded by the
  affected sites.
- Make commits transactional: a failed commit leaves the session valid at
  its previous revision and retryable, verified under allocation-failure
  injection and address, undefined-behavior, and thread sanitizers.
- Remove all process-global parser state so unlimited sessions run
  concurrently; document-mediated dump and walk entry points and
  session-aware node values coordinate the platform surfaces.
- Publish the language-neutral sessions-and-deltas contract binding all
  platform APIs, plus adversarial-input and pathological-document coverage
  for the incremental machinery.

## 1.0.3 - 2026-07-15

- Add a single environment setup and validation entry point for local
  development, CI, IDE import, and release preparation.
- Refresh supported build runners and toolchains while keeping workflow policy
  audits focused on security and quality outcomes rather than Action versions.
- Harden PR concurrency, release staging, publication recovery, package audits,
  and cross-platform consumer validation.
- Add a reusable repository setup template covering platform-native bindings,
  stable quality gates, tag releases, and lessons learned from deployment.

## 1.0.2 - 2026-07-15

- Fix the Kotlin/JVM native loader so clean application shutdowns remove both
  the extracted JNI library and its temporary directory.
- Use JVM platform library-name mapping and non-overwriting extraction while
  preserving zero-configuration native loading from the published JAR.
- Fix Kotlin Multiplatform project import in Android Studio and IntelliJ IDEA so
  source sets remain visible after Gradle sync.
- Add a Gradle-backed `All Kotlin tests` IDE entry that runs every Kotlin test
  supported by the current host, including Android managed-device coverage.
- Expand consumer-facing package documentation and release guidance.

## 1.0.0 - 2026-07-15

- Establish the standalone Markdown Core C parser and read-only canonical AST
  facade without renderer APIs.
- Add coordinated SwiftPM, Kotlin Multiplatform/Maven Central, and
  ECMAScript/WASM packages backed by the same parser and canonical AST contract.
- Add cross-platform correctness, conformance, consumer, security, package
  content, performance, and release-support validation.
