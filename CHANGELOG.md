# Changelog

All notable release changes are recorded here. Markdown Core follows Semantic
Versioning for source packages and public API behavior; the C binary ABI is not
promised to remain compatible between releases.

## Unreleased

- Added (C, Swift, Kotlin, and ECMAScript): `append`, the one mutation. A
  document is the live head of a chain; `append(chunk)` adds bytes at the
  end — any byte split is legal, mid-word or mid-character — and returns
  the document all bytes so far describe. One rule: an append supersedes
  its receiver (which keeps free at any time and its revision, series and
  length, and answers for no tree), the revision advances strictly by one
  on the chain's own counter, and mutating a superseded handle is a
  deterministic error, so history is linear and derived state can be
  destroyed and rebuilt in place. A failed `append` ends the chain. The
  bindings decode O(changed) per append: an unchanged (id, revision)
  subtree reuses the previously decoded value outright, so a stream's
  per-tick decode cost follows the change, not the document. On the engine
  side the head's tree now GROWS IN PLACE for prose ticks — a paragraph
  continuing or opening on an ordinary character, a blank line, the line
  after a heading — retracting the previous projection, feeding the chunk,
  settling what it closed and publishing again, with ids handed over at
  the frontier by the same diff a rebuild uses. A tick whose chunk begins
  a line with a marker (`#`, a bullet or a number, `>`, a fence, `|`, `[`,
  `<`, `*` or `_`, a backtick, `$`, `\`, or indentation) still rebuilds
  from scratch, as does every tick while a list, quote, fence, table or
  definition is open, and a failed append leaves the head answering for no
  tree. The chain counts both kinds of tick; the shapes still rebuilding
  are the next milestone's (`docs/reviews/2026-08-13-living-tree-plan.md`).
- Breaking (C, Swift, Kotlin, and ECMAScript): the delta is gone. A
  mutation returns the successor document and nothing else — `markdown_core_commit`,
  `markdown_core_delta`, `markdown_core_diff`, the part flags, and the three
  delta accessors are removed from C, and Commit/Delta/Diff types and every
  delta decode path are removed from all three bindings. What changed is
  asked of the new tree itself: a node's `revision` is subtree-covering (the
  document revision at which its own fields, child list, or any descendant
  last changed), so a consumer walks the new tree top-down and stops
  descending wherever the (id, revision) pair is one it already holds —
  (id, revision) is the entire update protocol. The Kotlin wire format drops
  its delta section (MKC4 → MKC5).
- Breaking (ECMAScript): `MarkupID.series` is a 16-digit lowercase hex string
  instead of a `bigint`, so a document is ordinary JSON. `JSON.stringify` threw
  on every tree before this — the salt was the one `bigint` on the public
  surface — and no `toJSON` could have fixed it, because an id serialized to a
  string and read back would no longer have matched the `bigint` that
  `document.node` compares. The contract calls a series' identity opaque and
  nothing does arithmetic on it, so the value is unchanged and only its
  rendering moved: same 64 bits, still compared with `===`, still usable as a
  `Map` key, and now round-tripping through JSON as the same identity. Kotlin
  and Swift keep `ULong` and `UInt64`, which serialize on their own platforms.
- Breaking (C, Swift, Kotlin, and ECMAScript): there is no session type.
  `MarkupSession` in Swift, Kotlin, and ECMAScript and the whole
  `markdown_core_session_*` family are removed rather than deprecated: the
  former one-shot parse and the former session open are one call —
  construct a document — and the only mutation is `append`. There is no
  whole-text edit: replacing the text describes a different document, and
  the way to say so is constructing a new one, a new chain with a new
  series. Options are fixed for a chain's whole life; changing them is a
  new chain too.
- Breaking (C, Swift, Kotlin, and ECMAScript): the incremental engine is
  removed, and with it every promise built on it — no stale region, no
  snapshot that structurally shares unchanged nodes with its predecessor, and
  no per-mutation cost proportional to the change. Every mutation parses
  the whole text; the engine went because it bought no time yet, measured
  rather than assumed. What a consumer relies on is unchanged: after any
  sequence of appends a document dumps byte-for-byte equal to a
  from-scratch parse of the same text, an untouched node keeps its `id` and
  its `revision`, and a pure positional shift is not a change.
- Breaking (C, Swift, Kotlin, and ECMAScript): the session-scoped footnote
  answers are removed — numbering, resolution, first-use order, and
  back-references, with `markdown_core_session_footnote_*` and the
  `FootnoteInfo` platform type. `FootnoteDefinition` and `FootnoteReference`
  carry their `label`; a consumer that wants numbering or resolution derives
  it from the tree.
- Breaking (C, Swift, Kotlin, and ECMAScript): the per-parse identity salt is
  now called `series` everywhere it was called `lineage`
  (`markdown_core_document_series`, `MarkupID.series`, Kotlin's `seriesBits`
  and `MarkupID.fromBits`, `document.series`). Nothing about the value or its
  contract changed: a SERIES is one chain — a document and every document
  its appends produce — node raw values restart at 1 for each new series, and the salt is
  what keeps two unrelated documents' identities from comparing equal. The
  old name is removed rather than aliased.
- Breaking (C, Swift, Kotlin, and ECMAScript): formula parsing now has one
  `formulas` option. Enabling it recognizes dollar delimiters, LaTeX
  delimiters, and `formula` fenced code together; disabling it turns off the
  complete formula grammar. The separate dollar and LaTeX delimiter options
  and their native parser flags have been removed.
- Breaking (C, Swift, Kotlin, and ECMAScript): directive labels are now
  first-class `DirectiveLabel` markup in the canonical AST. A present label is
  the directive's first real child, owns its complete inline content, and has a
  scope spanning the source brackets; a missing label has no such child, while
  explicit `[]` is a zero-child `DirectiveLabel`. Typed binding properties are
  now `label: DirectiveLabel?`, and canonical dumps emit the label node instead
  of a parent `label=` scalar. The public kind inventory follows canonical AST
  order directly, including `DirectiveBlock`, `DirectiveLabel`, then
  `FootnoteDefinition`.
- Breaking (Swift): realign the Swift AST surface with the frozen
  canonical-ast contract, which forbids per-platform renames.
  - Every container's `children` collection is now `content` (`Document`,
    `BlockQuote`, `Paragraph`, `Heading`, `ListItem`, `DirectiveBlock`,
    `FootnoteDefinition`, `Emphasis`, `Strong`, `Strikethrough`, `Link`,
    `Image`), and `List` exposes the contract's typed `items: [ListItem]`.
  - Boolean fields return to their frozen names: `isTight` → `tight`,
    `isChecked` → `checked`, `isFenced` → `fenced`, `isClosed` → `closed`
    (`isHeader` is contract-defined and unchanged).
  - `DirectiveBlock` gains its separate block `content` collection.
  - `Code` and `CodeBlock` gain the contract's `mode: PlacementMode` field
    (`embedded`/`standalone` respectively).
  - Leaf kinds no longer carry an always-empty public `children` property
    (`Text`, `Code`, `CodeBlock`, `HTML`, `HTMLBlock`, `Formula`,
    `FormulaBlock`, `ThematicBreak`, `SoftBreak`, `LineBreak`,
    `FootnoteReference`); traverse structure through `MarkupWalker`.
- Breaking (C, Swift, Kotlin, and ECMAScript): every node value carries its
  own `scope` — its absolute source extent, read in O(1) off the value. The
  bulk scope table and the document-mediated lookup that went with it are
  removed: `markdown_core_document_scope_table`,
  `markdown_core_scope_table_free`, `Document.scope(of:)` in Swift,
  `Document.scope(node)` in Kotlin, and `document.scope(node)` in
  ECMAScript. `scope` is deliberately NOT part of equality on any platform,
  so a change that only shifts positions still leaves every reactive
  comparison below it unchanged. Each binding's `MarkupWalker` keeps its scope-free
  typed-visitor traversal.
- Kotlin/JVM: native-bridge and wire-decoder implementation types are no
  longer importable or constructible from Java; dedicated ABI and
  Java-compiler gates keep the documented API as the only public surface.
- Kotlin/Android: publish the exact private-JNI consumer rule in the main AAR
  and verify it with a minified release application whose own configuration
  intentionally contains no generic native-method keep. Mapping and DEX
  inspection pin the `JvmNative` binary name and every JNI method name when
  used, plus prove that an unused library is still removed.
- Tooling: update the development-only `brace-expansion` lockfile resolution
  to patched release 5.0.8.

## 2.0.0 - 2026-07-20

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
