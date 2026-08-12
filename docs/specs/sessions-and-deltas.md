# Documents, deltas, and platform surfaces

Status: rewritten 2026-08-09 onto the document model — there is no session; a
document is created from text and options, and `commit` hands it new text and
supersedes it (`../reviews/2026-08-09-api-model-and-allocation-failure.md`).
Rewritten 2026-08-03 onto the unified-CST ownership model. This
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

A **document** is created from text and options, and committing hands it new
text:

```text
Document(markdown, options)        -> Document
document.commit(markdown)          -> Commit { Document, Delta }
```

**There is no session.** A document is the handle; there is no separate mutable
owner, no pending state, and no edit script. `Document(markdown, options)` is
the ONE entry point — the former one-shot parse and the former session open are
the same operation, and each call gets its own `DocumentDomain`, so identities
from different calls never compare equal.

**`commit` READS its receiver and takes nothing.** The document it was called
on keeps everything it owns, stays queryable and walkable, and may be
committed again; it is released by whoever holds it. Two commits from one
receiver are two lines of descent, told apart by their revisions, and nodes
from two lines are no more comparable than nodes from two documents.

**`commit` takes text, not options.** Options are fixed when the document is
created and are immutable for its whole series. A `DocumentDomain` therefore
never changes within a series: a domain change is a new `Document(...)` call,
and its identities compare equal to nothing that came before — which is exactly
what a consumer expects when it changes what the parser means.

`Commit.document` is the complete correctness result and is immediately
self-contained. `Commit.delta` describes what changed between the receiver and
it. A consumer may discard every delta and re-derive from `Commit.document`;
the three integration paths are `incremental-canonical-ast.md` §2.1, and a
binding must support all three.

**What the engine must find, it finds itself.** The consumer hands over whole
text, so the changed region is the engine's to determine — the minimal replaced
span between the previous text and this one. `Delta.edits` reports that span.
Nothing is retained between commits for it, and there is no normalization of a
caller script, because there is no caller script.

**Streaming is this operation, called again.** Appending a chunk means
committing the accumulated text; `incremental-canonical-ast.md` §8.2 forbids a
streaming-only node, tail, type, opcode, domain, cache class, invalidation
branch, or parser algorithm, and one operation makes that true by construction
rather than by discipline. Locating the change costs a prefix and suffix scan,
which is linear in the document and negligible beside the parse.

There is no process-global state in the parse path. Any number of documents may
advance concurrently with no shared lock and no cross-document interference.

## Equivalence invariant

After any sequence of commits, a document is semantically identical to one
created directly from the same final bytes: the canonical diagnostic dump of both is byte-equal. Identities and
revisions are excluded — they are document history, not content.

## Text

- **A commit's text is whole text.** There is no byte-range edit, no pending
  buffer, and no append: a host that has a range replacement applies it to its
  own string and commits the result, which it already holds.
- **A resolved `Scope` is not an input.** Positions come out; text goes in. The
  two were separate types so a projected coordinate could not be fed back in as
  an edit (`incremental-canonical-ast.md` §8.1), and with whole-text commit the
  question cannot arise at all.
- **The store never normalizes its contents and never rejects them.** It holds
  any byte sequence, byte for byte, and hands the same bytes back; UTF-8 is
  assumed and never validated (`incremental-canonical-ast.md` §7.1). A
  truncated final code point at a chunk boundary therefore needs no exception
  to be legal — it is simply bytes, and the document that describes them is an
  ordinary complete document. NUL is replaced with U+FFFD during parsing
  because CommonMark requires it of canonical text, and nothing else is.

## Failure and memory

- **On allocation failure the commit reports an error and the document it was
  called on is done.** There is no restoration and no retry: the caller holds
  the text, so recovery is building a document from it again. An aborted
  identity or revision is burned, never reused.

  What replaced a transactional-and-retryable promise, and why, is
  [`../reviews/2026-08-09-api-model-and-allocation-failure.md`](../reviews/2026-08-09-api-model-and-allocation-failure.md).
  In short: the clause was asserted in one commit with an empty PR body and no
  consumer ever named; it is not inherited from cmark, which aborts; and it is
  unreachable through the shipped public surface, because the default allocator
  aborts and the injecting one is not public. What survives is what the v1
  contract already required — do not crash, do not leak, report the error, and
  never let a truncated document masquerade as success.
