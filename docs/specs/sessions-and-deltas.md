# Sessions and deltas contract

Status: draft for v2.0 (milestone M0, 2026-07-15; footnote queries added
with the revised footnote contract, M3, 2026-07-16; platform surface
concretized with the M4 Swift binding, 2026-07-17 — the per-commit id-set
record is named **delta** everywhere: `markdown_core_delta_*` in C, `Delta`
on the platforms). This contract is not fully implemented yet; it becomes
binding when the v2 milestones land.
Companion documents: `canonical-ast.md` (node inventory),
`canonical-ast-dump.md` (diagnostic dump grammar), and
`../migration/2026-07-15-v2-incremental-sessions-plan.md` (design rationale
and delivery plan).

This is the language-neutral public contract for incremental parsing. Platform
APIs may use idiomatic syntax, but they must not change names, semantics,
ordering, identity rules, or cost guarantees defined here.

## Model

A **session** is the single mutable owner of one Markdown text and its living
AST. Consumers apply **edits** (insert, replace, delete on byte ranges;
appending a streamed token is an edit at end-of-text), then **commit**. Each
commit incrementally reparses only the stale region around the edits and
produces:

- a **snapshot**: an immutable `Document` value tree that structurally shares
  every unchanged node with the previous snapshot, and
- an optional **delta**: the exact set of node ids that were added,
  removed, or changed by the commit.

`Document.parse(source, options)` remains the one-shot entry point. It is a
pure function implemented as an internal single-commit session and keeps its
v1 memory profile (no session survives the call).

There is no process-global state anywhere in the parse path. Any number of
sessions may be created and used concurrently with no global locks and no
cross-session interference.

## Equivalence invariant

After any sequence of edits and commits, the session's document is
semantically identical to a from-scratch `Document.parse` of the same final
text: the canonical diagnostic dump of both trees is byte-equal. Node ids and
revisions are excluded from this invariant (they are session history, not
content).

## Node identity

Every node has:

- `id` — unique within its session, assigned when the node is created, never
  reused. Ids are stable **within a node kind**: if an edit changes what kind
  of node occupies a source region (paragraph becomes a heading, a paragraph
  is promoted to a formula block or retyped into a table), the old node is
  reported `removed` and a new node `added`. Ids are never adopted across
  kinds.
- `revision` — the commit revision at which the node's own fields, its child
  list, or any descendant last changed. A pure positional shift caused by an
  edit elsewhere does not change a node's revision.

Sessions also expose a `lineage` value (a per-session random 64-bit salt).
Nodes from different sessions never compare equal.

### Id stability guarantees

- Nodes outside the stale region of an edit keep their id and revision.
- A block whose source bytes are unchanged by an edit keeps its id, its
  revision, and its entire subtree, even when the surrounding structure is
  reparsed.
- During append-only streaming, the open block at the end of the document
  (for example the paragraph currently receiving tokens) keeps its id across
  commits, and its trailing `Text` node keeps its id with a revision bump per
  content change. Earlier inline siblings that are byte-identical keep id and
  revision.
- Everything else is best effort: reparsed regions adopt old ids where kind
  and position match, and report honest `added`/`removed` entries where they
  do not.

## Equality

Platform node equality is `(lineage, id, revision)` — O(1), allocation-free,
and safe in render hot paths. Two equal nodes are guaranteed to have
identical AST content (fields and descendants). Hash values are derived from
the same tuple. Unchanged nodes are additionally the same platform object
across consecutive snapshots (reference identity is a valid fast path).

Absolute source position is not content: two equal nodes may sit at different
absolute positions in different snapshots.

## Scope

Nodes do not store absolute source positions. The public API is:

- `document.scope(of: node)` — resolves the absolute `Scope` (start/end
  line:column) of a node within that snapshot, O(depth).
- `Walker` supplies the resolved scope with every event; `TreeDumper` prints
  absolute scopes for a `Document` root, byte-identical to the v1 dump
  grammar. Dumping a non-`Document` subtree prints scopes with the subtree as
  origin.

