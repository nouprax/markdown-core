# Incremental canonical AST contract

Status: **frozen design contract** for the next Markdown Core
self-contained-AST and stable-trace session milestone, revised 2026-07-31. No
current public API implements this target. Shipping requires every
conformance, failure-injection, and complexity gate in this document.

Companion contracts:

- `canonical-ast.md` defines Markdown Core's canonical node inventory and
  parser semantics.
- `sessions-and-deltas.md` defines the current session baseline that this
  SemVer-major contract replaces.

This document defines one edit-optimized canonical Markdown AST and the
exact-base delta that lets a consumer update its own state in work
proportional to what actually changed. It does not define a renderer, layout
model, document workspace, consumer state model, or second parser output.

The words **must**, **must not**, **should**, and **may** are normative.

## 1. SemVer-major adoption

This is an intentionally breaking milestone. Its complete API, identity,
lifetime, and complexity rules ship together. The implementation must not
retain aliases, compatibility projections, or parallel old/new update
surfaces.

| Superseded contract | This contract |
| --- | --- |
| The delta is the update path, and reference identity across snapshots is an optional fast path | Three stated integration paths (2.1); handing over the document is complete on its own — stable keys, `O(1)` equality, and unchanged subtrees the core reuses rather than rebuilds — so no binding API may require a delta |
| A public node cannot retain its exact immutable document owner | A public node is a lightweight read-only view retaining the exact immutable `Document` that resolves its fields |
| A session snapshot may become unusable after the next commit, and a retained snapshot must be `materialize()`d while still current | Every returned `Document` is immediately self-contained and remains readable after later commits and session close |
| One node `revision` conflates local and descendant changes, and its old meaning was the subtree one | `track.revision` is a `MarkupRevision` pair; `.self` and `.subtree` have distinct meanings and no scalar spelling conflates them |
| The commit delta is four disjoint node-ID arrays, plus a second ordered-entry API that merges three of them because that merged form is what bindings actually consume | The commit delta is one postorder `diffs` list, and that is the only form; each entry is a `MarkupID` plus a six-flag `DiffParts` bitmask, and `bubbled` becomes the `DESCENDANT` flag |
| A node's changed part is not reported, so a consumer re-reads the whole node | Each diff entry carries the closed set of parts that changed, at zero per-node storage cost |
| Absolute source position is a whole-snapshot materialization | Absolute position is an `O(log n)` query against stable extents; a position-only shift produces no diff entry at all |
| Footnote and reference indexes are live-session queries | Parser-defined sites, buckets, winners, negative resolutions, and ordered relations are immutable data in the one `Document` |
| The initial empty session may use revision zero | Every public identity and revision is positive; zero is invalid |

`canonical-ast.md`, `sessions-and-deltas.md`, public headers, bindings,
fixtures, and examples must adopt this contract atomically. A package cannot
advertise this capability while exposing the superseded four-array API.

## 2. One public parser model

Markdown Core has exactly one semantic output: the canonical Markdown AST
rooted at `Document`.

```text
one-shot:
    stored Markdown bytes + ParseOptions
        -> Document

incremental:
    Session + ordinary byte edits
        -> session.commit(): Commit

Commit {
    Document document
    Delta delta
}
```

`Commit.document` is the complete correctness result. It is the same public
AST type returned by one-shot parsing. `Commit.delta` is an immutable
exact-base description of what changed between the session's immediately
preceding published document and `Commit.document`.

A consumer may ignore or discard `Delta` and derive all of its state from
`Commit.document`. Doing so cannot change parser output, identity, revisions,
or structural sharing.

The one `Document` owns all parser-produced truth:

- canonical node kinds, scalar fields, text fields, and typed child edges;
- stable node and parser-owned auxiliary identities;
- node-local and subtree trace stamps;
- exact stored source bytes and stable source extents;
- deterministic source-coordinate and raw-to-canonical-text projections;
- definition, reference, resolution, footnote, link, and embed facts; and
- immutable indexes required to resolve those AST values without a session.

Tracking fields are part of the AST. They describe identity, continuity,
source, and semantic equality of the same canonical values. They are not a
second tree, reactive snapshot, subscriber graph, or consumer model.

There must not be:

- a `ReactiveDocument`, `ReactiveSnapshot`, mirror tree, or projection root
  beside `Document`;
- a nested AST plus a separately authoritative normalized AST;
- a compatibility `Document` reconstructed from another public model;
- two independently versioned copies of one field, text, edge, source extent,
  or relation;
- a synchronization protocol between parser-owned semantic surfaces; or
- correctness that depends on replaying every `Delta`.

Persistent arenas, ropes, piece trees, tries, order-maintenance labels, hash
indexes, reverse indexes, and caches are allowed private storage. Their
physical IDs, chunks, balancing, interning, and cache state are not public AST
identity and cannot by themselves advance a public trace.

### 2.1 Integration paths

There are three ways to consume a commit. They see the same document and the
same identities, none is a fallback for another, and a binding must support
all three.

**A — hand over the document.** The consumer reads no delta:

```text
let document = session.commit().document
// hand it to SwiftUI / Compose / React and stop
```

A value-diffing framework computes its own update directly from the AST. Cost:
11.3.

**B — apply the delta to consumer-owned state.** The consumer maintains state
that must be updated in place rather than re-derived: a native renderer's
display list, a DOM or view hierarchy, a text-layout or measurement cache, an
LSP token array, a search or outline index, a database projection, or a
binding's own platform value tree. Its downstream does not diff values, so
path A would mean rebuilding that state on every commit. It walks `diffs`
and edits its own structure. Cost: 11.2; shapes: section 10.

**C — read the delta for location only.** The consumer keeps no mirror and
only needs to know *where* the document changed: a side-by-side editor
highlighting the preview region a keystroke affected, a telemetry probe, a
test assertion. It reads the entries and resolves `Document.scope` against the
retained old document and the new one.

B is why `Delta` is public, and it is the ordinary path below the C API, where
there is no framework to diff on the consumer's behalf. A is why `Delta` is
optional: a B or C consumer that drops or distrusts a delta falls back to A
and reaches the same state (9.6). Neither direction is a downgrade.

Path A puts the whole burden on AST shape. The first two below are what every
binding must guarantee; the last two are properties of the core that a binding
inherits and must not throw away.

- **Identity.** `MarkupID` is a stable, hashable, serializable key, usable
  unmodified as a SwiftUI `ForEach(id:)`, a Compose `key()`, or a React `key`.
  It survives edits elsewhere and is never reused after retirement (5.2).
- **`O(1)` equality.** Node equality and hashing are `(MarkupID,
  revision.subtree)` for whole-subtree equality and `(MarkupID, revision.self)`
  for local equality: two-word comparisons, allocation-free, safe in a render
  hot path. Equal means the values are identical; storage layout produces no
  false negatives.
- **Instance reuse.** The core reuses an unchanged subtree rather than
  rebuilding it, so the next document holds the same node, not merely an equal
  one. This is what makes the structural sharing below a memory fact rather
  than a hope, and it is stated of the core because that is where it is true.

  It is deliberately *not* required of a binding's node views. In Swift an
  idiomatic view is a `struct`, and a value type has no identity to compare —
  there the requirement would not be hard to meet, it would be meaningless. And
  in a language whose views are references, a binding that constructs a fresh
  wrapper on each `node(id)` call destroys the property regardless of what the
  core did, so keeping it would oblige every binding to cache wrappers and
  manage their lifetimes.

  The short-circuit a framework actually runs is the `O(1)` equality above,
  which behaves identically for a `struct`, a `data class`, and a JavaScript
  object. A binding whose views are references may offer identity as an extra
  one-word check; none is obliged to.
- **Structural sharing.** Adjacent documents share every unchanged node, so
  retaining the previous document to diff against costs the changed frontier,
  not a second tree.

The last two also serve path B: a consumer walking `diffs` can skip an
unchanged subtree on the two-word comparison alone, and retaining the old
document to compare against is cheap.

The cost of the path-A diff is stated in 11.3. It is not `O(changed)` — no
top-down value diff can be — but every comparison it performs is `O(1)`, and a
virtualized container reduces it to the visible window.

