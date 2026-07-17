# v2 incremental sessions: design and delivery plan

Status: approved design (2026-07-15); milestones M0–M2 delivered (M2 on
2026-07-16: refmap v2, per-block postprocess pipeline, footnote projection
scaffolding, equivalence suite). The binding contract text
lives in `../specs/sessions-and-changesets.md`; this document records why the
design is shaped this way, the exact engine architecture, the deltas to the
frozen v1 contracts, and the milestone/gate plan.

## Context

Markdown Core 1.0.3 parses complete sources only. Appending one streamed
token costs a full reparse plus a full platform-tree copy — O(n) per token,
O(n²) per stream — and every parse yields a brand-new object graph with no
node identity and no equality (Swift nodes are not `Equatable`; Kotlin and ES
nodes compare by reference), so UI frameworks re-render everything on every
update.

v2 makes the engine incremental (arbitrary insert/replace/delete; append-only
token streaming as the hot path), gives every node a stable id and an O(1)
content-equality contract on all platforms, and redesigns the consumer API
around sessions, immutable snapshots, and changesets.

## Decisions

1. Input model: full incremental editing; append is the optimized common
   case.
2. Update model: immutable snapshots with structural sharing plus a
   per-commit changeset.
3. Compatibility: clean v2.0 break; `Document.parse` survives as a
   convenience over the session core.