One-shot `Document.parse` materializes scopes eagerly so `scope(of:)` behaves
identically in both modes.

Session snapshots resolve scopes lazily: deltas deliberately omit pure
positional shifts (an edit that only moves later content commits an empty
delta), so a snapshot cannot carry positions in its shared node values.
Instead, the first scope use on a snapshot — `scope(of:)`, a `Walker` walk,
or a dump — materializes every scope from the session's native tree in one
walk and caches the table; the snapshot is self-contained from then on,
including after the session advances or is freed. Queueing edits does not
end a snapshot's currency (edits never touch the committed tree); the next
successful commit does. Requesting a scope from a snapshot that was
superseded before it ever materialized is a documented programmer error
(platforms trap), as is passing a node of a different session or revision.

## Edits

- `edit(byteStart, byteEnd, replacement)` replaces the byte range
  `[byteStart, byteEnd)` of the current text with the replacement bytes.
  `byteStart == byteEnd` inserts; an empty replacement deletes; `append` is
  sugar for an edit at `[length, length)`.
- Offsets refer to the session's **stored text**: the raw bytes exactly as
  edited, which is also what `session.length` reports. The store never
  normalizes its contents; NUL and invalid UTF-8 are replaced with U+FFFD
  during parsing, per line, exactly as the one-shot parse does. This is what
  lets a streamed append complete a multi-byte character whose first bytes
  arrived in an earlier edit — the completing commit parses the whole
  character, identical to a batch parse of the same final bytes.
- Edits are cheap: they update the text store and extend the pending stale
  range. No parsing
  happens until commit. Multiple edits may be queued per commit; coalescing
  token appends into fewer commits is the recommended way to trade latency
  for throughput.

## Commit and deltas

`commit()` reparses incrementally and returns the new snapshot plus, on
request, a delta containing four id arrays:

| Array | Meaning |
| --- | --- |
| `added` | nodes that did not exist at the previous revision |
| `removed` | nodes that no longer exist (ids are retired, never reused) |
| `changed` | nodes whose own fields or direct child list changed |
| `bubbled` | ancestors whose revision changed only because a descendant did |

The four arrays are disjoint. Applying a delta to a mirror of the
previous revision (materialize `added` and `changed`, relink `bubbled`, evict
`removed`) yields exactly the new tree; this is the mechanism bindings use to
build snapshots in O(delta) rather than O(document).

Deltas are plain caller-owned data and remain valid after the session
advances or is freed.

## Cost model

- Per-commit work is proportional to the size of the touched leaf blocks plus
  the delta, independent of total document size. Streaming appends touch
  only the open frontier.
- Inline content is reparsed per touched leaf block (inline syntax is
  non-local within a leaf). Streaming into one enormous paragraph is
  therefore linear per commit in that paragraph's size.
- Documents parsed mid-stream behave as if the input ended at the current
  text: unterminated constructs parse exactly as `Document.parse` would parse
  them (for example `CodeBlock.closed == false`).
- Non-local Markdown constructs degrade gracefully, never worse than one full
  parse per commit: an edit inside an unclosed fence, raw-HTML block, or
  directive reparses forward to end of input; an edit that changes the
  document's link-reference definitions (label, destination, or title —
  a reparse that re-harvests byte-identical definitions does not count)
  re-resolves every affected reference within that bound (narrowing this to
  exactly the blocks that referenced the label is a planned refinement); a
  footnote ordinal or resolution change bumps only the revisions of the
  references whose query answers changed (definitions stay at their source
  position; numbering and resolution are index-backed queries, per the
  revised footnote decision of 2026-07-16).

## Footnote queries

The tree is source-faithful (`canonical-ast.md`, footnote semantics):
definitions never move and references always carry their label. Everything
presentational is a query against the session's committed revision:

- `footnote(id)` — for a `FootnoteReference`: the winning definition's id
  (0 while unresolved), the label's 1-based first-use `number` (0 while
  unresolved), the reference's 1-based ordinal among the label's references
  in document order, and how many references share the label. For a
  `FootnoteDefinition`: the label's winning definition id (its own unless an
  earlier definition shadows it), the label's `number` and reference count
  (0 when unreferenced), and ordinal 0.