This is what a self-contained AST is *for*, and it is why no binding API may
require a delta to obtain, retain, walk, or compare a document.

## 3. Parser-only boundary

Markdown Core remains a Markdown parser:

```text
stored Markdown bytes
    -> canonical Markdown syntax and parser-defined semantics
    + stable tracking fields for that AST
    + the exact changed frontier of each commit
```

Markdown Core must not:

- choose fonts, styles, layout, pagination, viewport behavior, paint order, or
  export formatting;
- construct views, platform text objects, accessibility objects, print
  records, or drawing commands;
- evaluate arbitrary expressions or generated application content;
- own a workspace, vault, project graph, network client, or permission policy;
- fetch, open, parse, cache, or recursively expand an embed target;
- store a UI signal, callback, scheduler, subscription, render cache, or
  framework object;
- attach consumer-owned state to an AST node;
- model, version, address, or validate consumer-owned state; or
- retain a consumer-supplied registry, route, interest, contract, target
  identity, or target-state revision.

A UI may consume `Commit.document` directly. An adapter may derive any other
application model from the same `Document`. Both are ordinary consumers of the
AST; neither changes Markdown Core's parser boundary.

The parser reports **what changed**. Deciding what that means for application
state is the consumer's job, and the consumer already owns the only index that
can answer it: its own map from `MarkupID` to its own state. Markdown Core must
not accept a second copy of that map, nor emit edit programs against state it
does not own — it cannot prove such a program correct without computing the
consumer's projection, which this section forbids.

### 3.1 Composition is downstream

One document is one compilation unit, and Markdown Core is its front end: it
turns that unit's bytes into an AST and reports how that AST differs from the
previous one, both as a pure function of `(bytes, options)`. Cross-unit
references are parsed and classified but never resolved, so what the parser
hands out is the *unresolved edges* of the cross-document graph. Owning that
graph — resolution, loading, cycle detection, deduplicated shared targets,
cross-unit numbering, cross-unit invalidation, and layout — is *composition*,
and composition happens strictly downstream, in whatever composition IR a
renderer, exporter, or workspace builds from one or more documents. Markdown
Core produces one AST per byte string and never a merged one.

The compiler analogy that fits is **linking, not `#include`**. A C
preprocessor must splice a header in before parsing because the header can
change how the unit parses; a TeX or typst import must expand during
processing for the same reason, since a macro definition changes later
tokenization. Markdown has no construct by which a target affects its host's
syntax or its host's relation answers, so an embed is an unresolved external
reference, and resolving it can wait until the composer holds every unit it
needs. That is why a model borrowed from those systems does not transfer here,
and it is the only property this boundary actually rests on: were Markdown
ever given a construct through which a target changed its host's parse, the
boundary would have to move, not be worked around.

Composition is cheap to own because the parser hands it working primitives:
stable `MarkupID`s to key its own structures by (5.2); immutable
self-contained documents with structural sharing, so it can hold many units at
once and no unit's commit can invalidate another's (4.2, 14.4.7); and
per-document relation answers, so it knows exactly what remains unresolved
(6.3).

This is not a scoping preference. Three things in this contract break at once
if an imported subtree is materialized into a host document's AST:

- **Fresh-parse equivalence (13).** A fresh parse of the host's bytes would
  not contain the imported nodes unless parsing also read another file, which
  would make the AST a function of a filesystem, a vault, or a network rather
  than of `(bytes, options)`.
- **Identity (5.2).** Imported nodes belong to another `DocumentDomain`.
  Admitting foreign domains into one tree ends the single-domain
  assumption that `MarkupID`, `Document.node`, and the `before` comparison of
  9.6 all rest on; re-minting them instead means they are not stable under the
  imported document's own edits.
- **Coordinates (7.2).** Imported nodes have no bytes in the host's source, so
  `Document.scope` has no answer for them.

A proxy node, a forwarding source identity, a synthetic extent, or a boundary
affinity invented to paper over that third point is a symptom of the boundary
being in the wrong place, not a mechanism. None may be added.

A renderer that must present the embedded content takes the target's own
`Document` and composes the two trees at its own layer.

An `Embed` or `CrossLink` node is therefore a leaf carrying an authored
`reference` string and the parser's classification of it, permanently. No
target content, resolved title, target identity, expansion state, or other
target-derived value may appear on it, or anywhere else in a host AST. If a
later feature wants a host-level semantic to depend on target content —
embedded headings joining the host's outline, an embedded figure joining host
numbering, a resolved display title, a validity mark for a dangling
reference — that feature belongs to the composition layer, for the same three
reasons above. Gate 14.1.7 is the mechanical check: delete every target and
the host's canonical dump must stay byte-identical.

The rest of this subsection is informative. It shows the shape that takes for
an Obsidian-style `![[target]]` with an inline sub-editor, because a rule that
forbids a design is only worth having if the alternative is concrete.

- The workspace resolves `target` to a file and opens a **second session**.
  Host and target each own their bytes, AST, identities, revisions, and
  deltas. Nothing is shared between them.
- The composition layer walks the host AST and, at each `Embed` occurrence,
  splices a subtree built from the target's current `Document`, recording
  `(host MarkupID, target DocumentVersion)`.
- The sub-editor submits its `SourceEdit`s to the **target's** session, in the
  target's own byte coordinates, and resolves its ranges against the target's
  own document. There is no shared coordinate space to map through, which is
  why no proxy extent, forwarding source identity, or host-space projection is
  needed anywhere.
- Committing the target changes the target's document only. **The host's bytes
  did not change, so the host commits nothing** and its AST, identities, and
  revisions are untouched. The composition layer reflows the region it built
  from that target, which is the same work it already does when a host
  paragraph changes height.
- Editing the reference itself is an ordinary host edit: the `Embed` node
  appears in the host's `diffs` with `VALUE`, and the composition layer
  re-resolves.

Exactly three signals drive the whole feature — a commit on the host, a commit
on a target, and a resolver change such as a rename, a move, or a target
appearing or disappearing — and all three are owned by the layer that owns the
resolver.

Under path A (2.1) it is nearly free, with one hazard worth naming: **a target
commit must reach only the views that show that target.** If every embed view
observes one shared resolver object, a commit on any target invalidates all of
them and the host's body re-runs — the `O(width)` walk of 11.3, paid for a
change that touched one embed. The fix is scope, not caching:

- hand each embed view an observable holding exactly **one** target's current
  `Document`, so a target commit invalidates exactly the views showing that
  target and the host's body does not re-evaluate at all — its input, the host
  document, is reference-identical and did not change (2.1);
- give that view the embed node's `reference` rather than the node or the host
  document, so an unrelated host edit leaves its input equal and the framework
  skips it outright; and
- key it by the embed node's `MarkupID`, which is stable under edits elsewhere
  (5.2), so a host re-render preserves the view's identity and with it the
  sub-editor's cursor, scroll offset, and undo stack instead of tearing them
  down and rebuilding the target's whole subtree.

Polling every embed on every commit — asking each target's renderer whether it
should refresh — is the shape to avoid: it is `O(embeds)` per commit of any
document and it defeats the framework's own skip.

Two consequences belong to the composer rather than the parser. A target
embedded `k` times is one immutable `Document` shared by `k` composition
subtrees instead of `k` materialized copies. And relation answers are
per-document by 6.3 — a target's `footnotes()` numbers the target's own
footnotes — so a composed presentation that wants one continuous sequence
assigns display numbers itself.

## 4. Canonical AST shape

The canonical node inventory, legal child kinds, field semantics, source
order, and Markdown recovery behavior remain owned by `canonical-ast.md`.
This contract adds the tracking shape required for stable editing.

Conceptually:

```text
Document : Markup {
    DocumentTrack documentTrack
    ChildList<Markup> content
}

DocumentTrack {
    DocumentVersion version
    SchemaVersion   schema
    ParseOptions    options
    Source          source
    Relations       relations
}

Markup {
    MarkupKind kind
    MarkupTrack track
    typed Scalar fields
    typed CanonicalText fields
    typed optional/singular child fields
    typed ChildList fields
}

MarkupTrack {
    MarkupID       identity
    MarkupRevision revision
    SourceExtent   extent
}

MarkupRevision {
    Revision self       // this node's own local projection
    Revision subtree    // that, plus everything reachable below it
}
```

