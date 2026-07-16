# Sessions and changesets contract

Status: draft for v2.0 (milestone M0, 2026-07-15). This contract is not
implemented yet; it becomes binding when the v2 milestones land. Companion
documents: `canonical-ast.md` (node inventory), `canonical-ast-dump.md`
(diagnostic dump grammar), and
`../migration/2026-07-15-v2-incremental-sessions-plan.md` (design rationale
and delivery plan).

This is the language-neutral public contract for incremental parsing. Platform
APIs may use idiomatic syntax, but they must not change names, semantics,
ordering, identity rules, or cost guarantees defined here.

## Model

A **session** is the single mutable owner of one Markdown text and its living
AST. Consumers apply **edits** (insert, replace, delete on byte ranges;
appending a streamed token is an edit at end-of-text), then **commit**. Each
commit incrementally reparses only the damaged region and produces:

- a **snapshot**: an immutable `Document` value tree that structurally shares
  every unchanged node with the previous snapshot, and
- an optional **changeset**: the exact set of node ids that were added,
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

- Nodes outside the damaged region of an edit keep their id and revision.
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
- Edits are cheap: they update the text store and queue damage. No parsing
  happens until commit. Multiple edits may be queued per commit; coalescing
  token appends into fewer commits is the recommended way to trade latency
  for throughput.

## Commit and changesets

`commit()` reparses incrementally and returns the new snapshot plus, on
request, a changeset containing four id arrays:

| Array | Meaning |
| --- | --- |
| `added` | nodes that did not exist at the previous revision |
| `removed` | nodes that no longer exist (ids are retired, never reused) |
| `changed` | nodes whose own fields or direct child list changed |
| `bubbled` | ancestors whose revision changed only because a descendant did |

The four arrays are disjoint. Applying a changeset to a mirror of the
previous revision (materialize `added` and `changed`, relink `bubbled`, evict
`removed`) yields exactly the new tree; this is the mechanism bindings use to
build snapshots in O(changeset) rather than O(document).

Changesets are plain caller-owned data and remain valid after the session
advances or is freed.

## Cost model

- Per-commit work is proportional to the size of the touched leaf blocks plus
  the changeset, independent of total document size. Streaming appends touch
  only the open frontier.
- Inline content is reparsed per touched leaf block (inline syntax is
  non-local within a leaf). Streaming into one enormous paragraph is
  therefore linear per commit in that paragraph's size.
- Documents parsed mid-stream behave as if the input ended at the current
  text: unterminated constructs parse exactly as `Document.parse` would parse
  them (for example `CodeBlock.closed == false`).
- Non-local Markdown constructs degrade gracefully, never worse than one full
  parse per commit: an edit inside an unclosed fence, raw-HTML block, or
  directive reparses forward to end of input; a link-reference definition
  change re-parses inline content only in the blocks that referenced that
  label; footnote renumbering touches only the references whose index
  changed.

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
  any snapshot are safe for concurrent reads from any thread.
- Distinct sessions are fully concurrent.
- One-shot documents keep the v1 concurrency contract verbatim.

## Platform surfaces

The canonical entry points on Swift, Kotlin, and ES are `Document.parse`
(unchanged shape) and `MarkupSession`:

| Operation | Contract |
| --- | --- |
| `MarkupSession(options)` | options are immutable for the session lifetime |
| `edit` / `append` | queue edits as defined above |
| `commit()` | returns `(document, changes)` |
| `updates(feeding:)` | async sugar: feed a token stream, yield per-commit `(document, changes)` |
| `document` / `revision` | last committed snapshot and its revision |

The C facade exposes the same model as
`markdown_core_session_open/edit/commit/document/node_by_id/free`,
`markdown_core_node_get_id/get_revision/get_parent`, and
`markdown_core_changeset_*` accessors; node handles borrowed from a session
are valid until the next mutating call on that session.

`ParseOptions` is unchanged from `canonical-ast.md`. Visitor and Walker
contracts are unchanged; walker events additionally carry the resolved scope.