- Releasing a document invalidates no OTHER document, node view, byte slice, or
  delta. Its own views die with it, and a superseded document is already dead:
  `commit` ends the one it was called on.
- Routine private storage compaction preserves every public identity,
  revision, and AST value and emits no diff entry.

## Concurrency

- `commit` on one document is externally synchronized (one writer), because it
  supersedes its receiver.
- Documents and deltas are immutable values, safe for concurrent reads from any
  thread according to the binding's value contract.
- Distinct documents are fully concurrent, which is what lets a workspace hold
  one per open file.

## Platform surfaces

The canonical entry point on Swift, Kotlin, and ES is `Document`:

| Operation | Contract |
| --- | --- |
| `Document(markdown, options)` | options are immutable for the document's whole series |
| `commit(markdown)` | returns `Commit { document, delta }`; the receiver is read, not taken |
| `version` | this document's `DocumentVersion` |
| `node(for:)` / `parent(of:)` / `index(of:)` | resolve an identity in this document |
| `scope(of:profile:)` | resolve a stable extent to a `Scope` |
| `footnote(of:)` / `footnotes()` / `resolution(of:)` / `references(of:)` | the parser answers above |
| `concrete` | the retained `ConcreteTree` |

Every one of those is a member of the **document**. There is no empty document
at revision zero and no separate open step: `Document(markdown, options)`
publishes the first document, and every public identity and revision is
positive.

Shared platform types, named identically on all three platforms:

- `MarkupID` — `(DocumentDomain, ordinal)`. Hashable, equatable, and
  serializable, usable unmodified as a SwiftUI `ForEach(id:)`, a Compose
  `key()`, or a React `key`.
- `MarkupTrack` — `{ identity, revision, extent }`; `revision` is one
  `Revision`, covering the node and everything below it. Node equality and
  hashing are the two-word tuple of `canonical-ast.md`.
- `Commit` — `{ document, delta }`.
- `Delta` — `{ before, after, diffs, edits }`. Always present on a platform
  `Commit`; the C-level nullable out-parameter is a C-consumer knob only.
- `Diff` / `DiffParts` — one identity plus the five-flag bitmask. No registry,
  subscription, interest, route, target, or acknowledgement type may be
  layered around them.
- `FootnoteAnswer` / `Resolution` — the records above; absent answers are
  platform-optional (`nil`/`null`) rather than 0.

The footnote label field on `FootnoteDefinition` and `FootnoteReference` is
named `label`, because `track.identity` names node identity; the diagnostic
dump grammar keeps its frozen `id=` key for that label.

### C facade

```text
markdown_core_document_new / _commit / _free
markdown_core_commit_document / _delta
markdown_core_document_node / _parent / _index / _scope / _version / _concrete
markdown_core_document_footnote / _footnotes / _resolution / _references
markdown_core_delta_before / _after / _diffs / _edits / _free
```

Answer and diff results are caller-owned values that stay valid after the
document is released; there are no mutation-bounded borrowed arrays. The whole
`markdown_core_session_*` family is removed rather than deprecated — there is
no session — and with it `markdown_core_session_ordered_delta_entries`,
`markdown_core_session_footnote_*`, `markdown_core_session_reference_info`, and
`markdown_core_document_scope_table`, and a package cannot advertise this
capability while exposing them.

`ParseOptions` and the exhaustive `MarkupVisitor` dispatch contract are
unchanged from `canonical-ast.md`. `MarkupWalker` keeps its two traversal
modes: the typed-visitor overload walks structure in preorder without
resolving scope, and the event overload emits entering/exiting events with the
resolved scope. Neither visits a syntax-only token; concrete traversal is the
`concrete` surface's own.