This is a semantic shape, not a mandatory memory layout. Bindings may expose
properties, methods, protocols, interfaces, or borrowed views, but every value
has exactly one owner and one meaning.

`node.track.identity` is the sole public node identity. `track.revision` is
always the pair and never a single number: no scalar member conflates local
with subtree change, which is the ambiguity section 1 removes. No wrapper
identity is layered around `MarkupID`.

`MarkupTrack` is deliberately small — four members, and no more may be added.
Per-field, per-text, per-edge, per-source, and per-relation revision stamps
must not be stored on the node.
Their only consumer is a pull-mode diff, which is `O(document)` regardless
(section 11.3), and it compares whole nodes rather than fields. The same
information is carried at zero per-node cost by the projection parts of
section 9.1, which are paid for only by commits that actually change
something.

### 4.1 Document-bound node views

A node obtained from a `Document` is a read-only view of one canonical node in
that exact AST revision. These concepts are distinct:

- logical continuity: `MarkupID`;
- exact view identity: `(DocumentVersion, MarkupID)`; and
- selected-value equality: `(MarkupID, revision.self)` for the node's local
  projection, `(MarkupID, revision.subtree)` for its whole subtree.

An old node view resolves only through its retained old `Document`. It cannot
be passed to a new document as an exact view merely because its `MarkupID`
survived. A caller resolves logical continuity explicitly:

```text
Document.version          -> DocumentVersion
Document.node(MarkupID)   -> Optional<Markup>
Document.parent(MarkupID) -> Optional<MarkupID>
Document.index(MarkupID)  -> Optional<ChildOrdinal>
```

A returned `Markup` *is* the document-bound view; there is no separate view
type layered around it. The accessor keeps the name `node` because a caller
reads it as "the node for this id", and because a second `markup` spelling
would collide with the member it is passed.

`Document.version` is the direct accessor for `documentTrack.version`. Every
consumer needs it for the one comparison of 9.6, so no consumer should have to
walk to it.

`none` means the identity is not live in this document. Passing an identity
from another domain is a programmer error and traps; it is not a result
value. There is no `Checked<Optional<...>>` double wrapping.

The tree need not copy every descendant wrapper into each parent:

- a child edge semantically contains ordered child nodes;
- private persistent storage may retain stable `MarkupID` references;
- iteration returns views bound to the owning `Document`; and
- wrapper or pointer identity is never logical node identity.

When only a descendant field changes, an implementation may share every
unchanged ancestor's local record and child sequence. A persistent aggregate
trace index may advance `revision.subtree` without copying all ancestor
payloads.

### 4.2 Self-contained document revisions

Every published `Document` must be:

- immutable;
- self-contained when returned;
- safe for concurrent reads according to its binding's value contract;
- independent of later session commits;
- usable after the session closes;
- able to resolve every live `MarkupID`, parser-owned site, extent, text,
  child edge, relation, and absolute source coordinate it exposes; and
- structurally shareable with adjacent immutable revisions.

Lazy indexes are allowed only when their computation depends entirely on
immutable values owned by that `Document`. A retained document must never
call into the live session, consult its current source, or fail because the
session advanced. There is no `materialize()` step and no window during which
a retained snapshot is only conditionally usable.

## 5. Identity and revisions

### 5.1 Document version

```text
DocumentVersion {
    DocumentDomain   domain
    Revision revision
}
```

A `DocumentDomain` is the scope that node identities and revisions live in. It
is unique to one document lineage and includes the schema, source profile,
parse options, and source profile that can affect AST truth. Changing
one of those inputs starts a fresh domain; it is not an ordinary same-lineage
update.

A domain is opaque and is only ever compared for equality. A consumer never
constructs or inspects one.

`Revision` is positive and monotonic within one domain. A successful
canonical no-op reuses the exact current `Document` and revision. Every
published canonical change advances it. Reserved candidate revisions may be
burned on failure but never reused.

### 5.2 Markup identity

```text
MarkupID {
    DocumentDomain domain
    MarkupOrdinal  value
}
```

`MarkupID` identifies one logical Markdown node while that node remains
continuous across adjacent committed parses. It is not an array index, source
offset, dense path, content hash, pointer, or wrapper reference.

The parser owns continuity. It may preserve a node identity only when its
language-specific continuity proof remains valid. A retired `MarkupID` never
becomes live again in the same domain. Delete and later reinsert allocates
a fresh identity, even if bytes and canonical value return to an earlier
value.

New siblings cannot steal identities from surviving old siblings. Matching
must use stable old witnesses and deterministic tie-breaking, not "first equal
node wins" against the new order.

### 5.3 Revision law

There is one counter, and one scalar type for it. `Revision` is positive and
strictly monotonic within a domain; every stamp in this contract is a value
drawn from it, and `revision.self` and `revision.subtree` each record the
revision at which that projection last changed. Nothing has a private revision
space, and no type exists for a per-source, per-field, or per-edge revision.

- zero is invalid;
- a canonical no-op publishes nothing, so no stamp moves;
- a published change advances `DocumentVersion.revision` strictly and moves
  exactly the stamps whose projection differs; and
- exhaustion fails or starts a deliberate fresh domain; it never wraps.

Because every stamp is a document revision, stamps are comparable, and the
comparison a consumer actually wants costs `O(1)`:

```text
markup.track.revision.subtree > remembered.revision
```

answers "did this subtree change since I last looked" with no diff and no
second traversal. Four disjoint revision spaces could not answer it. A stamp
is meaningful only paired with the `MarkupID` it belongs to; comparing stamps
across different nodes says nothing.

Strict monotonicity makes this ABA-safe without further machinery: an
A -> B -> A value sequence necessarily ends at a revision above the one the
first A carried. No separate ABA stamp, nonce, or distinctness rule is
required, and none may be added.

### 5.4 Node aggregate traces

`revision.self` covers the node's local canonical projection:

- kind;
- scalar and text field values;
- direct typed child-edge membership and order;
- directly stored parser relations; and
- stable source shape/provenance, excluding revision-relative numeric
  coordinates.

`revision.subtree` covers `revision.self` plus the recursively reachable child
semantic projections.

These are equality and pruning aids, not the update mechanism. Two views with
equal `(MarkupID, revision.self)` have identical local values; equal
`(MarkupID, revision.subtree)` have identical subtrees. `revision.subtree`
advancing is exactly the membership condition of section 9.1, and
`revision.self` advancing is exactly the condition for a flag other than
`DESCENDANT`. Section 9 is how a consumer learns *that* something changed;
these stamps are how it cheaply confirms that something did not.

## 6. Tracked AST values

### 6.1 Scalar and text fields

```text
CanonicalText {
    Utf8Text value
    TextMap  map
}

TextMap = [SpanPair]       // ascending, non-overlapping, covers `value`

SpanPair {
    Span canonical         // into this field's `value`
    Span source            // into the document's stored bytes
}
```

Scalar fields are plain typed values. Optional `none` is a real field value,
not an absent node; a presence change is an ordinary field change.

`CanonicalText.value` is canonical UTF-8 text. `CanonicalText.map` relates
each stretch of it to the source bytes that produced it, one `Span` (8.1) per
side. An entry carries no text of its own: unlike a run in a text-layout or
attributed-string API, it is a correspondence, not a slice of content.

The two spaces are not the same and do not advance at the same rate: `value`
holds decoded text, the source holds what was authored, and they diverge
wherever Markdown transforms bytes. `&amp;` is five source bytes and one
canonical byte; `\*` is two and one; `&#x1F600;` is nine and four; with smart
punctuation `--` is two and three. Source bytes that produce no canonical
output at all, such as an escape's backslash or a stripped comment, fall
between pairs.

Both spans are therefore stored, rather than one span and a shared length.
Neither side's length derives from the other, and neither derives from the
neighbouring entries, because the canonical side is contiguous while the
source side may have gaps. When a pair's two spans hold *identical bytes* the
correspondence inside it is byte for byte and a consumer may address any
interior position; otherwise the pair corresponds only as a unit.