- `footnotes()` — the referenced winning definitions in first-use order (the
  order a renderer lists them in).
- `references(definition)` — the references resolving to a winning
  definition, in document order (back-reference targets); empty for
  shadowed, unreferenced, or non-definition ids.

The C surface is `markdown_core_session_footnote_info`,
`markdown_core_session_footnotes`, and
`markdown_core_session_footnote_references` over a
`markdown_core_footnote_info` record; borrowed arrays stay valid until the
next mutating call. Labels match case-folded with collapsed whitespace; the
earliest definition in document order wins; reference labels longer than the
link-label limit (1000 bytes) never resolve.

When a commit changes only these answers — an ordinal shift after an earlier
first use appears, a resolution flip after a definition is added or removed —
the affected references and definitions are reported `changed` with a
revision bump and byte-identical dump content, and their untouched ancestors
`bubbled`. One-shot documents do not carry the index; footnote queries are a
session feature.

## Failure and memory

- Commits are transactional under allocation failure: on OOM the session
  remains valid at the previous committed revision, the error is reported,
  and `commit()` may be retried. Text edits already applied to the text store
  are retained (text advances, tree does not) — this is observable and
  documented.
- Session teardown frees everything owned by the session. Snapshots held by
  bindings are self-contained platform values and survive the session.

## Concurrency

- All calls on one session are externally synchronized (one writer at a
  time).
- Between mutating calls, the session's document view, node accessors, and
  any snapshot are safe for concurrent reads from any thread. The borrowed
  C document view stays valid across `markdown_core_session_edit` (edits
  never touch the committed tree) and ends at the next commit or free.
- Distinct sessions are fully concurrent.
- One-shot documents keep the v1 concurrency contract verbatim.

## Platform surfaces

The canonical entry points on Swift, Kotlin, and ES are `Document.parse`
(unchanged shape, now implemented over an internal single-commit session so
one-shot nodes carry ids) and `MarkupSession`:

| Operation | Contract |
| --- | --- |
| `MarkupSession(options)` | options are immutable for the session lifetime |
| `replace` / `append` | queue edits as defined above (byte ranges of the stored text) |
| `commit()` | returns a `Commit` value: the new `document` plus its `delta: Delta` |
| `updates(feeding:)` | async sugar: feed a token stream, yield one `Commit` per token |
| `document` / `revision` | last committed snapshot and its revision; the empty document at revision 0 before the first commit |
| `node(for:)` | the committed snapshot's current value for an id |
| `footnote(of:)` / `footnotes()` / `references(of:)` | the footnote queries below |

Shared platform types, named identically on all three platforms:

- `MarkupID` — node identity: the session's `lineage` salt plus the raw
  64-bit id. `Identifiable`-style APIs use `MarkupID` (revision-free, stable
  across commits); node equality is `MarkupID` plus `revision`.
- `Commit` — `{ document, delta: Delta }`.
- `Delta` — `{ beforeRevision, afterRevision, added, removed, changed,
  bubbled }` as arrays of `MarkupID`. Always present on a platform `Commit`
  (the C-level nullable out-parameter is a C-consumer knob only).
- `FootnoteInfo` — the per-node footnote query record; unresolved and
  not-applicable answers are platform-optional (`nil`/`null`) rather than 0.

The platform footnote field on `FootnoteDefinition`/`FootnoteReference` is
named `label` (the node identity property occupies `id`); the diagnostic
dump grammar keeps its frozen `id=` key for that label.

The C facade exposes the same model as
`markdown_core_session_open/edit/commit/document/node_by_id/free`,
`markdown_core_node_get_id/get_revision/get_parent`, and
`markdown_core_delta_*` accessors; node handles borrowed from a session
are valid until the next mutating call on that session.

`ParseOptions` is unchanged from `canonical-ast.md`. Visitor and Walker
contracts are unchanged; walker events additionally carry the resolved scope.