4. Platforms: C + Swift + Kotlin + ES in lockstep under one contract.
5. Footnotes ride the reference-map machinery (decided 2026-07-16,
   superseding the original "keep v1 observable behavior via incremental
   projection" — the superseded design is recorded in the appendix):
   definitions stay at their source position, used or not; references always
   parse as footnote-reference nodes carrying their label (no degradation to
   text, no renumbering); numbering, first-use order, resolution state, and
   back-reference ordinals are queries over a session-maintained index.
   Ordinal/resolution changes surface as revision bumps (`changed` entries)
   with no content or structure change. Unresolved *link* references keep
   the CommonMark behavior (literal text) — link formation is grammar, not
   semantics, and changing it would fork the dialect.
6. Scope moves off node values; absolute positions are resolved via
   `document.scope(of:)` and walker events.
7. Pure session-based C API with **zero process-global state**: any number of
   sessions usable concurrently with no global locks and no cross-session
   interference; each session purely local and thread-confinable; purity
   delivered at the binding layer (immutable snapshots, pure one-shot parse).
   Pure-function C API and platform-nodes-over-C-AST were evaluated and
   rejected — see the appendix; no work is planned from the appendix.

Core invariants: equivalence (incremental result dump-equals a from-scratch
parse of the final text), O(1) shift-invariant equality, per-commit cost
bounded by touched leaves + changeset and never worse than one full parse.

## Engine facts the design builds on (verified against source)

- The block phase is already line-incremental (`core/blocks.c`):
  `S_process_line` → `check_open_blocks` → `open_new_blocks` →
  `add_text_to_container`; blocks finalize as lines arrive; paragraph
  finalize harvests link-reference definitions into `parser->refmap`. The
  internal `parser_new/feed/finish` API exists but is not public; the facade
  (`extensions/ast.c`) feeds once.
- Everything else is whole-document at `parser_finish`
  (blocks.c:1654-1712): `process_inlines` (whole-tree pass over a by-then
  frozen refmap), `process_footnotes` (reparents used definitions, drops
  unused, renumbers), `consolidate_text_nodes`, autolink and formula
  extension postprocess passes (whole-tree walks with block-local effects),
  optional HTML-comment strip. `finish` is terminal.
- `assert(!map->prepared)` (references.c:38): no definition inserts after the
  first lookup. "Oldest wins" is insertion-order — wrong under edits; must
  become document-order.
- `struct markdown_core_node` (node.h:59-98) has absolute positions, unused
  `user_data`, spare flag bits, and no id/version fields. Inline TEXT
  literals borrow the parent block's content buffer.
- Process-global state inventory: `core/arena.c:7` global arena stack
  (mutable, non-thread-safe, unused by the facade path — delete);
  `core/registry.c:13` process-global extension list (once-initialized,
  read-only — replace with a const table or per-session registration); the
  ES binding's WASM module singleton with shared mutable scratch
  (`src/runtime/native.ts`, `src/wire/node-decoder.ts` — scratch becomes
  per-session). `special_chars`/`skip_chars` are already parser-local.

## Architecture

### Layering

The C core keeps one living, mutable tree per session and emits changesets.
Immutable snapshots with structural sharing are a binding-layer construct.
The C `session_document` view is borrowed and valid until the next mutating
call.

### C core: `markdown_core_session`

New files `core/session.c/.h`, `core/text.c/.h`, `core/damage.c`,
`core/adopt.c`, `core/changeset.c`; surgical changes in `blocks.c`, `node.h`,
`map.c`, `references.c`, `footnotes.c`, `inlines.c`, `extensions/ast.c`,
`extensions/autolink.c`, `extensions/formula.c`.

Session owns: persistent parser (never "finished"), chunked line store,
living DOCUMENT root, id allocator + lineage salt + revision counter,
id→node table, coalesced edit queue, refmap v2, footnote projection index,
and the saved open-block frontier.

Node additions (~16-24 bytes): `uint64_t id; uint64_t last_changed_rev;`
plus internal flags `CLEAN_START` (document children) and `SEALED_RELATIVE`.

Positions: the parser keeps writing absolute positions into open nodes
exactly as today. At seal time (block finalized + inlines done) positions
convert to parent-relative line deltas (columns are line-local, already
shift-safe). Suffix adoption after an edit then touches only adopted subtree
roots. `markdown_core_node_scope()` keeps its v1 signature and resolves
absolutes via the parent chain; the dump resolves scopes with a running
accumulator, so the dump grammar and goldens are unchanged.

Text store: stores the raw bytes exactly as edited (never normalized — NUL
and invalid UTF-8 are replaced with U+FFFD during parsing, per line, exactly
as the one-shot path does, so a streamed append can complete a multi-byte
character split across edits). M1 ships a contiguous-buffer implementation
behind the final interface; the chunked line records with per-chunk line/byte
bases (edit O(log chunks + touched lines), append O(1) amortized) arrive with
M3 damage planning, which is their only consumer.

### Commit pipeline

```
 1. Damage plan  — edits → lines → document children; extend backward to the
                   nearest CLEAN_START child. Fast path: all edits beyond the
                   last sealed byte (streaming append) ⇒ damage = ∅, go to 4.
 2. Retract      — remove refmap and footnote-map definitions owned by
                   damaged children; detach damaged children into a graveyard.
 3. Staged reparse — feed lines through the existing S_process_line machinery
                   from damage start; after each line, resync check; on
                   resync adopt the entire old suffix (splice + one
                   relative-line fix + frontier restore shifted by Δ), else
                   continue to EOF (graceful degradation ≤ 1 full parse).
 4. Frontier feed — append lines into the open-block chain (v1 hot path).
 5. Block adoption — graveyard vs staged children: byte-identical spans
                   transplant wholesale; edit-intersecting blocks adopt ids
                   by (kind, ordinal).
 6. Inline phase — only dirty leaves (new/reparsed/refmap-dependent/open
                   frontier): parse_inlines → block-local pipeline:
                   consolidate_text_nodes(block) → autolink block postprocess
                   → formula block promotion → HTML-comment strip.
                   Inline-level id adoption keeps the trailing frontier Text
                   id stable.
 7. Refmap diff  — per-label winner (min document order) delta ⇒ dirty
                   dependent leaves ⇒ rerun 6 for them once.
 8. Footnote index refresh — recompute first-use ordinals/resolution; bump
                   revisions of references whose query answers changed.
 9. Seal         — relativize new nodes, set CLEAN_START, bump
                   last_changed_rev + bubble (recording `bubbled`), build the
                   changeset, free the graveyard, revision++.
```

Resync condition: the current byte offset maps through the cumulative edit
delta to the old start of a surviving document child with `CLEAN_START`, and
the staged parse state is clean (`parser->current == root`, no open
descendants). `CLEAN_START` is recorded at `open_new_blocks` time ("document
was the only open block when this child's first line began") and subsumes
the adjacency hazards: setext underlines, lazy continuation, table delimiter
look-back, and paragraph interruption all attach to an open block, so the
resulting blocks are never clean-started. Unclosed fences / HTML blocks /
`:::` directives keep the state un-clean ⇒ natural reparse-to-EOF. List
tightness changes update only the List node (items transplant).

Id adoption levels: L0 suffix transplant (zero work), L1 block-level
two-pointer match by span/kind/ordinal (O(damaged children)), L2
inline-level prefix/suffix byte-match with unique-middle-candidate adoption
(O(touched leaf)). Kind mismatch never adopts.

Transactional OOM: steps 3-7 build into staging structures; the living tree
is spliced only after all allocation-bearing work succeeded. On OOM the
staging is freed, the graveyard re-attached, an error returned; the session
stays valid at the previous revision and commit is retryable.

### Reference map v2

Remove the `prepared` freeze; `markdown_core_key_index` gains remove support;
per-label bucket lists keep all definitions; winner = minimum document order.
Definitions record their owning document-child id so damage retracts by
owner. Inline parsing records every label lookup (hits and misses) against
the leaf id; commits diff per-label winners and dirty exactly the dependent
leaves.

### Footnotes: source-order AST over the reference map (revised 2026-07-16)

Footnote definitions harvest their labels into the same map machinery as
link reference definitions (owner ids, document-order winner, remove-owned
retraction); footnote references record lookups per leaf and re-dirty
through the same refmap-delta path (step 7). The tree is source-faithful:
definitions never move and are never dropped; references always exist and
carry their label. The session maintains a footnote index (label →
definition, references in document order, first-use order) from which
numbering, resolution state, and back-reference ordinals are answered as
queries; commits diff the index and bump the revision of references whose
ordinal or resolution changed (`changed` entries with identical dump
fields). The GFM placement/renumber/drop behavior becomes a renderer or
consumer concern, aligned with the mdast model. The M2 projection
scaffolding's placement phase is deleted when this lands; the node fields
`parent_footnote_def`, `footnote.ref_ix`, and `footnote.def_count` become
removable.

### Extension pipeline

The internal extension ABI gains `postprocess_block(ext, parser, block)`
(typedef `markdown_core_postprocess_block_func`); autolink and formula
convert to it and run per dirty block. The whole-tree `postprocess_func` is
removed from bundled extensions.
Table/tasklist/strikethrough/smart-punctuation/directive are untouched.
HTML-comment strip becomes per-node at seal. Formula promotion, table
retyping, and setext flips change the node kind ⇒ removed + added.

## Public API

### C facade

Additions to `include/markdown_core.h` and `core/exports/markdown_core.map`
(all names pass the existing symbol-ban regex):

```c
typedef struct markdown_core_session markdown_core_session;
typedef struct markdown_core_changeset markdown_core_changeset;
typedef uint64_t markdown_core_node_id;

markdown_core_session *markdown_core_session_open(const markdown_core_parse_options *, markdown_core_error **);
void   markdown_core_session_free(markdown_core_session *);
bool   markdown_core_session_edit(markdown_core_session *, size_t byte_start,
                                  size_t byte_end, const uint8_t *bytes,
                                  size_t length, markdown_core_error **);
bool   markdown_core_session_commit(markdown_core_session *,
                                    markdown_core_changeset **, /* nullable */
                                    markdown_core_error **);
const markdown_core_document *markdown_core_session_document(const markdown_core_session *);
uint64_t markdown_core_session_revision(const markdown_core_session *);
uint64_t markdown_core_session_lineage(const markdown_core_session *);
size_t   markdown_core_session_length(const markdown_core_session *);
const markdown_core_node *markdown_core_session_node_by_id(const markdown_core_session *, markdown_core_node_id);

markdown_core_node_id markdown_core_node_get_id(const markdown_core_node *);
uint64_t markdown_core_node_get_revision(const markdown_core_node *);
const markdown_core_node *markdown_core_node_get_parent(const markdown_core_node *);

void   markdown_core_changeset_revisions(const markdown_core_changeset *, uint64_t *before, uint64_t *after);
size_t markdown_core_changeset_added(const markdown_core_changeset *, const markdown_core_node_id **);
size_t markdown_core_changeset_removed(const markdown_core_changeset *, const markdown_core_node_id **);
size_t markdown_core_changeset_changed(const markdown_core_changeset *, const markdown_core_node_id **);
size_t markdown_core_changeset_bubbled(const markdown_core_changeset *, const markdown_core_node_id **);
void   markdown_core_changeset_free(markdown_core_changeset *);
```

`markdown_core_document_parse` is reimplemented as open + edit + commit +
detach-root.

### Bindings (mirror-by-changeset, same architecture ×3)

Each platform `MarkupSession` owns the C session plus a mutable id → node
mirror. Per commit it decodes only `added ∪ changed` payloads, rebuilds
`bubbled` ancestors reusing unchanged children, evicts `removed`, and emits a
new immutable `Document` sharing everything unchanged — O(changeset·depth).
Equality everywhere is `(lineage, id, revision)`.

- Swift: structs stay `Sendable`; `Markup` gains `id`/`revision`; all 28
  kinds become `Equatable/Hashable/Identifiable`; `MarkupSession` is a
  non-`Sendable` class with `append`/`replace`/`commit` and an
  `updates(feeding:)` async sequence.
- Kotlin: classes gain `id`/`revision` + `equals`/`hashCode`; the native
  bridge gains `markdown_core_kotlin_session_{open,edit,commit,free}` and an
  MKC3 delta wire (removed ids | bubbled (id, childIds) | changed/added full
  records); `MarkupSession.updates(input: Flow<String>): Flow<Commit>`.
- ES: nodes stay plain objects plus `id` (number, documented < 2^53) and
  `revision`; `bridge.c` exports `es_session_*`; `MarkupSession` with
  `edit`/`append`/`commit`/`updates(AsyncIterable<string>)`.

## Deltas to frozen v1 contracts (applied at the milestone that implements them)

| Artifact | Change |
| --- | --- |
| `docs/specs/canonical-ast.md` | 28-kind inventory unchanged; every kind gains `id`/`revision`; `scope` becomes query/walker-supplied; equality+identity section added; `MarkupSession` added as a canonical entry point; footnote contract revised (source-order definitions, label-carrying references, query-based numbering/resolution — decision #5 as revised 2026-07-16). |
| `docs/specs/canonical-ast-dump.md` | Grammar unchanged; note that subtree dumps are subtree-origin. Full-document goldens in `specs/canonical-ast/` stay byte-identical through M2; footnote-bearing goldens regenerate deliberately when the revised footnote contract lands (M3). |
| `docs/specs/test-architecture.md` | Add equivalence, id-stability, and edit-storm suites to the frozen topology. |
| `scripts/audit-public-surface.sh` | Add the new C symbols to the header+map sync; scope the Swift/Kotlin/ES mutation-ban greps to the model/walker directories (Session's `append`/`replace` would otherwise trip them); pin the Session surfaces exactly; ES frozen runtime export list gains `MarkupSession`. |
| Build lists ×4 | `packages/markdown-core/core/CMakeLists.txt` (+`extensions/CMakeLists.txt`), root `Package.swift`, `packages/es-markdown-core/scripts/build.mjs`, and `packages/kotlin-markdown-core/android-runtime/src/main/cpp/CMakeLists.txt` (the Android runtime keeps its own explicit engine list): add `session.c`, `text.c`, `damage.c`, `adopt.c`, `changeset.c`. New facade symbols go into **both** export allowlists: `core/exports/markdown_core.map` (Linux version script) and `core/exports/markdown_core.exports` (macOS exported-symbols list). |

## Milestones and gates

- **M0 — Contracts** (this document + `sessions-and-changesets.md`). Gate:
  review; existing audits stay green.
- **M1 — Ids, relative positions, session skeleton** (correct, not yet
  incremental): zero-global-state workstream (delete global arena,
  const/per-session registry, per-session ES scratch); node id/revision/
  flags; seal-time relativization + parent-chain scope; text store; session
  where every commit is a full staged reparse + adoption + changeset; rebase
  `document_parse`; facade symbols/exports/build lists; audit-script v2.
  Gates: full v1 suites + goldens unchanged, complexity ≤ 4.0 (one-shot fast
  path), oom_sweep, TSan, audit.
- **M2 — Semantic core**: refmap v2, per-block inline pipeline + extension
  `postprocess_block`, per-node comment strip, footnote projection
  scaffolding (full recompute per commit). Gates: goldens unchanged,
  equivalence runner incl. link-ref edit fixtures, sanitizers.
- **M3 — True incrementality**: damage planning + CLEAN_START, resync +
  suffix adoption + frontier save/restore, graveyard/transplant,
  refmap-delta inline dirtiness, footnote contract change (source-order
  tree, query-based numbering — regenerate footnote goldens and spec deltas
  first), footnote index + query API, append fast path, transactional OOM
  staging. Gates: adversarial equivalence fixtures (setext, lazy
  continuation, unclosed fence/HTML/directive, table delimiter edits,
  list-tightness flips, footnote ordinal-shift cascades,
  no-blank documents), changeset-mirror check, id-stability tests, new
  complexity cases (`session_stream_flat`, `session_edit_storm`), oom_sweep
  session variant, TSan.
- **M4 — Bindings**: Swift, Kotlin (MKC3), ES sessions; binding conformance
  replays the equivalence corpus through sessions. Gates: all platform
  conformance/dump gates, platform id-stability + O(1)-equality tests, delta
  benchmarks, audit v2, and the public-surface naming freeze review
  (`docs/specs/c-naming.md`) before anything ships.
- **M5 — Hardening & release**: edit-script fuzz target, pathological corpus,
  perf tuning, CHANGELOG/VERSION 2.0.0, release dry-run. Gates: full CI
  matrix + package audits.

## Verification

- **Equivalence gate** (`equivalence_runner.c`, CTest): every canonical
  fixture + CommonMark spec case parsed batch, per-line append replay,
  sampled per-byte replay, and N seeded random edit scripts — final dump
  byte-equal to a one-shot parse of the final text. Plus a
  **changeset-mirror check**: maintain an id→(kind,parent,ordinal,rev)
  shadow from changesets only and compare against a fresh walk after every
  commit (catches adoption bugs dumps cannot see).
- **Id stability**: streaming keeps frontier paragraph + trailing Text ids;
  a clean-boundary insert at the top leaves every downstream (id, revision)
  intact; kind change ⇒ removed + added.
- **Concurrent multi-session gates** (decision #7): C `concurrency_runner`
  `sessions` case (N threads × one session each, interleaved
  edit/commit/read + cross-thread snapshot reads) under TSan, plus an
  `nm`-based check that no writable globals remain in engine objects; Swift
  strict-concurrency test (Sendable snapshots across isolation domains,
  non-Sendable session); Kotlin parallel sessions on JVM executors and
  native workers; ES multiple sessions per context and per-worker WASM
  instantiation with no shared scratch.
- **Perf**: per-commit append cost flat versus document size; whole-stream
  cost ~O(n); existing doubling ratios and binding boundary benchmarks stay
  green; new delta-decode benchmarks.
- **Safety**: oom_sweep over `session_commit` allocation sites (previous
  revision intact + retryable); ASan/UBSan/TSan presets over session tests.

## Risks and mitigations

- No-clean-boundary documents (one giant list/quote): damage = whole block;
  bounded at ≤ 1 full parse; binding cost still O(changed) via transplants;
  the damage planner keeps a restart-stack abstraction so a later release
  can add list-item-level restarts.
- Huge single-paragraph streams: inline reparse is O(open leaf) per commit;
  documented cost model + commit-coalescing guidance.
- Unclosed fence over a large suffix: reparse-to-EOF per commit while
  unclosed; suffix ids survive via span transplants.
- Adoption bugs → stale binding mirrors: changeset-mirror gate + seeded edit
  fuzzing target exactly this.
- Footnote ordinal-shift cascades: inserting an early first-use reference
  shifts every later ordinal; surfaces as revision-only `changed` entries
  (no content or structure change); index refresh bounded by #refs + #defs.
- One-shot perf regression: the M1 fast path is benchmarked before anything
  is built on it.

## Appendix: rejected alternatives (rationale record only — no planned work)

- **Footnotes as an incremental projection of the v1/GFM observable
  behavior** (original decision #5, superseded 2026-07-16): the session
  would have kept used definitions moved to the document tail in first-use
  order, dropped unused ones, and renumbered reference literals, re-applied
  incrementally per commit (unproject at commit start, re-project at the
  end). Superseded because the moved/renumbered shape is itself an artifact
  of cmark-gfm's decision to implement footnotes as a post-parse pass over
  an untouchable core: it forces tree restructuring and literal rewrites on
  every ordinal shift (large changesets, heavy UI re-renders), leaves
  unresolved references degrading to text after the block pipeline (which
  the M2 pipeline had to work around), and duplicates machinery the
  reference map already provides. The M2-delivered projection scaffolding
  (collect/resolve/apply in `core/footnotes.c`) remains the interim
  implementation until the revised contract lands in M3.

- **Pure-function C API** (`new_doc = edit(old_doc, …)`, old stays valid):
  requires a persistent C tree — coexisting snapshot versions sharing
  unchanged subtrees mean shared ownership, i.e. atomic per-subtree
  refcounts (tree-sitter's internals), no parent pointers (a shared node
  cannot have two parents — breaking scope resolution and iterators), and a
  rewrite of the in-place block machinery. It also duplicates the
  persistence the bindings already provide. A consuming/linear variant is
  semantically a session with a use-after-free footgun.
- **Platform nodes as views over the C AST** (no boundary copy): UI diffing
  holds old and new snapshots simultaneously, forcing the same persistent
  refcounted C tree; additionally every property read on Kotlin JVM/Android
  becomes a JNI crossing (the anti-pattern the MKC2 wire was built to
  avoid), and ES nodes holding WASM pointers need `FinalizationRegistry`
  (non-deterministic frees; leaked snapshots pin native memory).