Equal length is not the test, and using it would be unsound. With smart
punctuation `...` is three source bytes and `…` is three UTF-8 bytes, and
`---` and `—` are three and three: the lengths agree while no interior position
corresponds, so a consumer addressing into one would land inside a UTF-8
scalar. Comparing the bytes costs `O(len)` on two slices the consumer already
holds, and is paid only when interior addressing is actually wanted.

No entry carries the text it maps, and none may be added. A consumer that
wants the bytes takes them itself: `value` for the canonical side, the
document's `Source.content` for the source side, both sliced by the span it
already holds. `O(1)` persistent slicing is required of both (7.1 and below),
so the slice is a view, not a copy. Storing the content in the map instead
would keep a second copy of every text field in the document and defeat the
structural sharing the rest of this contract rests on — and what to do with
those slices, including composing them with anything else, is consumer work
(3).

Equal canonical text with a changed escape/entity/provenance mapping — `&amp;`
rewritten as `&#38;` — is a `TEXT_MAP` change, not a `TEXT` change (9.1).

Text storage must support persistent slicing and localized replacement.
Repeated tail appends must not copy the complete prefix each commit. A tiny
retained slice must not accidentally retain an unbounded source buffer beyond
the binding's documented amplification limit.

### 6.2 Typed child edges

```text
ChildList<T: Markup> {
    ChildRole role
    [T]       value
}

ChildRole =
    CONTENT | ITEMS | ROWS | CELLS
```

The child sequence must be persistent: localized insert/remove/replace
operations path-copy only the
affected persistent frontier plus inserted or removed members. Dense positions
and private order labels do not escape as stable identity. Rebalancing a
persistent sequence with equal membership and order changes no public
revision and emits no diff entry. Child kind legality remains statically or
dynamically checked by the canonical AST schema.

### 6.3 Parser-owned auxiliary records

```text
Relations     // opaque; a document's parser-owned auxiliary records
```

`Relations` is opaque in this contract. Its record types and its query surface
are owned by `canonical-ast.md`; what is specified here is only how a change
to it reaches a consumer. It holds immutable parser-defined records for:

- link/reference definitions and occurrences;
- normalized definition buckets and deterministic winners;
- positive and negative reference resolutions;
- footnote definitions, references, ordering, and labels;
- cross-link and embed occurrences; and
- recovery facts required to explain the canonical AST.

Each record has a stable typed identity. Negative results are explicit
immutable values so a later definition insertion can be discovered without
pretending "nothing was read."

A change to any of these records surfaces as a `RELATIONS` part on
every `Markup` whose answers changed, discovered through a parser-owned
inverted index so that the cost is proportional to the affected nodes and not
to the number of nodes that could have been affected. Auxiliary records carry
no separate public revision scope: a consumer that projects a relation-derived
value holds it against the `MarkupID` it belongs to, and that identity is what
the diff entry names.

These records describe Markdown parsing only. An embed occurrence contains
the authored target and parser-defined classification; it does not load or
parse the target.

## 7. Exact source model

### 7.1 Stored bytes

```text
Source {
    SourceProfile profile
    [byte]        content
}

SourceProfile =
    STRICT_UTF8       // stored bytes must be valid UTF-8
  | PERMISSIVE_BYTES  // any byte sequence may be stored
```

The index that makes 7.2 resolve in `O(log n)` is private storage, not a
member of `Source`: the capability is public, the structure is not (2).

The source owns the exact committed stored bytes, including bytes that are not
valid UTF-8 when the selected source profile permits them. Canonical Markdown
decoding and recovery are deterministic functions of those bytes and the
frozen profile.

The source carries no revision of its own: every published document has
different stored bytes from its predecessor, because a commit whose normalized
edits leave the bytes unchanged is a no-op that reuses the current document
(5.1). So a source revision would equal `DocumentVersion.revision` always.

An edit that changes bytes without changing the canonical AST — trailing
whitespace, for instance — therefore advances `DocumentVersion` and produces
an empty `diffs`. Old documents retain their exact old bytes.

### 7.2 Stable extents and lazy coordinates

```text
SourceExtent {
    DocumentDomain domain
    ExtentOrdinal  value
}

CoordinateProfile =
    STORED_BYTE
  | UNICODE_SCALAR
  | UTF16
  | LINE_COLUMN
  | NATIVE(closed binding-defined projection)

Document.scope(SourceExtent, CoordinateProfile)
    -> Optional<Scope>
```

`Scope` and `Position` are owned by `canonical-ast.md`, which defines
`Scope(start: Position, end: Position)`, and `document.scope(of:)` is the
existing accessor this generalizes. `Scope` itself is unchanged; because this
contract offers five coordinate profiles, `Position` widens from line/column
to the requested profile's position type, with line/column staying the profile
the dump grammar prints:

```text
Position =
    Offset                   // STORED_BYTE
  | EncodedOffset            // UNICODE_SCALAR, UTF16
  | (line, column)           // LINE_COLUMN, the form canonical-ast.md defines
  | closed binding value     // NATIVE
```

Stored bytes are the substrate, so an offset into them is just `Offset`. The
other profiles are projections computed from that substrate, so their offsets
are `EncodedOffset` and say so. The distinction is load-bearing: a UTF-16 and
a stored-byte offset agree on ASCII and diverge on everything else, and only
one of them may be used to construct a `Span` (8.1). `EncodedOffset` need not
name which projection produced it, because it only ever arrives from a
`Document.scope` call whose profile the caller just named.

An extent identity is stable editing identity, not a captured numeric range.
It carries no coordinates by construction: a stored span would go stale the
moment anything before it is edited, which is the whole reason extents exist.

It is domain-qualified for the same reason `MarkupID` is (5.2): two sessions
mint ordinals independently, so an extent from one document must not resolve
against another. The three further members it might otherwise carry are
fossils of a merged-document model this contract rejects (3.1):

- it names no source, because a document owns exactly one `Source` (7.1);
- it carries no boundary affinity, because the parser assigns every extent
  exactly on each parse, so nothing ever extrapolates a boundary across an
  edit; and
- it needs no proxy or forwarding form, because every extent it can name lies
  in this document's own bytes.

Affinity is what a consumer-owned marker needs in order to survive an edit
without a reparse — a cursor, a bookmark, a selection. That is consumer state,
and it is the consumer's to keep (3).

`SourceExtent` stays distinct from `MarkupID` so that sub-node extents — a
link destination span, a fence info string — can be named later without a new
addressing scheme. A node's `extent` is the only one today, which is why it is
not called a *primary* extent: there is no secondary one to be primary against.

It carries no `ID` suffix, unlike `MarkupID`, because the identity is the whole
of it. A `MarkupID` names a `Markup` that exists separately; there is no
`SourceExtent` record behind a `SourceExtent`.

An extent that no longer exists resolves to `none`; the parser never silently
retargets it by current dense ordinal.

Resolution must be `O(log n)` in the number of source-bearing units and must
not require a whole-document materialization pass. A persistent aggregate
sequence keyed by private order-maintenance labels, carrying subtree byte
sums, satisfies this: an edit path-copies `O(log n)` nodes, and every other
node's absolute coordinate is recomputed on demand from the same tree.

Every `CoordinateProfile` is deterministic and schema-versioned. `NATIVE` is
data, never a callback or platform object.

### 7.3 Source movement is not a change

Numeric source positions are revision-specific query values. They are not node
identity, canonical semantic field content, or a diff entry.

A prefix insertion:

- preserves every later node's `MarkupID`;
- preserves their `revision.self` and `revision.subtree`;
- preserves parse and derived relation values;
- changes the numeric coordinates obtained by resolving their stable extents;
  and
- **emits no diff entry for any of them**.

The parser must not rewrite every later node, field, or extent merely to store
new absolute offsets, and must not manufacture diff entries to describe a
shift. Old documents continue to resolve their old positions exactly.

A consumer that has chosen to materialize absolute coordinates into its own
state remaps them from `Delta.edits` (section 9.2) — the normalized form
of the edits it submitted itself. That cost belongs to the consumer that chose
to denormalize, is proportional to what it chose to hold, and is not
observable by any consumer that resolves coordinates on demand. There is no
coordinate event family, remap contract, remap side channel, or coordinate
route.

## 8. Ordinary edits and commits

