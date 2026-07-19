# v2 incremental sessions: design and delivery plan

Status: approved design (2026-07-15); milestones M0–M2 delivered (M2 on
2026-07-16: refmap v2, per-block postprocess pipeline, footnote projection
scaffolding, equivalence suite). M3 substantially delivered in two
workstreams: the revised footnote contract landed first (2026-07-16) —
projection deleted, source-order tree, session footnote index +
`session_footnote_*` query API, ordinal/resolution revision bumps, footnote
goldens and spec deltas regenerated — and true incrementality second
(2026-07-16, `extensions/incremental.c`): CLEAN_START restart planning, staged
reparse with resync + suffix transplant, graveyard adoption through the
standard machine, session-persistent reference map with
definition-sequence reconciliation, transactional splice, and the
equivalence/complexity gates. Interim M3 simplifications that remain open
are listed under the M3 milestone below. The binding contract text
lives in `../specs/sessions-and-deltas.md`; this document records why the
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
around sessions, immutable snapshots, and deltas.

## Decisions

1. Input model: full incremental editing; append is the optimized common
   case.
2. Update model: immutable snapshots with structural sharing plus a
   per-commit delta.
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
bounded by touched leaves + delta and never worse than one full parse.

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

The C core keeps one living, mutable tree per session and emits deltas.
Immutable snapshots with structural sharing are a binding-layer construct.
The C `session_document` view is borrowed and valid until the next mutating
call.

### C core: `markdown_core_session`

New files `core/session.c/.h`, `core/text.c/.h`, `core/incremental.c` (named `damage.c`
in the original plan; renamed at delivery — the file holds the whole commit
pipeline, of which planning the stale region is only the first step; the
shipped code says "restart plan" and "stale region" for the same reason),
`core/adopt.c`, `core/delta.c`; surgical changes in `blocks.c`, `node.h`,
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
                   delta, free the graveyard, revision++.
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
consumer concern, aligned with the mdast model. Landed 2026-07-16 as the
first M3 workstream: the M2 projection scaffolding (`core/footnotes.c`) is
deleted, the node fields `parent_footnote_def`, `footnote.ref_ix`, and
`footnote.def_count` are removed, the index lives in
`extensions/footnote.c`, and the query surface is
`markdown_core_session_footnote_info/footnotes/footnote_references`.

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
typedef struct markdown_core_delta markdown_core_delta;
typedef uint64_t markdown_core_node_id;

markdown_core_session *markdown_core_session_open(const markdown_core_parse_options *, markdown_core_error **);
void   markdown_core_session_free(markdown_core_session *);
bool   markdown_core_session_edit(markdown_core_session *, size_t byte_start,
                                  size_t byte_end, const uint8_t *bytes,
                                  size_t length, markdown_core_error **);
bool   markdown_core_session_commit(markdown_core_session *,
                                    markdown_core_delta **, /* nullable */
                                    markdown_core_error **);
const markdown_core_document *markdown_core_session_document(const markdown_core_session *);
uint64_t markdown_core_session_revision(const markdown_core_session *);
uint64_t markdown_core_session_lineage(const markdown_core_session *);
size_t   markdown_core_session_length(const markdown_core_session *);
const markdown_core_node *markdown_core_session_node_by_id(const markdown_core_session *, markdown_core_node_id);

markdown_core_node_id markdown_core_node_get_id(const markdown_core_node *);
uint64_t markdown_core_node_get_revision(const markdown_core_node *);
const markdown_core_node *markdown_core_node_get_parent(const markdown_core_node *);

