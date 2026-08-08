# Sessions, deltas, and platform surfaces

Status: rewritten 2026-08-03 onto the unified-CST ownership model. This
contract is not implemented yet; it becomes binding together with
`incremental-canonical-ast.md`, and the two ship atomically with
`canonical-ast.md` (that document's rollout gate, section 16).

Companion documents: `incremental-canonical-ast.md` owns the model — one
unified CST, its typed semantic projection, identity, revisions, extents, the
delta, and every complexity bound. `canonical-ast.md` owns the node inventory.
`canonical-ast-dump.md` owns the diagnostic dump grammar.

**This document owns two things and defers everything else:** the parser
answer record types, and the concrete platform and C surfaces that expose the
model. Where it restates a rule from `incremental-canonical-ast.md` it does so
to name the API that carries it, never to redefine it; a conflict is resolved
in that document's favour.

Platform APIs may use idiomatic syntax, but they must not change names,
semantics, ordering, identity rules, or cost guarantees.

## Model

A **session** is the single mutable owner of one Markdown text. Consumers
apply **edits** to byte ranges, then **commit**. Each commit reparses only the
ownership regions the edit touched and publishes a `Commit`:

```text
Commit {
    Document document
    Delta    delta
}
```

`Commit.document` is the complete correctness result and is immediately
self-contained: it stays readable after later commits and after the session
closes, with no materialization step. `Commit.delta` describes what changed
between the session's previously published document and this one. A consumer
may discard every delta and re-derive from `Commit.document`; the three
integration paths are `incremental-canonical-ast.md` §2.1, and a binding must
support all three.

`Document.parse(source, options)` remains the one-shot entry point. It is a
pure function of `(bytes, options)` and returns the same `Document` type. It
gets its own `DocumentDomain`, so its identities never compare equal to
another parse's.

There is no process-global state in the parse path. Any number of sessions may
advance concurrently with no shared lock and no cross-session interference.

## Equivalence invariant

After any sequence of edits and commits, the session's document is
semantically identical to a from-scratch `Document.parse` of the same final
bytes: the canonical diagnostic dump of both is byte-equal. Identities and
revisions are excluded — they are document history, not content.

## Edits

- `edit(byteStart, byteEnd, replacement)` replaces `[byteStart, byteEnd)` of
  the session's current pending bytes. `byteStart == byteEnd` inserts; an empty
  replacement deletes; `append` is sugar for an edit at `[length, length)`.
- Offsets are **stored bytes**, not a resolved `Scope`. The two are separate
  types so a projected coordinate cannot be fed back in as an edit
  (`incremental-canonical-ast.md` §8.1). Byte granularity is what lets a
  streamed append complete a multi-byte character whose first bytes arrived in
  an earlier edit.
- **The store never normalizes its contents and never rejects them.** It holds
  any byte sequence, byte for byte, and hands the same bytes back; UTF-8 is
  assumed and never validated (`incremental-canonical-ast.md` §7.1). A
  truncated final code point at a streamed chunk boundary therefore needs no
  exception to be legal — it is simply bytes. NUL is replaced with U+FFFD
  during parsing because CommonMark requires it of canonical text, and nothing
  else is.
- Edits are cheap: they update the pending text and extend the pending edit
  script. No parsing happens until commit. Multiple edits may be queued per
  commit, and they normalize to one deterministic non-overlapping ascending
  script before parsing. Overlap, overflow, or a stale base fails the commit
  without publishing a partial source or AST.
- `commit(source)` is an optional convenience for a host that owns whole text
  rather than the edit — a two-way text binding, a reload from disk, a remote
  replacement. It normalizes the difference into the same script and must
  produce an identical document, identities, and delta. It costs one byte-level
  diff, so a host that already knows its edit submits that edit instead.

Typing, paste, IME replacement, collaboration chunks, and LLM output all use
this one path. There is no streaming-only node, provisional AST, finalize
opcode, or parser mode.

## Commit and delta

```text
Delta {
    DocumentVersion before
    DocumentVersion after
    [Diff]          diffs      // how the nodes differ
    [SourceEdit]    edits      // how the bytes differ
}

Diff {
    MarkupID  markup
    DiffParts parts
}

DiffPart = VALUE | TEXT | CHILDREN | ANSWERS | DESCENDANT
```

`diffs` is one postorder list and is the only form: retired entries first in
`before` postorder, then live entries in `after` postorder, each `MarkupID`
appearing exactly once carrying the union of its differing parts. There is no
second ordered-entry API, no lifecycle tag, no parent member, no position
member, and no per-field address; each is derivable from the document in
`O(1)` or `O(log n)`. The membership law, the meaning of each part, and the
`|diffs|` bound are `incremental-canonical-ast.md` §9.

`Delta` is plain immutable caller-owned data. It retains no session, remains
valid after the session advances or closes, and is not an AST mutation
protocol: an entry names an address, and the consumer re-reads the current
document there. Concatenating adjacent deltas is sound and idempotent, so
there is no compose operation, acknowledgement, nonce, or fallback-reason
enumeration. A stale or mistrusted delta is detected by the single `before`
comparison and resolved by rebuilding from the current document.

## Parser answers

Numbering, first-use order, resolution, and back-reference ordinals are not
node fields. They are relations over the whole document, so they are queries
on the semantic document, addressed by the `MarkupID` whose answer they are:

```text
Document.footnote(MarkupID)   -> Optional<FootnoteAnswer>
Document.footnotes()          -> [MarkupID]
Document.resolution(MarkupID) -> Optional<Resolution>
Document.references(MarkupID) -> [MarkupID]
```

They are members of the document and not of the session, because a retained
document must answer them after later commits and after the session closes.
The query resolves through that document's own immutable relation indexes; it
never consults a newer session revision.

### Answer records

```text
FootnoteAnswer {
    MarkupID winner          // the winning FootnoteDefinition for this label
    integer  number          // the winner's 1-based number in first-use order
                             // of the label; 0 when the LABEL has no reference
    integer  ordinal         // 1-based among that label's references in
                             // document order; 0 when asked of a definition
    integer  referenceCount  // references resolving to `winner`; a property of
                             // the label, so a shadowed definition reports it
}

Resolution {
    MarkupID winner          // the winning ReferenceDefinition for this label
    integer  number          // the winner's 1-based number in first-use order
                             // of the label; 0 when the LABEL has no reference
    integer  ordinal         // 1-based among that label's references in
                             // document order; 0 when asked of a definition
    integer  referenceCount  // references resolving to `winner`; a property of
                             // the label, so a shadowed definition reports it
}
```

The two records have the same shape because the two namespaces answer the same
questions; they stay separate types because `[x]` and `[^x]` are separate
label spaces that must never share a bucket (`canonical-ast.md`, reference and
footnote semantics).

- Asked of a `FootnoteReference`, `LinkReference`, or `ImageReference`, the
  answer is always present: those nodes exist only where their label is
  defined, so there is no unresolved case to encode.
- Asked of a `FootnoteDefinition` or `ReferenceDefinition`, `winner` is the
  definition that wins the label — its own identity unless an earlier
  definition shadows it — and `ordinal` is 0, because a definition is not one
  of its own references.

  **`number` and `referenceCount` describe the label, not the node asked.**
  They are the winner's number in first-use order and the count of references
  resolving to that winner, so a shadowed definition reports the same pair its
  winner does. Both are 0 when **the label** has no references at all:
  numbering is first-use order, so a label with no first use has no number,
  and 0 is the sentinel for that rather than an absent field. Reading them as
  properties of the queried node instead would make a shadowed definition
  report 0 beside a non-zero `winner`, which is the one combination the record
  cannot mean.

  `Document.references(identity)` is the accessor that *is* node-scoped: it
  returns the references resolving to a winning definition in document order,
  and is empty for a shadowed definition, for a definition whose label nothing
  refers to, and for a non-definition identity. So a shadowed definition
  answers a non-zero `referenceCount` and an empty `references` list, and the
  two are consistent because they are answering different questions.

  `Document.footnotes()` returns the referenced winning definitions in
  first-use order — the order a renderer lists them in — which is exactly the
  set whose `number` is non-zero.

  Those zeros are real immutable values, not "nothing was read", so a later
  definition insertion or a first reference appearing is discovered as an
  ordinary `ANSWERS` change rather than by rescanning.
- Asked of any other kind, the result is absent.

Labels match case-folded with collapsed whitespace, the earliest definition in
document order wins, and reference labels longer than the link-label limit
(1000 bytes) never resolve — so they never produce a reference node at all.

A change to any of these answers surfaces as the `ANSWERS` part on exactly the
identities whose observable result changed, and on the root `Document` for a
change to `footnotes()`. Filling, compacting, or rebuilding an index emits
nothing.

## Coordinates

Nodes store no absolute position. A node's `track.extent` is a stable
identity, and coordinates are resolved against the owning document:

```text
Document.scope(SourceExtent, CoordinateProfile) -> Optional<Scope>
```

A commit that only shifts later content publishes an empty `diffs`, and a
consumer that resolves on demand pays nothing for the shift. A consumer that has chosen to materialize
absolute coordinates remaps them from `Delta.edits`; there is no coordinate
event, remap channel, or scope table.

`CoordinateProfile` selects the space: `STORED_BYTE`, `UNICODE_SCALAR`,
`UTF16`, `LINE_COLUMN`, or a binding's closed `NATIVE` projection.
`LINE_COLUMN` is what the dump grammar prints.

## Concrete surface

The unified CST is reachable from any document, one-shot or committed:

```text
Document.concrete -> ConcreteTree
```

It exposes concrete nodes and tokens in source order, delimiters and trivia,
`MissingToken`, `UnexpectedToken`, and `ErrorRegion`, child traversal, and
source mapping. It is the retained owner, not a second tree or a
reconstruction, so reaching it allocates nothing and advances no trace.
`ConcreteID` never appears in a semantic value, a parser answer, or a `Delta`,
and no answer is ever addressed to a concrete record.

An adapter building Semantic IR or a frame tree consumes the semantic
projection and parser answers only; requiring concrete access is outside the
supported boundary.

## Failure and memory

- Commits are transactional. On allocation failure the session remains valid
  at the previous committed revision, the error is reported, and `commit()`
  may be retried from the still-current committed source. An aborted identity
  or revision is burned, never reused.
- Pending edits already applied to the pending text are retained: text
  advances, the tree does not. This is observable and documented.
- Closing a session invalidates no published document, node view, byte slice,
  or delta.
- Routine private storage compaction preserves every public identity,
  revision, and AST value and emits no diff entry.

## Concurrency

- All mutating calls on one session are externally synchronized (one writer).
- Published documents and deltas are immutable values, safe for concurrent
  reads from any thread according to the binding's value contract, including
  while the session advances.
- Distinct sessions are fully concurrent, which is what lets a workspace hold
  one session per open document.

## Platform surfaces

The canonical entry points on Swift, Kotlin, and ES are `Document.parse` and
`MarkupSession`:

| Operation | Contract |
| --- | --- |
| `MarkupSession(options)` | options are immutable for the session lifetime |
| `replace` / `append` | queue edits as byte ranges of the pending text |
| `commit()` | returns `Commit { document, delta }` |
| `commit(source)` | optional whole-buffer convenience over the same primitive |
| `document` / `version` | the last committed document and its `DocumentVersion` |
| `node(for:)` / `parent(of:)` / `index(of:)` | resolve an identity in that document |
| `scope(of:profile:)` | resolve a stable extent to a `Scope` |
| `footnote(of:)` / `footnotes()` / `resolution(of:)` / `references(of:)` | the parser answers above |
| `concrete` | the retained `ConcreteTree` |

Every one of those accessors is a member of the **document**, not of the
session, except the session's own `commit`, `edit`, and `document`. There is
no empty document at revision zero: a session publishes its first document at
its first commit, and every public identity and revision is positive.

Shared platform types, named identically on all three platforms:

- `MarkupID` — `(DocumentDomain, ordinal)`. Hashable, equatable, and
  serializable, usable unmodified as a SwiftUI `ForEach(id:)`, a Compose
  `key()`, or a React `key`.
- `MarkupTrack` — `{ identity, revision, extent }`; `MarkupRevision` is the
  `{ self, subtree }` pair. Node equality and hashing are the two-word tuples
  of `canonical-ast.md`.
- `Commit` — `{ document, delta }`.
- `Delta` — `{ before, after, diffs, edits }`. Always present on a platform
  `Commit`; the C-level nullable out-parameter is a C-consumer knob only.
- `Diff` / `DiffParts` — one identity plus the six-flag bitmask. No registry,
  subscription, interest, route, target, or acknowledgement type may be
  layered around them.
- `FootnoteAnswer` / `Resolution` — the records above; absent answers are
  platform-optional (`nil`/`null`) rather than 0.

The footnote label field on `FootnoteDefinition` and `FootnoteReference` is
named `label`, because `track.identity` names node identity; the diagnostic
dump grammar keeps its frozen `id=` key for that label.

### C facade

```text
markdown_core_session_open / _edit / _commit / _close
markdown_core_commit_document / _delta
markdown_core_document_node / _parent / _index / _scope / _version / _concrete
markdown_core_document_footnote / _footnotes / _resolution / _references
markdown_core_delta_before / _after / _diffs / _edits / _free
```

Answer and diff results are caller-owned values that stay valid after the
session or document is released; there are no mutation-bounded borrowed
arrays. The superseded `markdown_core_session_ordered_delta_entries`,
`markdown_core_session_footnote_*`, and `markdown_core_document_scope_table`
are removed rather than deprecated, and a package cannot advertise this
capability while exposing them.

`ParseOptions` and the exhaustive `MarkupVisitor` dispatch contract are
unchanged from `canonical-ast.md`. `MarkupWalker` keeps its two traversal
modes: the typed-visitor overload walks structure in preorder without
resolving scope, and the event overload emits entering/exiting events with the
resolved scope. Neither visits a syntax-only token; concrete traversal is the
`concrete` surface's own.