### 8.1 Source edit primitive

```text
SourceEdit {
    Span   span         // the half-open run to replace
    [byte] replacement  // the bytes that take its place
}

Span {
    Offset start
    Offset end
}

Session {
    edit(SourceEdit)
    commit()       -> Commit
    commit([byte]) -> Commit    // optional binding convenience, below
}
```

`span` is a half-open run of *stored bytes* in the session's current pending
source. It is not a `Scope`: a scope is a revision-relative,
profile-dependent query result resolved against a published document, and an
edit must name storage in the pending one. The two stay distinct types so a
resolved scope cannot be fed back in as an edit, and `Span` is built from
`Offset` rather than `EncodedOffset` so a projected coordinate cannot be
either (7.2).

Byte granularity is also what lets a
streamed chunk deliver the first half of a multi-byte character and a later
chunk complete it (8.2), and it is the only space that stays well defined
under a storage profile that permits bytes which are not valid UTF-8 (7.1).

Edits name the current pending source coordinate space. A binding may expose
single-edit and batch conveniences, but they normalize to one deterministic
non-overlapping edit set before parsing. Overlap, overflow, stale source base,
or a source-profile violation fails without publishing a partial source or
AST.

Edits do not directly mutate a published `Document`. `commit` applies pending
edits to a private source candidate, parses and validates one complete
candidate AST, computes its `Delta`, and publishes both atomically.

A binding may additionally offer `commit(source)`, which normalizes the
difference between the given bytes and the session's stored bytes into the
same deterministic edit set. It is a convenience over the same primitive, for
a host that owns the whole text rather than the edit — a two-way text binding,
a document reloaded from disk, a remote replacement. It must produce the
identical document, identity, and delta as the equivalent explicit edits, and
it costs one byte-level diff of the two buffers, so a host that already knows
its edit should submit that edit instead.

### 8.2 Streaming is ordinary editing

Typing, paste, IME replacement, remote collaboration chunks, and LLM output
all use the same byte-edit and commit path. Appending a chunk is an insertion
at end-of-source. Correcting an unfinished construct is an ordinary
replacement.

There must not be a streaming-only node, mutable tail, provisional AST type,
finalize opcode, revision domain, cache class, invalidation branch, or parser
algorithm. Chunk partition and commit scheduling may affect performance and
intermediate valid ASTs, but not final canonical output.

## 9. Delta

A document has exactly two authored levels: its bytes, and the nodes parsed
from them. A delta is the difference between two documents at both levels, and
nothing else.

```text
Delta {
    DocumentVersion before   // which two documents
    DocumentVersion after
    [Diff]          diffs    // how their nodes differ  (9.1)
    [SourceEdit]    edits    // how their bytes differ  (9.2)
}

Diff {
    MarkupID  markup
    DiffParts parts
}

DiffPart =
    VALUE | TEXT | TEXT_MAP | CHILDREN | RELATIONS | DESCENDANT
```

9.1 gives each part its meaning and 9.3 the rule that closes the set.

Absolute source coordinates are neither level. They are a query over the byte
level, which is why they are resolved through `Document.scope` (7.2) and never
delivered (7.3).

`before` is the exact previously published document identity and `after`
equals `Commit.document.version`. The delta carries no schema
member: `before` pins the domain, and the domain pins the schema (5.1).

`Delta` is plain immutable caller-owned data. It is not an AST, an alternate
root, a log a consumer must replay, a subscriber registry, a renderer change
list, or correctness state required by `Document`. It retains no consumer
value and no mutable parser session, and it remains valid after the session
advances or closes.

`Delta` is read by paths B and C of 2.1. Path A never reads it, and nothing in
this contract requires it: a consumer that drops every delta and re-derives
from `Commit.document` reaches the same state (9.6, 14.6).

It is not an AST mutation protocol. The AST is immutable and a delta never
changes it. What a path-B consumer mutates is its own state, addressed by
`MarkupID`; the parser neither models nor validates that state (3).

### 9.1 `diffs`: the node-level difference

Write `proj(n)` for a node's observable projection, split into parts:

```text
proj(n) = (VALUE, TEXT, TEXT_MAP, CHILDREN, RELATIONS, DESCENDANT)

VALUE       kind, scalar fields, singular child edges, source shape
TEXT        canonical text bytes
TEXT_MAP    raw-source to canonical-text segment mapping
CHILDREN    the list-valued child edge: membership and order
RELATIONS   this node's parser relation answers
DESCENDANT  the projections of everything below it
```

Absence is a projection value: `proj(n) = ⊥` when `n` is not live in that
document. Then, for every `n` live in `before` or `after`:

```text
n ∈ diffs    ⟺  proj_before(n) ≠ proj_after(n)

parts(n)     =  { p : proj_before(n).p ≠ proj_after(n).p }
                restricted to the parts n has in `after`
```

That is the entire definition of `diffs`. Everything below is a consequence
of it, not an additional rule.

**A retired node has no parts in `after`, so `parts` is empty.** That is why
there is no lifecycle tag: a consumer distinguishes the case with
`Document.node`, which returns `none` for exactly those nodes. A created node
differs from `⊥` in every part it has, so it carries all of them.

**`DESCENDANT ∈ parts(n)` exactly when some node below `n` differs**, which is
exactly when `n`'s `revision.subtree` advanced (5.4). The ancestor spine of
every change is therefore in `diffs`, because those ancestors' projections
genuinely differ — their `DESCENDANT` part changed. A consumer that
materializes a parent-linked structure needs precisely that; a consumer that
keys a flat map skips a `DESCENDANT`-only entry with one flag test.

**A node whose only change is its absolute source position emits nothing**,
because position is not in `proj` at all.

**Private storage compaction, rebalancing, interning, and cache maintenance
emit nothing**, because they change no projection. A canonical no-op produces
an empty `diffs` and reuses the document.

**A container whose child *values* changed but whose child *sequence* did not
gets `DESCENDANT`, not `CHILDREN`.** `CHILDREN` is membership and order of the
child identity sequence. This is the distinction that lets a consumer holding
a `W`-wide container skip an `O(W)` rebuild for an `O(1)` change.

**`diffs` is a pure function of (`before`, `after`).** Two commits reaching
the same document from the same document produce identical `diffs`,
regardless of how the edit was expressed, how many chunks it arrived in, or
what the parser did internally.

**`|diffs|` is the changed frontier plus its deduplicated ancestor spine**,
bounded by `O(changed * depth)` and independent of document width and size.
There is no padding, sentinel, or whole-index entry: a projection either
differs or it does not.

### 9.2 `edits`: the byte-level difference

`edits` is a normalized, non-overlapping, ascending edit script from `before`'s
stored bytes to `after`'s stored bytes, expressed in `before`'s coordinate
space. The session uses the caller's own edits, normalized.

Unlike `diffs` it is not required to be minimal, and it is not a pure
function of the two byte strings: two routes to the same document may carry
different but equally valid `edits`. A consumer must treat it as *an* edit
script, not *the* edit script.

It is here for the consumers that do not already know it. An editor that
submitted the edit does; these do not:

- a host that called `commit(source)` (8.1) handed over a whole replacement
  buffer and never computed a range — the session did, and `edits` is the only
  place that result exists;
- a component downstream of the editor — a preview pane, an outline, a
  collaborating peer, an indexer — did not submit the edit at all; and
- a side-by-side editor showing *where* the document changed needs both
  halves: `diffs` locates it in the preview pane, `edits` locates it in the
  source pane.

For a consumer that has materialized absolute coordinates, replaying `edits`
in one ascending prefix-sum walk over its held entries costs
`O(|edits| + held)` against `O(held * log n)` for re-resolving each through
`Document.scope`. That is a constant-factor win, not an enabling one — the
coordinates are always recoverable from the document — and it is worth having
only because the session normalized this script before parsing, so producing
it costs nothing.

`edits` carries the replacing bytes, not the replaced ones, so it is not an
inverse: a consumer that needs the old bytes reads them from the retained old
document, which still owns them exactly (7.1). It carries no node identity and
describes no consumer state.

### 9.3 Why these six parts and no more

`DiffPart` is closed, and the rule that closes it is a cost rule, not a
taxonomy:

