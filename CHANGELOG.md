# Changelog

All notable release changes are recorded here. Markdown Core follows Semantic
Versioning for source packages and public API behavior; the C binary ABI is not
promised to remain compatible between releases.

## 3.0.0 - unreleased

The engine is reconstructed from the 1.0 baseline. The 2.0.0 line is withdrawn:
its major version was bought with a session and incremental parsing API that no
longer exists.

- Keep the bytes of a footnote call whose label crosses a line ending, and read
  a label spelled with a character reference out of the source rather than out
  of a released buffer.
- Resolve a repeated footnote label to the definition that opens first, and
  keep the definition that does not win where it was written, instead of
  destroying it and everything inside it.
- Give an unresolved footnote call a source position instead of line zero.
- Stop the formula and directive extensions from changing what CommonMark
  emphasis means when they are attached.
- Test the flanking scan's bound before reading it, and stop the directive
  extension registering a byte its inline matcher cannot consume.
- Report an ordered list of diagnostics beside the parsed document —
  `(severity, code, scope, message)` — covering the eight places where a
  construct the author wrote did not become one and neither the tree nor the
  concrete records can say so: a directive's rejected label or attribute list, a
  directive block that did not open or never closed, a table whose delimiter row
  does not match its header, a full or collapsed reference and a footnote call
  naming nothing the document defines, and a label the parser refused as too
  long. Recording them changes nothing the parse builds, and an allocation the
  list cannot make abandons the parse rather than reporting a short one.
- Report an allocation loss as `MARKDOWN_CORE_ERROR_ALLOCATION_FAILED` rather
  than as `MARKDOWN_CORE_ERROR_INTERNAL`, and stop the failure reporter needing
  an allocation of its own to say so.
- A node's `scope` is a pair of line/column BOUNDARIES saying which range of the
  source an element occupies — not a byte range, and no substring is taken with
  it. A block closed by a blank line therefore ends at column 0 of that line,
  which is what cmark-gfm reports and what an editor needs.
- `Document.concrete` is the normalized source and its line index: the text a
  scope's coordinates are counted against, which is not the string that was
  passed in wherever it held a NUL. The per-byte region set and its
  `RegionRole`, `Region`, `regionCount`, `region(...)` and `ownerOf(...)`
  surface are removed from C, Swift, Kotlin and ECMAScript.

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
