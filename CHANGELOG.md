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
  side the head's tree GROWS IN PLACE on every tick — retracting the
  previous projection, feeding the chunk, settling what it closed and
  publishing again, with ids handed over at the frontier by pairing the
  re-derived tail against the tail it replaces — whatever the chunk brings:
  prose, headings, quotes, lists,
  fences, HTML blocks, tables, formula blocks, directives, footnotes and
  definitions. A definition that arrives re-refines exactly the units whose
  inline parse asked about its label (each unit's probes are threaded by
  label through one index), pairs their new children against the old so an
  untouched inline node keeps its id and revision, and undoes cleanly when
  the definition was only the close's; a definition streamed in byte by
  byte leaves the link that mentions it on one id across every tick. The
  cost of a tick is the chunk, the units it closed, the units a definition
  in it flipped, and the open leaf — flat across document size for prose,
  lists, quotes and definition-dense text (a footnote per two lines streams
  at 1–2 µs a tick from 256 KiB to 4 MiB), and flat for a growing fence or
  HTML block too (the block's literal borrows its own content buffer, which
  therefore never moves, and its record witnesses that buffer by length:
  0.19 µs a tick at every size from 256 KiB to 4 MiB, against 21 µs and
  396 µs before), and flat for a run of link reference definitions, which
  settles as it is fed: a definition is final as soon as a `[` stands where
  its title would have begun, so `add_line` takes it off the paragraph
  there and the close is left the last one alone (2.0 µs a tick at every
  size, against 87 µs rising to 713 µs before; a 340 KiB article carrying a
  references section streams in 74 ms, against 8.6 s), and flat for a leaf
  that is the whole document: a block still open KEEPS the inline children
  a tick settled under it, and the next tick's refine resumes after them
  rather than deriving the leaf again (a 4 MiB paragraph with every
  extension on costs 0.055 ms a tick, against 14.4 ms; 1 MiB costs 0.024 ms
  a tick against 2.837 ms). What a refine may keep is what the handlers
  that ran say they READ, so a handler that looks past what it consumed
  says so and the prefix stops there. The pairing at
  the frontier aligns what its sweeps leave, so a run changed at both ends
  in one chunk — a definition flipping a link at its front while its tail
  grows — keeps the unchanged siblings between. Neither what a chunk BRINGS
  nor the length of the run it lands in is charged to the pairing: a chunk
  of three hundred lines and a paragraph of two hundred thousand keep every
  id alike, so no document's size decides an identity. What is bounded is
  the CHANGES, at 64 unmatched children per run; past that the run is being
  rewritten rather than edited and it pairs positionally, as it always did.
  An append never rebuilds:
  there is no other kind of tick, and a failed append leaves the head
  answering for no tree.
- Added (C extension API): `markdown_core_inline_parser_note_read`, for an
  inline handler to say how far past its match it looked, and
  `markdown_core_parser_settled_inline_child`, for a block postprocessor to
  begin after the children a previous tick settled. Both are optional and
  silence is the old answer: a handler that says nothing is assumed to have
  read to the end of the buffer, which stops the settled prefix where it
  always stopped, and a postprocessor that does not ask walks every child
  as it always did. An extension needs neither unless it wants a streaming
  tick to cost what changed rather than what the block holds.
- Breaking (C extension API): an extension that opens blocks
  (`try_opening_block`) and allocates payloads (`alloc_opaque`) must also
  provide `opaque_size` — the size of the plain-data payload behind a
  block's pointer, which a stream snapshots before a speculative close and
  puts back after — or `markdown_core_parser_attach_extension` refuses it.
  Every bundled extension that opens blocks and allocates payloads (table,
  formula, directive) provides it; one that opens blocks without payloads
  (task lists) or allocates payloads without opening blocks (cross links,
  embeds) is unaffected. With that, no build can end in a state
  the engine cannot reopen, so the rebuild path, the whole-tree diff and
  the whole-tree hash stamp are gone — a one-shot parse no longer stamps
  every node (6–11% of parse time on the throughput corpora), and the
  subtree hash is stamped on exactly the subtrees a tick pairs.
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