> a part exists if and only if a consumer that ignored it would either be
> wrong, or pay more than `O(1)`.

`TEXT` and `TEXT_MAP` are `O(length)`. `CHILDREN` is `O(width)`. `RELATIONS`
is the cost of a parser relation query. A consumer that ignored `DESCENDANT`
while materializing a parent-linked structure would be *wrong*: it would
retain an ancestor holding a stale child, and a value-diffing UI below it
would never reach the change. Everything else about a node — its kind, its
scalar fields, its singular child edges, its source shape — is `O(1)` to
reproject, so it is one part, `VALUE`, and needs no field address. Splitting
`VALUE` further would let a consumer skip `O(1)` work at the cost of a wider
vocabulary in every binding forever.

Two facts about the canonical node inventory remove the parameters:

- no canonical node has two text-valued fields, so `TEXT` and `TEXT_MAP` need
  no field address; and
- no canonical node has two list-valued child edges — `Table` pairs one
  singular `header` with one list `rows`, `DirectiveBlock` pairs one singular
  `label` with one list `content`, and every other kind has exactly one of
  `content`, `items`, `rows`, or `cells` — so `CHILDREN` needs no `ChildRole`.

A change to a singular child edge is a change to the owning node's local
value, and is therefore `VALUE`: relinking one child reference is `O(1)`.

`parts` is consequently a six-flag bitmask, a `Diff` is a `MarkupID` plus one
byte, and `diffs` is a flat array with no variable-size records and no
pointer chasing. The parser always reports every part that differs; filtering
is a local predicate on data the consumer is already iterating, so there is no
interest declaration, subscription, or negotiation.

### 9.4 What the delta does not carry

- **No created/retired/changed tag.** 9.1 derives it: `Document.node(markup)`
  returns `none` exactly for retired nodes, and a consumer's action for
  created and changed is the same — reproject from the current document.
- **No parent.** `Document.parent` answers it for live nodes, and a retired
  node's surviving parent carries `CHILDREN` in the same delta.
- **No ordinal or position.** `Document.index` is `O(log n)`.
- **No per-field address.** See 9.3.
- **No revision numbers.** Membership in `diffs` *is* the statement that the
  node's revision advanced.
- **No coordinate entries.** See 7.3.

Each is reachable from the self-contained `Document` in `O(1)` or `O(log n)`.
Putting a derivable value in the delta means every commit pays to produce it
for every consumer, including the ones that never read it.

### 9.5 Serialization and composition

9.1 defines `diffs` as a set. Two choices turn it into a list.

**Uniqueness.** Each `MarkupID` appears exactly once, carrying the union of
its differing parts.

**Postorder.** Retired entries come first, in `before` postorder; live entries
follow, in `after` postorder. Postorder is what gives a bottom-up consumer the
property it needs without a second API: every entry whose parent also has an
entry appears before that parent, so one forward pass evicts, then rebuilds
children before parents, with no ancestor walk and no sort. It is also a total
document order, so a consumer holding a postorder-sorted key set can intersect
the two by galloping search in
`O(min(|diffs|, |held|) * log max(|diffs|, |held|))` instead of scanning.

This is why there is one list rather than a delta plus a separate ordered-entry
table that merges three of its arrays: the ordered form is the only form.

**Composition.** Concatenating adjacent deltas is sound:

```text
diffs(A,B) ++ diffs(B,C)  ⊇  diffs(A,C)
```

It is a superset, not an equality — an A -> B -> A node appears in both halves
and in neither side of `diffs(A,C)`. Applying a superset is correct because
an entry names an **address** and never a value or an operation: the consumer
re-reads the current document at each address, so a spurious entry reprojects
a node to the value it already has. Duplicates are idempotent for the same
reason, and a consumer may merge them or not.

There is therefore no `compose` operation, no compose error, no ABA stamp, and
no requirement that a consumer preserve intermediate states it never observed.

### 9.6 The complete consumer contract

A consumer holds its own map from `MarkupID` to its own state. That map is the
only index the update needs, and the consumer necessarily already has it.

```text
sync(commit):
    if myBase != commit.delta.before:
        rebuild from commit.document; myBase = commit.document.version; return
    for diff in commit.delta.diffs:              // children before parents
        node = commit.document.node(diff.markup)
        if node == none:                drop my state for diff.markup
        else if parts == {DESCENDANT}:  relink my value for diff.markup
        else:                           reproject the parts I hold, then relink
    myBase = commit.delta.after
```

This is the whole protocol. A stale, skipped, wrong-domain, wrong-schema,
replayed, or corrupted delta is detected by the single `before` comparison and
resolved by rebuilding from the self-contained current `Document`. There is no
fallback reason enumeration, no acknowledgement round trip, no nonce, no
prepared-finish handshake, no target-state precondition, and no registry
advance, because the parser holds no consumer state that could go out of sync.

Rebuilding is always available and always correct. Its cost is the consumer's
own projection cost over the document, reported separately from delta cost
(section 11.3).

## 10. Delta consumer patterns

This section is informative and applies to paths B and C of 2.1. Nothing here
is a contract the parser implements, names, or validates.

**Consumer-owned mirror — the general path-B shape.** The consumer keys its
own records by `MarkupID` and walks `diffs` in order. An entry whose identity
is absent from the new document evicts that record; every other entry
reprojects it, creating on first sight.

Placement needs no delta member: a container carrying `CHILDREN` reads its new
child order from the document, and an isolated new node resolves
`Document.parent` and `Document.index` in `O(log n)`.

Because entries arrive in postorder, a structure that must be built bottom-up
— an immutable value tree, a layout tree, a display list — completes in one
forward pass: a `DESCENDANT`-only entry relinks a container around
children already rebuilt earlier in the same pass, with no ancestor walk and
no sort. A parent-linked immutable tree is also what a value-diffing UI needs
downstream, which is why the spine is in the list at all (9.1).

**Ordered flow or outline.** The consumer keys rows by `MarkupID` and keeps
them in document order. A block insertion arrives as one entry for the new
block plus a
`CHILDREN` part on the container; the row's position comes from
`Document.index`. No movement or reflow entries are emitted for the unchanged
suffix, because nothing about it changed.

**Materialized absolute coordinates.** The consumer that stores absolute
offsets for `K` nodes remaps them from `edits` in `O(edits + K)` and
reprojects only the entries that also appear in `diffs`. The consumer that
instead calls `Document.scope` on demand pays nothing for a shift. Which of
the two happens is a consumer decision, and the parser is not told which.

**Query-derived projections.** A consumer holding a footnote list or a
resolved-link view watches for the `RELATIONS` part on the identities it
holds and re-runs the parser query for those. Negative and empty results are
real immutable values in the document, so a later definition insertion is
discovered as an ordinary `RELATIONS` change rather than by rescanning.

## 11. Complexity contract

### 11.1 Parser commit cost

`session.commit()` is measured independently of any consumer:

- edited stored bytes and persistent source paths;
- reparsed grammar ownership regions;
- identity-matching frontier;
- changed canonical AST records and trace stamps;
- parser relation/index maintenance;
- persistent nodes/bytes copied; and
- `Delta` construction.

A localized edit may legitimately reparse a complete paragraph, unclosed
fence, raw HTML block, directive region, or other grammar-defined owner. It
must not repeat a whole-document parse once per changed node.

Structural edits path-copy `O(log n)` persistent sequence nodes plus the
inserted or removed members. `|diffs|` is the changed frontier plus its
deduplicated ancestor spine (9.1), bounded by `O(changed * depth)` and
independent of document width and size: a one-block insertion into a document
of any size produces its own entry plus one per enclosing container.

Two implementation constraints follow from that bound and are easy to violate.
`DESCENDANT` must be discovered by walking **up** from the changed frontier,
never by testing a container's children: a retired child seeds that walk
exactly as a changed one does, whereas deciding a container's `DESCENDANT` by
comparing its child list costs `O(width)` per container and breaks the bound.
And a node reaches the frontier only if its own projection actually differs —
an edit that resolves to no change must not seed the walk, or a no-op
publishes an ancestor spine (14.5.4, 14.5.5).

A definition or footnote edit costs the reparse and re-resolution of exactly
the units whose answers changed, collected through the parser-owned inverted
index. It must not scan every unit that contains a reference.