void   markdown_core_delta_revisions(const markdown_core_delta *, uint64_t *before, uint64_t *after);
size_t markdown_core_delta_added(const markdown_core_delta *, const markdown_core_node_id **);
size_t markdown_core_delta_removed(const markdown_core_delta *, const markdown_core_node_id **);
size_t markdown_core_delta_changed(const markdown_core_delta *, const markdown_core_node_id **);
size_t markdown_core_delta_bubbled(const markdown_core_delta *, const markdown_core_node_id **);
void   markdown_core_delta_free(markdown_core_delta *);
```

`markdown_core_document_parse` is reimplemented as open + edit + commit +
detach-root.

### Bindings (mirror-by-delta, same architecture ×3)

Each platform `MarkupSession` owns the C session plus a mutable id → node
mirror. Per commit it decodes only `added ∪ changed` payloads, rebuilds
`bubbled` ancestors reusing unchanged children, evicts `removed`, and emits a
new immutable `Document` sharing everything unchanged — O(delta·depth).
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
| Build lists ×4 | `packages/markdown-core/core/CMakeLists.txt` (+`extensions/CMakeLists.txt`), root `Package.swift`, `packages/es-markdown-core/scripts/build.mjs`, and `packages/kotlin-markdown-core/android-runtime/src/main/cpp/CMakeLists.txt` (the Android runtime keeps its own explicit engine list): add `session.c`, `text.c`, `incremental.c`, `adopt.c`, `delta.c`. New facade symbols go into **both** export allowlists: `core/exports/markdown_core.map` (Linux version script) and `core/exports/markdown_core.exports` (macOS exported-symbols list). |

## Milestones and gates

- **M0 — Contracts** (this document + `sessions-and-deltas.md`). Gate:
  review; existing audits stay green.
- **M1 — Ids, relative positions, session skeleton** (correct, not yet
  incremental): zero-global-state workstream (delete global arena,
  const/per-session registry, per-session ES scratch); node id/revision/
  flags; seal-time relativization + parent-chain scope; text store; session
  where every commit is a full staged reparse + adoption + delta; rebase
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
  no-blank documents), delta-mirror check, id-stability tests, new
  complexity cases (`session_stream_flat`, `session_edit_storm`), oom_sweep
  session variant, TSan.

  Delivered 2026-07-16 with these deviations and interim simplifications:

  - No persistent parser / frontier save/restore: every commit runs a fresh
    staged parser from the restart point (the last CLEAN_START document
    child at or before the first edited byte). The streaming append fast
    path is therefore "reparse the unclosed tail region", which has the same
    per-commit asymptotics as frontier feeding (the inline phase re-parses
    the open leaf either way) at the cost of re-running the block phase over
    the restart child's lines. `session_stream_flat` holds at ~1x across a
    1024x document-size spread. Frontier save/restore remains available as a
    later constant-factor refinement.
  - ~~Refmap-delta inline dirtiness is coarse~~ — resolved 2026-07-17
    (per-unit lookup records and winner-delta reconciliation): the map's
    lookup path gained an observation sink; each inline-owning unit's
    normalized label lookups (hits and misses alike, misses against an
    empty map included) persist in a session table keyed by unit id,
    maintained by both commit paths and skipped for the one-shot
    convenience parse. A commit whose definition sequences differ now
    reconciles the session map in place instead of falling back: stale
    entries leave, staged entries take document orders in the vacated span
    (whole-map renumber when the span is too small — rare, O(definitions)),
    affected buckets relink to prefix → staged → suffix, and labels whose
    winning (url, title) payload changed name the dependent units through
    the lookup table. Dependents rebuild as cloned shells re-refined
    against the reconciled map through the per-unit pipeline
    (`markdown_core_parser_refine_unit`), adopt pairwise for id stability,
    splice by pointer surgery inside the existing transaction, and bubble
    ancestor revisions. Only paragraphs and headings rebuild per-unit;
    extension-owned units (table cells, directives) still fall back to a
    full reparse before anything is touched. Once the map surgery has run,
    a failing commit marks the map stale (`refmap_stale`) and the next
    commit takes the full path, which rebuilds the map wholesale — the
    session stays valid at its previous revision throughout. Gates:
    `ref_*` boundary scripts in the equivalence runner (retarget with
    quoted/heading dependents, empty-map miss resolution, losing-duplicate
    edits that stay re-run-free, renumber, table-cell fallback, the
    footnotes-enabled `[^n]` link/footnote-reference kind flip) and the
    `session_ref_retarget` complexity case (definition-edit commits flat
    across a 1024x document-size spread).
  - ~~The footnote index is still rebuilt by a whole-tree walk per commit~~
    — resolved 2026-07-17 (session-persistent footnote sites): the index
    keeps its definition and reference nodes as document-ordered site lists
    (node, anchoring document child, owning inline unit), collected by the
    full path's walk and merged in place by incremental commits before
    adoption: sites anchored in the graveyard leave and the staged region's
    freshly collected sites take their place (classified against surviving
    anchors' restart-relative lines), a rebuilt unit's sites are replaced by
    its clone's at the same position, and everything else survives
    untouched. Numbering, grouping, and the revision diff then rebuild from
    the sites alone — O(#refs + #defs) per commit, never the tree. A clone
    that gains footnote sites where its unit had none (the `[^n]`
    link/footnote-reference kind flip) has no merge position and falls back
    to the full path. Gates: the equivalence runner's footnote-query check
    (every footnote-enabled commit's numbering, resolution, and
    back-reference answers must equal a fresh session's on the same text,
    node identity mapped positionally over the already-dump-equal trees),
    the `footnote_sites` boundary script (empty staged runs, ordinal
    cascades through arriving/departing mid-document sites, top-level and
    quoted dependent rebuilds carrying references, a combined commit whose
    staged sites must precede the rebuilt suffix units' runs, tail-cluster
    definition deletion), the widened session oom_sweep stage (a dependent rebuild
    carrying footnote sites under allocation injection), and the
    `session_footnote_shift` complexity case (label-flip commits against a
    fixed footnote cluster flat across a 1024x document-size spread —
    552x before this change).
  - The text store stays contiguous; the chunked line records were not
    needed because restart planning derives geometry from the clean-child
    index (start bytes and lines of CLEAN_START document children,
    updated in place per commit) plus per-commit fed-line offsets. Suffix
    bookkeeping after a resync is O(top-level suffix children), and only
    when the commit shifted line numbers or byte offsets.
  - The inline phase of an incremental commit runs with an unlimited
    reference-expansion budget; a session-tracked upper bound on the
    one-shot expansion total proves the equivalent one-shot parse would not
    have denied any lookup either, and commits past the bound fall back to
    a full reparse (which re-measures it).
  - ~~Deferred to issue #15 (restart locality)~~ — resolved 2026-07-17
    (sentinel clean entries + a line-ordered definition index): map entries
    carry the harvesting paragraph's start line (stamped at add time like
    the owner, shifted with the suffix), and a vanished clean definition
    paragraph leaves a `node == NULL` sentinel entry in the clean index —
    a valid restart point and resync boundary, resolved to the first real
    child at or beyond its line (real children can appear inside a cluster
    when a paragraph stops vanishing). Staleness and prefix/suffix
    classification became pure line-interval queries over a
    session-persistent, line-ordered at-rest definition index
    (`session->def_index`): stale collection is a binary-searched range,
    reconcile extrema come from the range's neighbors (slice copies only
    on renumber), stale entries unlink O(1) through new map back links,
    and the staged range splices in place (capacity reserved during
    prepare; `refmap_stale` covers aborted reconciliations). Owner-based
    region classification is gone; anchors remain only as parse-local
    vanish markers. The session counts full/restarted/resynced commits
    (`fallback_runner --case restart_locality_counters` pins them), so
    degraded-to-full cases stay counted. Gates: the `head_defs` boundary
    script (mid-cluster retargets, definition paragraphs arriving/leaving,
    pure-shift gap edits, a paragraph flipping between vanishing and
    real), and the `session_head_defs` complexity case (last-definition
    retargets against a document-scale leading cluster, flat across a
    1024x spread — 0.8x measured, ~3167x before the change). The
    resync-delay behaviors carried in the issue's orbit both resolved
    (issue #26, M5): the blockquote half no longer reproduced after this
    index rework and is pinned by `session_quote_suffix`; the footnote
    half (definitions stay open across blank lines) landed as the sealing
    rule below.
- **M4 — Bindings**: Swift, Kotlin (MKC3), ES sessions; binding conformance
  replays the equivalence corpus through sessions. Gates: all platform
  conformance/dump gates, platform id-stability + O(1)-equality tests, delta
  benchmarks, audit v2, and the public-surface naming freeze review
  (`docs/specs/c-naming.md`) before anything ships.

  Swift workstream delivered 2026-07-17 (Kotlin and ES remain), with the
  freeze-review outcomes applied surface-wide:

  - Platform names concretized and user-approved: `MarkupID`
    `{ lineage, rawValue }` (Identifiable uses it revision-free; equality is
    id + revision), `Commit { document, changes: Delta }`, and the
    per-commit record renamed **delta** everywhere — C
    `markdown_core_delta_*`, file `extensions/delta.c`, spec file
    `sessions-and-deltas.md`. The footnote field is `label` on platforms
    (`id` names node identity); the dump grammar keeps its `id=` key.
  - Scope mechanics settled: deltas deliberately omit pure positional
    shifts (a blank-line-only edit commits an empty delta), so platform
    node values carry no positions; dump/walk are document-mediated, and a
    session snapshot lazily materializes all scopes from the C tree on
    first use while current, becoming self-contained afterwards. The C
    borrowed-view contract was sharpened to match: edits never invalidate
    the committed view, only commit/free do.
  - `MarkupSession.document` is non-optional — the revision-0 empty root is
    addressable and mirrored from `session_open`.
  - One-shot and session decoding are one pipeline: `Document.parse` is
    session sugar over a delta-free first-commit bulk build (post-order
    frame stack, one mirror write per node, no delta materialization — the
    C nullable out-parameter is exactly this knob). Measured cost of the
    unification on the one-shot decode boundary: ~15-25% on
    `large_document` (mirror construction), accepted for the single
    pipeline; the delta path is untouched.
  - Engine fix found by the new Swift delta-mirror gate: the incremental
    path recorded the document in `changed` (via the staged-root dummy
    verdict) while the footnote index diff, running before the document's
    revision stamp, re-collected it as `bubbled` — the same latent hazard
    existed for dependent-rebuild bubble ancestors. Revision stamps now
    land before the footnote diff (rolled back if the diff fails); the
    equivalence runner gained a corpus-wide delta-disjointness check.
  - Swift gates in place: `sessions` suite (id-stability across streaming /
    clean-boundary inserts / kind changes, empty-delta pure shifts, scope
    materialization survival, footnote queries, `updates(feeding:)`),
    conformance session replay of the manifest corpus with per-commit dump
    equality + delta-mirror integrity, session streaming benchmark
    (`session_stream_and_delta_decode`), audit v2 (session surface pinned
    exactly, mutation ban scoped to model/walker).

  Kotlin workstream delivered 2026-07-18 (ES remains), same architecture over
  the wire boundary:

  - **MKC3 wire** replaces MKC2 end to end: every node record carries
    (kind, id, revision, fields) and names its children by id — positions
    never travel with records. A commit payload is
    `removed ids | records for added ∪ changed ∪ bubbled`, with records
    ordered children-before-parents (C-side depth sort; equal depths are
    never ancestor-related), which is exactly the order the mirror rebuilds
    in — one pass, one record shape for all three verdicts (the planned
    bubbled-only `(id, childIds)` shape was dropped: bubbled nodes are
    containers with tiny fields, and one shape removes a decoder branch).
    Scopes travel as a separate `(id, revision, scope)` table payload that a
    snapshot requests once on first scope use while current
    (`ScopeResolver`, atomic pending → materialized/detached state), and the
    revision in each entry rejects stale node values instead of pairing old
    fields with a current position.
  - The bridge gains `markdown_core_kotlin_session_{open,free,lineage,
    revision,length,root,edit,commit,scopes,footnote_info,footnotes,
    footnote_references}`; JNI mirrors each (exports/map/def lists updated);
    Kotlin/Native binds them through the existing cinterop def. One-shot
    `markdown_core_kotlin_parse` is reimplemented as C-side session sugar —
    open + edit + delta-bearing commit + free in a single crossing — whose
    payload is the same record stream a session commit produces (the first
    commit lists every node as added) plus lineage, root id, and the eagerly
    materialized scope table: one decode pipeline for both modes. An empty
    source commits an empty delta, so the decoder synthesizes the revision-0
    empty root from the scope table.
  - Platform surface: all 28 model classes gain `id: MarkupID`/`revision`
    with O(1) equals/hashCode, drop stored scopes, and rename the footnote
    field to `label`; `Document.scope(node)`/`Document.dump()` mediate
    scopes and dumps; `Walker.walk(document, ...)` supplies the resolved
    scope per event. `MarkupSession` is `AutoCloseable` (explicit `close`;
    snapshots and materialized scopes survive it), a pure-shift commit
    re-wraps the root `Document` object around a fresh resolver (Kotlin
    classes cannot carry two snapshots' positions the way Swift's copied
    structs can), and `updates(input: Flow<String>): Flow<Commit>` lands the
    coroutines dependency (`kotlinx-coroutines-core` as `api`).
  - Gates: `SessionTest` (streaming id stability, clean-boundary inserts,
    kind changes, empty-delta pure shifts, scope survival after close,
    superseded-unmaterialized traps, footnote queries, `updates`, closed
    sessions, rejected edits), `SessionAstTest` conformance replay of the
    manifest corpus (per-commit dump equality against one-shot references +
    delta-mirror integrity: disjoint arrays, untouched revisions intact,
    removed ids gone) on JVM, native, and Android-host conformance runs,
    `SessionConcurrencyTest` (parallel sessions with disagreeing options on
    `Dispatchers.Default` — JVM executors, native workers, Android host) plus
    a raw-thread executor variant in `ConcurrencyJvmTest`, the
    `session_stream_flat` JVM streaming benchmark
    (`jni_session_stream_and_delta_decode`), and audit v2 (Kotlin session
    surface pinned exactly, mutation ban scoped away from the session
    directory, document-mediated dump pins).

  ES workstream delivered 2026-07-18, completing M4: the same
  mirror-by-delta architecture, but over pointer-walking bridge accessors
  instead of a serialized wire — a WASM export call is a cheap in-process
  crossing, so the record framing that exists to amortize JNI would only
  add a second decoder:

  - The bridge stays per-field: `bridge.c` gains `es_session_{open,free,
    edit,commit,document,revision,lineage,length,footnote_info,footnotes,
    footnote_references}`, `es_delta_{revision,ids,free}`, and
    `es_node_{id,revision}`; `es_document_parse/free` are deleted — the
    one-shot parse is TS-side session sugar (open, one edit, one
    delta-free commit through the C nullable out-parameter, decode, eager
    scope materialization, free) sharing the session decode pipeline end
    to end.
  - Mirror-by-delta without records: a commit reads the four delta id
    arrays straight from WASM memory, then rebuilds top-down from the
    committed root — a node in `added ∪ changed ∪ bubbled` re-decodes and
    recurses, everything else returns the previous snapshot's object from
    the id → node mirror (children-before-parents ordering falls out of
    the recursion; no depth sort). A pure-shift empty delta re-wraps the
    root `Document` around a fresh resolver, exactly the Kotlin path, and
    the detach-before-native-commit / reattach-on-transactional-failure
    order from the Swift race fix (#22) is ported verbatim.
  - Identity: `MarkupID { lineage: bigint, rawValue: number }`, interned
    per session so the same identity is always the same object — ids work
    as `Map`/React keys and delta arrays answer `includes` by reference;
    ids and revisions cross the boundary as bigint and convert under a
    safe-integer guard. Node values stay plain objects, gain `id`/
    `revision`, drop stored scopes, and rename the footnote field to
    `label`; `document.scope(node)` and `document.dump()` are
    non-enumerable mediators wired at snapshot adoption;
    `TreeDumper.dump(document, node)` rebases subtree origins; Walker
    events carry the resolved scope. The lazy `ScopeResolver` is the
    Kotlin state machine minus the atomics (JS contexts are
    single-threaded).
  - Engine-facing fix found by the ES id tests: the session lineage
    entropy (session address ⊕ `time(NULL)` ⊕ `clock()`) survives the WASM
    libc only at seconds granularity, so back-to-back one-shot parses
    reusing a freed session's address within one wall-clock second minted
    identical lineages. The WASI `clock_time_get` shim now advances its
    reported time a full second per call — entropy is the engine's only
    time consumer, so the drift is unobservable.
  - Gates: `sessions` node suite (streaming id stability, clean-boundary
    inserts, kind changes, empty-delta pure shifts, scope survival after
    close, superseded-unmaterialized traps, footnote queries, `updates`
    over sync and async iterables, closed sessions, rejected edits,
    interleaved sessions per context, per-worker session replay on
    isolated WASM instances), conformance session replay of the manifest
    corpus (per-commit dump equality against one-shot references +
    delta-mirror integrity), a browser streamed-session check, the packed
    npm consumer's session smoke, the
    `wasm_session_stream_and_delta_decode` streaming benchmark, and audit
    v2 (the ES runtime export list gains `MarkupSession`, the session
    surface pinned exactly, scope-free node values and the
    document-mediated dump pinned).
- **M5 — Hardening & release**: edit-script fuzz target, pathological corpus,
  perf tuning, CHANGELOG/VERSION 2.0.0, release dry-run. Gates: full CI
  matrix + package audits.

  Edit-script fuzz target delivered 2026-07-18: the equivalence runner's
  replay machinery (shadow text, delta mirror, verified commit, footnote
  query equivalence) moved to a shared harness
  (`tests/support/session_replay.{c,h}`) with a report callback, and gained
  a deterministic byte-script interpreter — two option bytes, then
  insert/delete/replace/commit operations whose positions are taken modulo
  the shadow length, so every input decodes to a valid edit sequence and
  every byte is fuzzer-meaningful. Three drivers share it: the equivalence
  runner (unchanged cases), the new `fuzz_session_edits` libFuzzer target
  (`MARKDOWN_CORE_FUZZ_SESSION`; verification failure aborts, crash inputs
  replay via `fuzz_smoke_runner --script`), and a seeded deterministic
  smoke (`fuzz_script_smoke`, 512 scripts per run, alternating token-biased
  and uniform splice payloads) in the CI fuzz label. Sanity campaign at
  delivery: 1.1M executions / 2 minutes under ASan, 10,646 edges covered,
  zero failures; correctness + ASan/UBSan presets green.

  Resync locality for footnote definitions delivered 2026-07-19 (issue
  #26), closing out the #15 resync-delay behaviors. A definition legitimately
  stays open across blank lines, which used to strip restart anchors from
  every cluster follower and ride restarts through whole clusters. Both
  halves now apply the same argument: a non-blank line whose first non-space
  sits below the continuation indent closes a definitions-only open chain on
  that very line, before anything above can capture it (no open paragraph is
  left for a lazy continuation to ride).

  - Flag side: when `check_open_blocks` stops at the document and the chain
    open at line start consisted solely of footnote definitions, the line is
    promoted to clean; its direct document children carry `CLEAN_START`
    qualified by `CLEAN_START_SEALING`, recording that the anchor holds only
    while the line keeps its sealing shape.
  - Restart side: planning re-checks a sealing anchor's line against the
    current text (blank or indented means reshaped into a continuation) and
    backs off one clean entry, mirroring the CRLF-fusion guard; sentinels
    take the same check since they do not record their sealing quality.
  - Resync side: the staged probe accepts a boundary while only footnote
    definitions are open, provided the upcoming line seals them; the splice
    finalizes them with ends dated to the line before the boundary, exactly
    as the one-shot parse dates them.
  - Contiguous definition clusters (no blank between definitions) keep an
    open trailing paragraph in the chain and stay genuinely fused — a lazy
    continuation could ride them — as does an indented body continuation
    after a blank.

  Gates: the `footnote_defs` boundary script (body edits at cluster
  head/interior/tail, the colon kind-flip both ways, a definition arriving
  and leaving mid-cluster, the indented continuation negative case, and the
  sealing line itself reshaped indented and back), the
  `restart_locality_counters` cluster layouts (every footnote-cluster body
  edit an incremental resynced commit; a quote-cluster front edit resyncs at
  the second quote), the `session_quote_suffix` complexity case (front edits
  flat across a 1024x quote-suffix spread, 0.9x measured), and the
  `session_footnote_defs` complexity case (last-definition body edits flat
  across a 1024x cluster spread, 0.97x measured — see the footnote-index
  rework below, which the flat form of this gate depends on). Fuzz campaign
  at delivery: 688K executions / 2.5 minutes of the session edit-script
  target, zero failures; correctness + ASan/UBSan presets green.

  Footnote index made damage-proportional, delivered 2026-07-19 with the
  same workstream. Gating #26 exposed that the #14-era refresh — an
  O(#sites) merge copy, a full rebuild that case-folded every label, and an
  O(#sites) diff — ran against every footnote-enabled commit, so even
  site-free edits against a 65K-definition corpus cost ~7.3ms and no
  growing-cluster gate could be wall-clock flat. The index is now
  maintained across commits:

  - **Session-persistent label interning** (`footnote_labels`): one owned
    normalized string per distinct label, folded once when a site first
    carries it; sites store label slots, and no commit path re-normalizes
    at rest (the profile's dominant cost — `utf8proc` folding plus
    `key_index` churn per site per commit — is gone).
  - **Sequence-preserving fast path**: the stale site ranges are two
    O(log #sites) probes over the persistent document-ordered lists
    (anchor lines, old coordinates; graveyard anchors keep theirs); when
    the stale and staged runs — and every rebuilt unit's run — carry
    identical label sequences with stable definition ids, the commit
    patches pointers and churned reference ids in place. Churned ids
    tombstone first and reinsert after (adoption can swap ids between
    positions), repointing their group entries through the site's stored
    group position. No aggregate can have moved, so no delta and no
    rebuild: O(staged + stale + rebuilt) per commit.
  - **Slow path**: any other commit merges the site lists in document
    order (the #14 merge, now post-adoption and clone-anchor tolerant) and
    rebuilds the derived structures from label-slot integers, diffing per
    node id against a hash-keyed record table (sorted array replaced;
    tombstoned open addressing, growth only through a fallible reserve so
    applies never allocate).
  - The refresh runs post-adoption as before; a fallback now clears the
    delta's id arrays so the full path re-records cleanly, and the
    label table survives every path — full rebuilds, failed commits, and
    index swaps included.

  Measured at 65K definitions: site-free edits 7.3ms → 1.3µs/commit,
  last-definition body edits 7.7ms → 1.6µs (both flat from 256 to 65K
  definitions); `session_footnote_shift` (the structural label-flip slow
  path over a fixed cluster) stays flat at 0.96x. Structure-changing
  commits pay the slow path's O(#sites) integer pass — comparable to the
  text memmove the edit itself already costs — and their deltas are
  output-sensitive either way. Gates: the flat `session_footnote_defs`
  above, every footnote boundary script and the per-commit footnote query
  equivalence in the replay harness, the OOM sweep (which caught a
  zero-capacity label-table probe on the injected-failure path), and a
  470K-execution fuzz campaign; correctness + ASan/UBSan presets green.

  Session pathological corpus delivered 2026-07-19: nine `session_*` cases
  in `pathological_runner` replay adversarial structures through the shared
  harness, so every commit carries the full verification set (dump equality
  against a one-shot parse, delta accounting, footnote-query equivalence)
  and the per-case CTest TIMEOUT bounds any commit whose cost degenerates
  against the structure.  The corpus attacks each layer of the incremental
  machinery where its worst case lives:

  - **Inline floods** — `session_emph_openers` (a 48 KiB single paragraph
    of `_a ` openers with a toggling mid-paragraph closer) and
    `session_backtick_runs` (backtick runs of every length below 1200 with
    a parity-flipping front backtick) force whole-paragraph inline
    reparses; the delimiter and code-span machinery must not leak state
    between commits.
  - **Deep chains** — `session_quotes_deep` (1024 open quotes; innermost
    text edits plus a level-64 quote-to-list marker flip re-kinding the 959
    levels below) and `session_list_spine` (512-deep list nesting; a
    mid-spine dedent re-parenting every deeper level).  Session trees pass
    through the recursive dump on every commit, so these depths deliberately
    stay below the one-shot cases'.
  - **Whole-document churn** — `session_reference_collisions` (2048
    colliding-label definitions all referencing one shared key whose
    definition toggles, resolving and collapsing 2047 links per commit
    against a collision-saturated reference map) and `session_fence_gate`
    (4096 paragraphs behind a toggling unclosed fence at the head,
    re-kinding the entire suffix through adoption, the graveyard, and the
    delta stream twice per round).
  - **Restart hostility** — `session_lazy_wall` (one quoted paragraph
    continued by 4096 lazy lines: no clean anchor exists, so tail edits
    ride the fallback; a mid-wall blank evicts the tail from the quote and
    its deletion knits the wall back) and `session_crlf_seam` (4096
    CRLF-separated paragraphs edited exactly at `\r\n` seams — deleting a
    middle `\r`, splicing a byte between `\r` and `\n` on text and blank
    lines — against the restart planner's line-shape re-checks).
  - **Footnote interning** — `session_footnote_labels` (1024 definitions
    whose labels need case folding, 256 references; a site-free edit that
    must not refold, and definition/reference label flips churning the
    interning table and cascading ordinals).

  The list-flip case pinned an engine fact worth recording: list markers
  only open below `MAX_LIST_DEPTH` (100), so a bullet at quote depth 511
  stays literal text — the flip sits at level 64.  Gates: all nine cases
  registered per-case as `pathological_session_*` (30s timeouts, audit
  cross-checked against `pathological_runner --list`), worst sanitizer
  wall-clock 2.4s; correctness (92) + ASan/UBSan/TSan suites (77 each) and
  the test-topology audit green.  Remaining in M5: perf tuning, release
  prep.

  Perf tuning delivered 2026-07-19, scoped by a sampling profile of the
  commit path (storm, bounded-stream, retarget, and footnote-defs
  workloads over the complexity corpora): the two dominant non-parsing
  costs were per-commit resource churn, both removed without touching the
  parse or adoption algorithms.

  - **Field equality without serialization** (`ast.c`): adoption compared
    paired nodes by JSON-dumping both field sets into heap buffers and
    memcmp-ing them — two allocations plus full escaping per surviving
    node pair (~10% of a storm commit, ~11% of a retarget commit).
    `markdown_core_ast_fields_equal` now compares fields directly per
    kind through the same accessors the dump uses; optional strings keep
    the dump's null/empty distinction, and the no-allocation path also
    retires the "allocation failure forces a revision bump" caveat.
  - **Warm parser** (`blocks.c` + session): every commit built and
    tore down a full parser — struct calloc, line buffers, an empty
    reference map the incremental path never reads (it swaps the
    session's map in), extension attachments, special-character
    re-derivation (6% of a storm commit, 12% of a bounded-stream
    commit).  A session now keeps one parser warm between commits
    (`session->warm_parser`): `markdown_core_parser_renew` (core)
    resets a parser whose parse ended while keeping the line buffers'
    capacity, its own (empty) reference map when the caller left one
    attached, and the extension attachments.  Commit paths acquire/release
    through the session; a poisoned parser (allocation failure during the
    parse or the renewal) is freed instead of held, so the session never
    holds a broken parser, and the full path — which consumes the
    parser's map as the session's next refmap — hands the parser back
    with `refmap == NULL` and the renewal replaces it.  A fallback
    releases the staged parser before the full path acquires, so even
    degraded commits reuse the shell.

  Measured on the profile driver (Release, M-series): storm 3.47→2.58
  µs/commit, bounded stream 1.11→0.82, retarget 9.68→5.85, footnote-defs
  0.98→0.75; complexity-runner session gates all improved and stay flat
  across the 1024x size spread.  The remaining hotspots — id-table
  maintenance (~11%), subtree frees (~9%), and general allocator churn —
  are arena-shaped; resolving them was pulled into M5 ahead of the
  release (decision 2026-07-19), as the final perf workstream before
  release prep.

  The arena workstream delivered 2026-07-19 in three parts:

  - **Self-referential allocators**: every `markdown_core_mem` function
    now receives the allocator itself (container-of pattern), so
    stateful allocators recover their state from the embedded struct
    with no global.  Mechanical sweep of ~180 call sites; the default
    allocator and injection harnesses ignore the argument.
  - **Session arena** (`extensions/arena.c`): a pooled session routes
    every allocation it owns — nodes, content buffers, tables, staged
    parsers — through a size-classed slab arena behind the
    `markdown_core_mem` face.  Freed blocks go to per-class freelists
    and later commits reuse them; growth within a block's class
    capacity is a realloc no-op (absorbing content-buffer growth);
    requests above the largest class pass through to the base allocator
    on a tracked list; teardown is one wholesale release.  Two
    boundaries are deliberate: the one-shot parse stays unpooled (its
    detached tree outlives the session, and `Document.parse` keeps its
    v1 memory profile per the contract), and ASan builds bypass pooling
    so the sanitizer keeps seeing individual allocations.  A
    growth-driven passthrough block skips calloc zeroing — the first
    arena cut re-zeroed the adoption stack on every commit and showed
    up as a 15% retarget regression before the fix.  Gate added:
    `session_oom_sweep_pooled` drives the sweep script through an arena
    over the injected allocator, so every base refill (slab,
    passthrough, the arena itself) fails in turn.
  - **Id table**: id and node now share one slot struct (one cache line
    per probe, one allocation per table).  An attempted
    remove-stale-before-put reorder — dropping the ownership probe in
    `ids_remove_stale_chain` — was measured and REVERTED: backward-shift
    deletion costs far more than the read probe it saved (storm
    2.03→2.60 µs/commit), so the put-first order with the ownership
    probe is load-bearing for performance, not just correctness.

  Cumulative profile-driver numbers for M5 perf (Release, M-series,
  µs/commit, baseline → warm parser → arena): storm 3.47→2.58→2.03,
  bounded stream 1.11→0.82→0.68, retarget 9.68→5.85→5.64, footnote-defs
  0.98→0.75→0.55.  What remains at the top of the storm profile is
  parsing itself (block feed + inline refine ~43%) and the adoption
  walk (~22%) — no allocator or table maintenance dominates any
  workload anymore.

  The arena surfaced a latent WASM limit: the ES module was built
  without `ALLOW_MEMORY_GROWTH`, so the heap was fixed at emscripten's
  16 MiB default and emscripten's malloc hangs the module once a parse
  exceeds it (the engine itself fails permanent exhaustion cleanly —
  probed natively with a hard-cutoff allocator at 19 budgets across a
  395 KB pooled commit; every one failed transactionally).  The
  arena's class rounding lowered the threshold into the ES robustness
  suite's range, but any sufficiently large document hit the same wall
  before it.  The module now links with memory growth enabled and the
  runtime supplies `emscripten_notify_memory_growth`; no view
  refreshing is needed because every DataView/Uint8Array over wasm
  memory is created at its use site.  Session memory characteristics
  (high-water retention) are now bounded by the host's memory limits,
  matching the documented session cost model.

## Verification

- **Equivalence gate** (`equivalence_runner.c`, CTest): every canonical
  fixture + CommonMark spec case parsed batch, per-line append replay,
  sampled per-byte replay, and N seeded random edit scripts — final dump
  byte-equal to a one-shot parse of the final text. Plus a
  **delta-mirror check**: maintain an id→(kind,parent,ordinal,rev)
  shadow from deltas only and compare against a fresh walk after every
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
- Adoption bugs → stale binding mirrors: delta-mirror gate + seeded edit
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
  every ordinal shift (large deltas, heavy UI re-renders), leaves
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