### 11.2 Delta application cost

Applying a delta is:

```text
O(|diffs|) + sum over touched entries of the consumer's own projection cost
```

The first term is the parser's contract. The second is the consumer's and must
be reported separately; a small diff list must never be advertised as
constant total work when the consumer's per-entry projection is not constant.

The implementation must not satisfy the first term with:

- a whole-document scan hidden inside commit;
- one whole-index sentinel presented as an exact diff list;
- materialization of every shifted source extent; or
- a constant-size token whose required expansion is falsely counted outside
  both `|diffs|` and consumer execution telemetry.

### 11.3 Framework diffing from the document

A consumer following 2.1 performs a top-down value diff instead of reading a
delta. Its cost must be documented honestly:

- it compares the children of each container it descends into, so it visits
  `O(sum of sibling counts along the dirty spine)` nodes, not `O(changed)`;
- every comparison is `O(1)` — a reference check, then a two-word tuple — so
  the constant is a cache line, not a parse; and
- it descends only where a comparison failed, so an unchanged subtree of any
  size costs exactly one comparison.

No top-down value diff can be `O(changed)`: finding which child of a container
changed requires looking at its children. A virtualized container
(`LazyVStack`, `LazyColumn`, a windowed list) holds only the visible slice, so
in practice this is `O(visible)` per commit regardless of document size.

This is not a fallback, and reading a delta is not an upgrade over it. Path A
suits a downstream that diffs values; paths B and C suit a downstream that
does not, or that only needs the location. Each pays the cost stated for it.

## 12. Publication, failure, and lifetime

Commit is atomic:

- it publishes one immutable `Document` and its matching `Delta`, or publishes
  neither;
- failure leaves the prior document/source current and readable;
- an aborted identity or revision is never reused; and
- pending-edit retry starts from the still-current committed source.

One session has one externally synchronized writer. Published documents and
deltas are immutable values that may be read concurrently according to binding
rules.

There is no process-global state in the parse path. Any number of sessions may
exist and advance concurrently with no shared lock and no cross-session
interference, which is what lets a workspace hold one session per open
document (3.1).

Closing a session invalidates no published `Document`, node view, stored byte
slice, or `Delta`.

Routine private storage compaction preserves every public identity, revision,
and AST value, and emits no diff entry. It is not a semantic commit.
Cross-domain import or profile/schema change produces a fresh document
stream; a consumer detects it by the `before` comparison of section 9.6 and
rebuilds.

## 13. Fresh-parse and chunk equivalence

After every successful commit, the incremental document's canonical dump
excluding tracking identities/revisions must equal a fresh parse of the exact
same stored bytes and parse options.

Tracking continuity is additionally constrained:

- surviving logical nodes preserve `MarkupID` where the continuity rules
  permit;
- deleted identities never return;
- equal scoped projections preserve their trace stamps;
- changed projections advance their stamps strictly; and
- private storage/layout choices cannot affect public identity, traces, or
  the diff list.

For any final byte string, processing it as:

- one edit and one commit;
- one byte per commit;
- token-sized chunks;
- random chunks;
- append then corrective replacement; or
- human/remote/LLM-attributed edits

must produce the same final canonical AST values as fresh parsing. Producer
attribution may be external annotation; it does not select parser semantics,
identity rules, or the diff list.

## 14. Required acceptance tests

### 14.1 Single-model and API gates

1. One-shot parse and incremental commit return the same self-contained
   `Document` type.
2. `Commit` contains exactly `document` and `delta`; bindings expose no legacy
   four-array `Delta` type or compatibility view.
3. Public API audits reject `added`, `removed`, `changed`, `bubbled`,
   per-family change iterators, a remap side channel, a consumer registry,
   route/interest/target/contract/acknowledgement types, and compatibility
   accessors.
4. There is no reactive/mirror AST, alternate root, or second field truth.
5. Retained documents remain readable after later commits and session close,
   with no materialization step.
6. `Delta` has exactly the four members of section 9, `Diff` has exactly
   two, and `DiffPart` has exactly six variants. Audits reject a lifecycle
   tag, a parent member, a position member, a per-field address, a schema
   member, a separate ordered-entry API, and any parameterized `DiffPart`.
7. A document containing embed and cross-link occurrences parses identically
   whether or not the targets exist or are readable, and the parse performs no
   target read: with every target deleted, the canonical dump is byte-identical
   and `diffs` is empty. No public type exposes a merged, expanded, or
   proxied subtree, and no field on an `Embed` or `CrossLink` node is derived
   from its target (3.1).
8. A complete application integrates by handing `Commit.document` to a
   reactive framework and never touching `Delta`; no binding API requires a
   delta to obtain, retain, walk, or compare a document. A second complete
   application maintains its own mutable state entirely from `diffs` and
   never re-derives. Every binding ships and documents both, and section 14.6
   proves they reach the same state.

### 14.2 Identity and trace gates

1. Prefix insertion preserves every eligible old suffix `MarkupID`.
2. Delete/reinsert allocates fresh node and auxiliary identities.
3. Duplicate-equal sibling matching is deterministic and cannot let a new
   sibling steal an old survivor's identity.
4. Local-only, subtree-only, text-only, projection-only, edge-only, and
   relation-only changes advance exactly their specified stamps.
5. A -> B -> A advances the affected stamps strictly without identity reuse.
6. Canonical no-op reuses the exact document and revisions.
7. Old node views resolve only through their retained old document; a
   foreign-domain identity traps.

### 14.3 Source and storage gates

1. Arbitrary legal stored-byte edits remain fresh-parse equivalent under the
   frozen decode/recovery profile.
2. Stored-byte, scalar, UTF-16, line/column, and binding-native projections
   resolve exactly for old and new retained documents.
3. `Document.scope` is `O(log n)` on a pinned large document and requires no
   whole-document pass; prefix insertion does not rewrite later nodes or
   extents.
4. Text and child collections path-copy only their changed persistent
   frontier plus inserted/removed content.
5. Long append traces avoid prefix-sum copying; tiny retained slices respect
   the declared memory amplification bound.

### 14.4 Framework-integration gates

For each binding, against a pinned document and an adjacent commit:

1. `MarkupID` is `Hashable`/`Equatable`, serializable, and usable unmodified
   as a SwiftUI `ForEach(id:)`, a Compose `key()`, and a React `key`.
2. Node equality and hashing are the two-word tuples of 2.1, allocation-free,
   and equal implies value-identical with no false negative from storage
   layout or rebalancing.
3. The core reuses every unchanged subtree in the next document, verified by
   reference at the C level. At a binding, the equality of gate 2 reports that
   subtree unchanged whether or not that binding's views carry identity at all.
4. A top-down value diff of the two documents reports exactly the nodes whose
   subtree projection differs — no false negatives against a fresh comparison,
   and no descent into a subtree whose root compared equal.
5. Retaining the previous document to diff against costs the changed frontier
   in additional live memory, not a second tree.
6. A virtualized container holding `V` visible rows performs `O(V)`
   comparisons per commit, independent of document size.
7. A commit on one session produces no commit, no delta, and no changed value
   in any other session's document. A host document holding an `Embed`
   occurrence is reference-identical across any number of target commits, so
   nothing in the host can invalidate a view (3.1).

### 14.5 Delta membership gates

For a pinned large document:

1. A prefix insertion emits a constant number of entries, none of which names
   a suffix node, and no entry has a coordinate-shift meaning.
2. A single deep field edit emits its own entry plus exactly one
   `DESCENDANT`-only entry per enclosing container, and nothing else. Entries
   are in postorder, so each container follows the child that dirtied it.
3. Every emitted entry's parts are exactly the parts that differ between the
   two documents, verified against a fresh comparison.
4. Private compaction, rebalancing, and interning emit an empty delta.
5. A canonical no-op emits an empty delta and reuses the document.
6. Entries are unique per `MarkupID`, and ordered as 9.5 requires: retired
   entries first in `before` postorder, then live entries in `after` postorder.
   A retired identity has no position in `after` at all — `Document.node`
   resolves it only in `before` — so a single after-order would be undefined
   for every deletion.
7. A definition or footnote flip emits `RELATIONS` on exactly the identities
   whose answers changed, and the collection work is proportional to that set,
   not to the reference population.
8. A scalar-field edit on a container with `W` children emits `VALUE` and not
   `CHILDREN`; a consumer reprojecting strictly by parts performs `O(1)` work
   and does not rebuild the `W`-wide child list. The same holds for `TEXT` on
   a node with an `L`-byte literal.
9. Adding or removing a singular child edge (`Table.header`,
   `Directive.label`) emits `VALUE` on the owner, never `CHILDREN`.
10. `DESCENDANT` never appears alone on a node whose own value changed, and
    never accompanies a retired entry, which has no parts at all. A *created*
    container does carry it, because 9.1 gives a created node every part it
    has and a container's projection includes what is below it; forbidding it
    there would contradict the membership law. One forward pass over `diffs`
    rebuilds a parent-linked value tree with no ancestor walk, no sort, and no
    second traversal.
11. Removing one grandchild of a container with `W` children emits
    `DESCENDANT` on that container in work independent of `W`, and an edit
    whose normalized result changes no projection publishes nothing at all —
    not an empty-parts entry on a live node, and not a spine (11.1).

### 14.6 Delta application gates

For randomized edit traces against a randomized consumer projection:

1. Applying the delta yields state equal to a fresh projection of
   `Commit.document`.
2. Concatenating any run of adjacent deltas and applying once yields the same
   state as applying them in sequence, including A -> B -> A.
3. Dropping any number of deltas and rebuilding from the latest document
   yields the same state as sequential application.
4. A delta whose `before` does not match the consumer's base is detected by
   that comparison alone and resolved by rebuild.
5. A consumer that materializes absolute coordinates for `K` nodes and remaps
   from `edits` matches a fresh `Document.scope` for all `K`.
6. A consumer that resolves coordinates on demand performs zero work for a
   pure prefix insertion elsewhere.

### 14.7 Complexity gates

Pinned large-document traces report:

- `|diffs|`, entries by kind, and parts by kind;
- persistent nodes/bytes copied by the parser;
- reparsed regions and identity-matching frontier;
- relation index probes and affected units;
- delta application work, separated into the `O(|diffs|)` term and the
  consumer's own projection work; and
- rebuild cost, labeled and measured separately.

`|diffs|` and delta application must be independent of unrelated document
nodes. Framework-diff traces are reported separately and measured against
11.3, never against `|diffs|`.

### 14.8 Streaming gates

1. Run human typing, paste, IME, remote, and LLM chunk traces through only
   ordinary byte edits and `commit`.
2. Randomize chunk boundaries and commit cadence.
3. Compare every final document with a fresh parse.
4. Compare consumer state with a fresh projection under applied deltas,
   concatenated deltas, and discarded deltas.
5. Audit public types and branches to prove there is no streaming-only
   semantic or update path.

## 15. Type index

Every public type this contract names, with the section that defines it.
Types marked `canonical-ast.md` are used here unchanged unless the cited
section says otherwise. `byte` and `integer` are primitives.

| Type | What it is | Defined |
| --- | --- | --- |
| `CanonicalText` | a node's text field: canonical UTF-8 plus its map back to source | 6.1 |
| `ChildList` | a node's one list-valued child edge | 6.2 |
| `ChildOrdinal` | zero-based position within one child list; a non-negative integer | here |
| `ChildRole` | names a node's list-valued child edge: `content`, `items`, `rows`, or `cells` | here |
| `Commit` | what `Session.commit()` returns: `{document, delta}` | 2 |
| `CoordinateProfile` | closed selector for the space a `Scope` resolves in | 7.2 |
| `Delta` | the difference between two documents, at both of a document's levels | 9 |
| `Diff` | one node whose projection differs, plus which parts differ | 9 |
| `DiffPart` | closed vocabulary of which part of a node differs | 9.1 |
| `DiffParts` | a set of `DiffPart`; a six-flag bitmask | 9.1 |
| `Document` | one immutable parsed unit; the AST's only public root | 4 |
| `DocumentDomain` | the opaque scope that identities and revisions live in | 5.1 |
| `MarkupRevision` | a node's revision pair: `(self, subtree)` | 4 |
| `Revision` | positive, monotonic counter within one domain; the only revision type | 5.1 |
| `DocumentTrack` | a document's version, schema, profiles, source, and relations | 4 |
| `DocumentVersion` | which published document: `(domain, revision)` | 5.1 |
| `EncodedOffset` | zero-based offset in a projected coordinate space, never storage | 7.2 |
| `Markup` | one canonical AST node | canonical-ast.md |
| `MarkupID` | stable identity of one logical node: `(domain, ordinal)` | 5.2 |
| `MarkupKind` | which canonical node kind a `Markup` is | canonical-ast.md |
| `MarkupOrdinal` | positive integer, unique within one domain, never reused | here |
| `MarkupTrack` | a node's identity, two revisions, and primary extent | 4 |
| `Offset` | zero-based byte offset; the context names which buffer | here |
| `ParseOptions` | the parse-time options that can affect AST truth | canonical-ast.md |
| `Position` | one endpoint of a `Scope`; widened per coordinate profile | canonical-ast.md, 7.2 |
| `Relations` | a document's immutable parser-owned auxiliary records | 6.3 |
| `SchemaVersion` | the frozen AST-schema version a document conforms to | here |
| `Scope` | a resolved source region: `(start: Position, end: Position)` | canonical-ast.md |
| `Session` | the single mutable owner of one document's pending source | 8.1 |
| `Source` | a document's committed bytes and how they are read | 7.1 |
| `SourceEdit` | one byte-level edit: a span to replace, and its replacement | 8.1 |
| `SourceExtent` | one source extent, identity only: `(domain, ordinal)` | 7.2 |
| `SourceProfile` | closed selector for which byte sequences may be stored | 7.1 |
| `Span` | a half-open run of bytes: `(start, end)` of `Offset` | 8.1 |
| `SpanPair` | one entry of a `TextMap`: a canonical span and its source span | 6.1 |
| `TextMap` | relates a field's canonical text to the source bytes that produced it | 6.1 |
| `Utf8Text` | canonical UTF-8 text; the value half of `CanonicalText` | here |

Every scalar above, in one place. These are the definitions; they get no
section of their own because there is nothing more to say about them.

```text
ChildOrdinal     = non-negative integer  // position within one child list
DiffParts        = set of DiffPart       // a six-flag bitmask
DocumentDomain   = opaque token          // compared for equality, never read
Revision         = positive integer      // the only revision scalar (5.3)
EncodedOffset    = non-negative integer  // in a projected coordinate space
ExtentOrdinal    = positive integer      // unique within one domain
MarkupOrdinal    = positive integer      // unique within one domain, not reused
Offset           = non-negative integer  // a byte offset into a named buffer
SchemaVersion    = positive integer      // the frozen AST-schema version
Utf8Text         = a run of valid UTF-8
```

Types this contract deliberately does **not** have, each removed for a reason
stated where it would have appeared: a lifecycle tag, a parent or position
member on `Diff` (9.4), a per-field address (9.3), a delta schema member (9),
a source identity or boundary affinity on an extent (7.2), a coordinate index
member on `Source` (7.1), and every consumer registry, route, interest,
contract, target, and acknowledgement type (3, 14.1.3).

## 16. Rollout gate

The capability ships only when:

1. `canonical-ast.md` and session contracts adopt this exact one-document
   ownership model;
2. every binding exposes equivalent identities, traces, source values,
   `Delta`, and rebuild behavior, and the consumer-facing surface in each
   binding is `Delta` plus `DiffPart` — nothing more;
3. the old four-array delta, its separate ordered-entry table, and
   whole-snapshot scope materialization are absent rather than deprecated;
4. one-shot, incremental, cache-disabled, and fresh-parse fixtures agree;
5. native and binding fault injection proves atomic publication;
6. release telemetry enforces the parser, diff-list-size, application,
   storage, and memory bounds separately; and
7. no parser package acquires rendering, layout, workspace, target-loading,
   framework, or application-model responsibilities.

Markdown Core remains independently useful as a Markdown parser whose sole
output is its canonical self-contained AST.
