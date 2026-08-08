# Incremental canonical AST contract

Status: **frozen design contract** for the next Markdown Core
self-contained-AST and stable-trace session milestone, revised 2026-08-03. No
current public API implements this target. Shipping requires every
conformance, failure-injection, and complexity gate in this document.

Companion contracts:

- `canonical-ast.md` defines Markdown Core's canonical node inventory and
  parser semantics.
- `sessions-and-deltas.md` defines the current session baseline that this
  SemVer-major contract replaces.

This document defines the edit-optimized typed projection of one canonical
Markdown CST and the exact-base delta that lets a consumer update its own
state in work proportional to what actually changed. It does not define a
renderer, layout model, document workspace, consumer state model, or second
parser output.

The words **must**, **must not**, **should**, and **may** are normative.

## 0. Unified CST prerequisite

This contract presupposes one front-end ownership chain, in the same sense
that a compiler keeps one source-faithful syntax substrate and derives later
representations from it:

```text
Source
    -> concrete interface: Unified CST
    -> semantic interface: typed AST projection (`Document` / `Markup`)
       -- Markdown Core boundary --
    -> Semantic IR (downstream)
    -> Frame Tree (downstream)
```

The unified CST is Markdown Core's only physical parser tree. It is
recoverable and lossless wherever the grammar assigns concrete spelling or
boundaries: it retains the tokens, delimiters, trivia, authored forms, and
source-backed recovery material needed to explain the parse. `Source` remains
the authority for the exact stored bytes. Every CST node and token maps back
to that source, and every AST value maps through its backing CST node rather
than reconstructing provenance after parsing.

The canonical Markdown AST described by this and `canonical-ast.md` is the
typed semantic projection of that CST. `Document` and `Markup` remain the
public semantic API, but they are read-only views over the same canonical
ownership: an AST field or typed child edge selects and interprets CST
structure; it does not belong to a separately allocated AST that must be kept
in sync. The projection does not remint a surviving semantic node's
`MarkupID`, source extent, or revision. Incremental reparsing, identity
matching, structural sharing, and source mapping therefore happen once at the
CST boundary and are inherited by every later projection.

The changed frontier is not inherited that way, and assuming it is publishes
noise. A concrete difference is only a *candidate*: the frontier a `Delta`
reports is that candidate set filtered by comparing semantic projections
(9.1), and only a node that survives the filter seeds the ancestor spine
(11.1). Trailing whitespace and a rewritten delimiter — `*x*` for `_x_`, which
`canonical-ast.md` gives no marker field — both change the CST and change no
projection, so both publish an empty `diffs` (14.5.4, 14.5.11).

This is an ownership and API refactor, not a replacement parser. The current
C-layer tree, block parser, inline delimiter engine, extension reducers,
incremental restart planning, and adoption strategy remain the parsing
mechanism. The C-layer node ownership is extended into the unified CST by
recording concrete boundaries, tokens, trivia, and recovery during those same
passes. The `semantic` interface is a zero-copy typed view over that result;
it must not run a second parser, walk the complete CST to construct an AST, or
allocate one semantic object per node merely to expose the existing
`Document` and `Markup` shape.

Concrete fidelity does not imply that every punctuation token is a full
`Markup`-sized tree record. Syntax-only tokens and trivia should use compact
source-backed records or implicit source gaps, and carry only the identity,
kind, flags, and extent their concrete API requires. They do not need semantic
fields, answer slots, child pointers, or `MarkupTrack`. A full semantic-node
allocation for every delimiter or trivia run requires separate benchmark and
memory evidence; it is not the default representation this contract permits.

Two layout rules keep the concrete material out of every bound that depends on
document size. They are normative because the obvious representation — one
interleaved child sequence per node, with offsets into the document — violates
both, and no complexity gate below survives it.

- **Concrete offsets are relative to the grammar-owned region that gets
  reparsed as a unit, never to the document.** A token, trivia run, or
  recovery record therefore never becomes a leaf of a document-wide structure,
  and an edit elsewhere can never touch one. This is what keeps extent
  resolution (7.2) answering over the same material it answered over before the
  CST existed.
- **A container's typed semantic child edge stays separately addressable from
  its concrete child order.** Projecting that edge is proportional to the edge,
  not to the container's token count, which is what 11.1 requires of ordinary
  traversal and what 9.4 requires of `Document.index`; a concrete
  ordinal is not a `ChildOrdinal`. The persistent sequence 6.2 requires is that
  typed edge, not the concrete order. Recovering the concrete order by merging
  on those relative offsets — paid only by the `concrete` interface — is the
  expected way to keep both.

Under these two rules the CST is a memory trade and not a complexity one: it
multiplies the record count inside a reparsed region by a small constant and
enters no other term.

The CST is concrete syntax with exactly one document-wide input: the set of
labels the document defines. `canonical-ast.md` makes an undefined `[^x]`
`Text`, and 6.3 makes a link reference exist only where its definition does,
so whether a run of bytes is one `Text` or a reference node is not a local
question. The definition set is therefore established before the CST is built
(6.3, step 0) and the CST is built under it. The alternative — a
definition-agnostic CST whose projection merges sibling runs into a `Text`
node no single CST node backs — is forbidden: such a node has nowhere to carry
its `MarkupID`, `SourceExtent`, or revision stamps, and it contradicts the
rule above that every AST value maps through its backing CST node. Nothing
else about the CST depends on a semantic relation.

Markdown Core exposes those two views through two public interfaces over the
same immutable published-document ownership:

- `concrete` exposes the unified CST: `ConcreteNode`s and `ConcreteToken`s in
  source order, delimiters and trivia, `MissingToken`, `UnexpectedToken` and
  `ErrorRegion`, child traversal, and source mapping. A materialized concrete
  node or token carries a `ConcreteID`; a trivia run left as an implicit source
  gap has no record and therefore no identity, and the interface must not
  pretend otherwise by minting one on demand. `ConcreteID` never appears in a
  semantic value, a parser answer, or a `Delta`.
- `semantic` accepts that concrete tree and exposes `Document` and `Markup` as
  its typed semantic projection: canonical kinds, typed fields and edges,
  canonical text, and parser-defined answers. Syntax-only tokens, trivia, and
  recovery detail remain reachable through `concrete`; they are not copied
  into a parallel semantic tree.

Conceptually, independent of a binding's naming conventions:

```text
concrete.parse(source, options) -> ConcreteTree
semantic(ConcreteTree)          -> Document
Document.node(MarkupID)         -> Markup
Document.concrete               -> ConcreteTree
```

The direction and ownership are normative even where a binding packages the
entry points differently: `semantic` cannot construct a `Document` without
its backing concrete tree, and `concrete` must never reconstruct a CST from
`Document` or `Markup`. A `Document` retains the exact immutable
published-document owner and CST revision that resolve all of its projected
values.

`Document.concrete` is how the incremental path reaches that owner, and it is
required rather than optional. Without it the `concrete` interface exists only
for one-shot parsing: `session.commit()` returns `Commit {document, delta}`,
and 2 forbids adding the tree as a third member, so a consumer of an
incrementally maintained document would have no legal way to reach the
interface gate 14.1.9 audits. It returns the retained owner, never a second
tree or a reconstruction, so calling it allocates nothing and advances no
trace.

Recovery follows the boundary the grammar can actually prove:

- Core Markdown constructs with open, context-sensitive matching continue to
  use literal fallback. An unmatched emphasis, link, list-continuation, or
  similar candidate remains concrete text/token content; the parser must not
  guess an error span, missing closer, or intended structure.
- A bounded syntax island may recover structurally only after its grammar's
  commit point. Formula, directive, footnote, cross-link, and embedded-content
  forms have explicit openers and a local closing or termination rule; once
  committed, their CST may contain `MissingToken`, `UnexpectedToken`, and
  `ErrorRegion` nodes inside that proven boundary. Before the commit point the
  same bytes use literal fallback. Recovery must not consume an unrelated
  following Markdown region merely to complete an island.

Syntax and semantic diagnostics are different products of different layers.
A syntax diagnostic explains CST recognition or bounded recovery — what token
was absent or unexpected and which source region was recovered. A semantic
diagnostic explains a well-formed projected construct or a relation between
constructs — for example an invalid semantic combination, a duplicate
definition, or a resolution result. Semantic analysis must not rewrite CST
recovery, and a syntax recovery node must not acquire semantic meaning merely
because the AST hides or summarizes it.

The project's downstream compatibility target is that adapters can construct
`Semantic IR` and then `Frame Tree` from this stable AST projection without
reparsing source or depending on private parser state. Markdown Core itself
does not define, return, cache, incrementally maintain, or own either output;
its semantic API ends at `Document`, `Markup`, parser-defined answers, and
diagnostics. The supported adapter boundary is the `semantic` interface alone:
an IR or frame adapter must not require concrete tokens, recovery nodes, or
private CST access. `Semantic IR` may resolve, normalize, and combine the
semantic values for a downstream composition or rendering policy, and `Frame
Tree` may then express layout and presentation. Neither feeds back into CST or
AST identity, and neither can become a second canonical Markdown tree. This
ownership chain is a prerequisite to every identity, delta, source, and
complexity rule below: there is one canonical tree and multiple derived views,
never a synchronized CST/AST pair.

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
| Absolute source position is a whole-snapshot materialization | Absolute position is a query against stable extents; a position-only shift produces no diff entry at all |
| Footnote and reference indexes are live-session queries | One `Document` pins persistent semantic relation indexes and derives every answer by `MarkupID` |
| The initial empty session may use revision zero | Every public identity and revision is positive; zero is invalid |

`canonical-ast.md`, `sessions-and-deltas.md`, public headers, bindings,
fixtures, and examples must adopt this contract atomically. A package cannot
advertise this capability while exposing the superseded four-array API.

## 2. One parser model, two public interfaces

Markdown Core has exactly one parser-owned model with the `concrete` and
`semantic` interfaces of section 0. The only public semantic parser output is
the typed canonical Markdown AST projection rooted at `Document`; Semantic IR
and Frame Tree are downstream consumers, not additional parser outputs.

```text
one-shot:
    stored Markdown bytes + ParseOptions
        -> concrete tree
        -> semantic(concrete tree): Document

incremental:
    Session + ordinary byte edits
        -> committed concrete tree
        -> session.commit(): Commit

Commit {
    Document document  // semantic projection retaining that concrete tree
    Delta delta
}
```

`Commit.document` is the complete correctness result. It is the same public
AST type returned by one-shot parsing. Its retained concrete owner is exposed
through the `concrete` interface; it is not a third `Commit` member or a
separately published tree. `Commit.delta` is an immutable exact-base
description of what changed between the session's immediately preceding
published document and `Commit.document`.

A consumer may ignore or discard `Delta` and derive all of its state from
`Commit.document`. Doing so cannot change parser output, identity, revisions,
or structural sharing.

The one backing immutable published-document owner contains the unified CST
plus non-tree persistent indexes derived from it. `Document` retains that
owner and projects:

- canonical node kinds, scalar fields, text fields, and typed child edges;
- stable node and parser-owned auxiliary identities;
- node-local and subtree trace stamps;
- exact stored source bytes and stable source extents;
- deterministic source-coordinate and raw-to-canonical-text projections;
- definition, reference, resolution, footnote, link, and embed facts; and
- immutable indexes required to resolve those AST values without a session.

Tracking fields are part of the canonical syntax ownership and exposed
through the AST projection. They describe identity, continuity, source, and
semantic equality of the same canonical values. They are not a second tree,
reactive snapshot, subscriber graph, or consumer model.

There must not be:

- a `ReactiveDocument`, `ReactiveSnapshot`, mirror tree, or independently
  owned projection root beside `Document`;
- separately allocated CST and AST node hierarchies joined by synchronization,
  remapping, or duplicated identity;
- a nested AST plus a separately authoritative normalized AST;
- a compatibility `Document` reconstructed from another public model;
- two independently versioned copies of one field, text, edge, source extent,
  or parser answer;
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
test assertion. It reads the entries, resolves `Document.scope` against the
document it was handed, and takes the replaced byte spans from `Delta.edits`
(9.2). It is not handed a predecessor to resolve against: a session publishes
one document, and a consumer that wants to compare against what it rendered
last keeps what it rendered last (4.2).

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
    -> unified concrete syntax
    -> canonical Markdown AST projection and parser-defined semantics
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
syntax or its host's parser answers, so an embed is an unresolved external
reference, and resolving it can wait until the composer holds every unit it
needs. That is why a model borrowed from those systems does not transfer here,
and it is the only property this boundary actually rests on: were Markdown
ever given a construct through which a target changed its host's parse, the
boundary would have to move, not be worked around.

Composition is cheap to own because the parser hands it working primitives:
stable `MarkupID`s to key its own structures by (5.2); immutable
self-contained documents, so it can hold many units at once and no unit's
commit can invalidate another's (4.2, 14.4.7) — many separate documents, which
needs no sharing between them; and
per-document parser answers, so it knows exactly what remains unresolved
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
subtrees instead of `k` materialized copies. And parser answers are
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
    DocumentVersion version
    SchemaVersion   schema
    ParseOptions    options
    Source          source
}

Markup {
    MarkupKind kind
    MarkupTrack track
    typed Scalar fields
    typed Utf8Text fields
    typed optional/singular child fields
    at most one typed [Markup] child list
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

This is the semantic shape projected from the unified CST, not a second
mandatory memory layout. Bindings may expose properties, methods, protocols,
interfaces, or borrowed views, but every value has exactly one owner and one
meaning. No binding or core implementation may materialize this shape as an
independently authoritative tree that must be synchronized with the CST.

The document's four members are direct rather than grouped behind one track
member. `MarkupTrack` is grouped because it is replicated on every node and
capped below; a document carries its members once, so there is nothing to
fence, and `version` — which every consumer reads for the one comparison of
9.6 — would otherwise need a bypass accessor to avoid a walk. The parser's
answer store is not among them: it is private storage behind the queries of
4.1, for the reason 6.3 gives.

`Document` is itself a `Markup`: it carries a `MarkupID`, and it appears in
`diffs` like any other node. A top-level insertion or removal is `CHILDREN`
on the document, which is the only node that owns that sequence, and the
ancestor spine of 9.1 terminates there. Its `revision.subtree` is not a
restatement of `DocumentVersion`: a commit that changes bytes without
changing the AST (7.1) advances the version and leaves the root revision
alone.

`node.track.identity` is the sole public node identity. `track.revision` is
always the pair and never a single number: no scalar member conflates local
with subtree change, which is the ambiguity section 1 removes. No wrapper
identity is layered around `MarkupID`.

`MarkupTrack` is deliberately small — an identity, a revision pair, and an
extent — and no more may be added.
Per-field, per-text, per-edge, per-source, and per-answer revision stamps
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

A node view belongs to the exact `Document` it came from and cannot be passed
to a later one as an exact view merely because its `MarkupID` survived. A
caller resolves logical continuity explicitly:

```text
Document.version          -> DocumentVersion
Document.node(MarkupID)   -> Optional<Markup>
Document.parent(MarkupID) -> Optional<MarkupID>
Document.index(MarkupID)  -> Optional<ChildOrdinal>
Document.concrete         -> ConcreteTree
```

A returned `Markup` *is* the document-bound view; there is no separate view
type layered around it. The accessor keeps the name `node` because a caller
reads it as "the node for this id", and because a second `markup` spelling
would collide with the member it is passed.

`Document.version` is a direct member for the reason given in 4: every
consumer reads it for the one comparison of 9.6.

`none` means the identity is not live in this document. Passing an identity
from another domain is a programmer error and traps; it is not a result
value. There is no `Checked<Optional<...>>` double wrapping.

Every parser answer (6.3) is a query on the `semantic` interface, addressed by
the `MarkupID` whose answer it is:

```text
Document.footnote(MarkupID)   -> Optional<FootnoteAnswer>
Document.footnotes()          -> [MarkupID]
Document.references(MarkupID) -> [MarkupID]
Document.resolution(MarkupID) -> Optional<Resolution>
```

The answer types belong to `sessions-and-deltas.md` (6.3); what this contract
fixes is where the queries live. They are members of the semantic document and
not of the concrete node API or session, because 4.2 requires a retained
document to answer them after its session closes, and because the detached
projection of 10 releases the session while keeping the answers. A consumer
that learns from an `ANSWERS` part which identities re-answer must be able to
ask *those* identities on the document it already holds. The query resolves
through that document's immutable owner; it never consults a newer session
revision or reparses its CST.

The tree need not copy every descendant wrapper into each parent:

- a child edge semantically contains ordered child nodes;
- private persistent storage may retain stable `MarkupID` references;
- iteration returns views bound to the owning `Document`; and
- wrapper or pointer identity is never logical node identity.

When only a descendant field changes, an implementation may share every
unchanged ancestor's local record and child sequence. A persistent aggregate
trace index may advance `revision.subtree` without copying all ancestor
payloads.

### 4.2 Self-contained documents

Every published `Document` must be:

- immutable;
- self-contained when returned;
- safe for concurrent reads according to its binding's value contract;
- usable after the session closes; and
- able to resolve every live `MarkupID`, parser-owned site, extent, text,
  child edge, parser answer, and absolute source coordinate it exposes.

Lazy indexes are allowed only when their computation depends entirely on
immutable values owned by that `Document`. It must never call into a live
session, consult a session's current source, or fail because a session
advanced. There is no `materialize()` step.

**Two clauses are removed: that a document be independent of later session
commits, and that it be structurally shareable with adjacent immutable
revisions.**

The first is unreachable and the second is what it was built for. A session
hands out one document — `Document.current`, the view it reuses in place at
every commit — so a caller cannot hold a predecessor to be independent OF.
The one document a caller does own outright comes from a one-shot parse, which
takes the substrate with it and whose session is already gone; that is the
"usable after the session closes" bullet above, and it is exercised. Bindings
that want a revision to outlive its commit take a value copy, which is what
they do: a decoded snapshot answers from its own values and refuses a node
from another revision.

Structural sharing was there to make the unreachable clause cheap. It bought
persistence in the source and in whatever index resolves 7.2, with the
refcounting, path copying and retained-bytes bounds that go with it, for a
predecessor no consumer can name. What survives is what a consumer can hold:
one immutable document at a time, valid for as long as it is held.

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

New siblings cannot steal identities from surviving old siblings. Matching is
therefore anchored to the edit rather than to content, and the rule is fixed
here because it decides the observable `MarkupID` stream and with it every
`diffs` list a fixture will ever freeze:

1. **Nothing outside a reparsed region is matched at all.** Those nodes are
   retained, not re-identified (4.1).
2. **Within a region, identity never crosses a parent or a kind.** A node
   matches only a former child of the same parent with the same `MarkupKind`;
   a paragraph that becomes a heading is a retirement and a creation.
3. **Nodes the edit does not overlap are matched positionally.** A child whose
   old source lies wholly before the normalized edit span matches by its
   ordinal from the start of the sequence; one whose old source lies wholly
   after it matches by its ordinal from the end. These are the stable old
   witnesses, and the session has them at every commit because it normalizes
   the edit script before parsing (9.2).
4. **Only the children the edit overlaps are matched by content**, as a
   longest common subsequence over `(kind, discriminator)` with the leftmost
   pair winning each tie.
5. Unmatched new children take fresh identities; unmatched old children
   retire.

Step 3 is what step 4 cannot do alone. Given `[A, A, B]` and an insertion at
the front producing `[A, A, A, B]`, content carries nothing that distinguishes
the inserted `A` from the two survivors, so a content-only match — including
an order-preserving one — hands the first survivor's identity to the new node
and reports the survivor created. That is exactly the "first equal node wins"
failure, and the information needed to avoid it is in the edit span, not in
the tree.

The result is a pure function of the old child sequence, the new child
sequence, and the normalized edit span. It must not depend on hash order,
arena addresses, allocation order, or traversal order, which is what 14.2.3
tests.

The `domain` is what makes the pair usable as a host-wide key. A host that
parses many units at once — a chat transcript, a feed, a notebook — holds
identities from many documents in one collection, and every document's
ordinals start from the same place. Without the domain those roots collide,
and an identity-keyed view or map silently merges nodes from different units
rather than reporting an error. `MarkupOrdinal` is therefore never the
public identity on its own.

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
- parser answers addressed to this node, derived through its pinned relation
  indexes; and
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

A node's text field is a `Utf8Text`: canonical UTF-8, and nothing beside it.

Scalar fields are plain typed values. Optional `none` is a real field value,
not an absent node; a presence change is an ordinary field change.

**A text field carries no map back to the source bytes that produced it.**
An earlier revision of this section paired every text field with a
`TextMap` — ascending span pairs relating each stretch of canonical text to
the source it was decoded from — and spent this section justifying the
node-relative spans, the "equal length is not the test" rule for interior
addressing, and the ban on storing content in an entry. All of it is removed,
and the reason is that no consumer ever asked to see the bytes behind decoded
text.

What consumers do ask for is the reverse lookup: from a node, the input row
and column it was written at. That is `Document.scope` (7.2), it is a
projection from the stored-byte substrate, and it needs no correspondence
between a decoded character and the bytes that produced it. The requirement
that put a CST under this contract at all is LOCAL DIAGNOSTICS on the
constructs that can be malformed — a directive's header, an embed's
reference — and a diagnostic names a source span. It never names a decoded
character's provenance.

Three facts make the map vacuous where it would have been read:

- `CrossLink.reference` and `Embed.reference` are source-faithful by
  `canonical-ast.md`, and `FootnoteReference.label` is written as in source.
  For the constructs the diagnostics requirement names, the map is the
  identity;
- of the kinds that carried a text field, all but `Text` hold their bytes
  verbatim or reassembled without decoding — a code block, a raw HTML node
  and a formula reproduce what was authored. Only `Text` resolves entities,
  escapes and smart punctuation, and a diagnostic never points at a `Text`;
- twenty-seven of the thirty-four kinds already live with no map at all.
  `Link.destination`, `CodeBlock.info`, every `label` and `name` and
  `attributes` are decoded scalars whose source span is the sub-node extent
  7.2 defers, and the answer there has always been that a consumer resolves
  the owning node's extent and searches within it. The map drew a line
  between seven kinds and twenty-seven that no requirement drew.

The cost of keeping it was not the storage. `TEXT_MAP` was a projection part
compared by value (9.1), so it entered every revision computation, and its
spans had to be node-relative for the sole reason that absolute ones would
make a prefix insertion differ on every later text field in the document —
the shape 7.3 exists to forbid. That is a second coordinate space, maintained
per node, tied to a first. This contract removed exactly that shape from node
positions; it does not reintroduce it one layer down for a reader that does
not exist.

A consumer that one day needs the correspondence gets it the way the other
twenty-seven kinds already do, and if that proves insufficient the answer is
the sub-node extent 7.2 defers — one mechanism, for all thirty-four kinds,
driven by a stated need.

Text storage must support localized replacement, and repeated tail appends
must not copy the complete prefix each commit — a streaming feed is the whole
reason the second sentence is here, and geometric growth is enough for it.

**Two further requirements are removed: that the storage support PERSISTENT
slicing, and that a tiny retained slice not retain an unbounded source buffer
beyond a documented amplification limit.** The second is a hazard only of the
first: it describes a slice that is a window into a shared immutable buffer,
which is a thing only a persistent store has. And the first was written for
the predecessor-reading document 4.2 removed. A store that owns its bytes
outright has no slice to amplify.

### 6.2 Typed child edges

A node's list-valued child edge is an ordered sequence of `Markup`, reached
through the field the canonical AST schema names for that kind — `content`,
`items`, `rows`, or `cells`. No canonical node has two, so the sequence needs
no role tag and this contract adds no wrapper type around it: the field
already names the edge, and 9.3 depends on that to keep `CHILDREN`
unparameterized. Child kind legality remains statically or dynamically
checked by the canonical AST schema.

The child sequence must be persistent: localized insert/remove/replace
operations path-copy only the affected persistent frontier plus inserted or
removed members. A copy-on-write array costs `O(width)` per commit and shares
nothing, which neither the bounds of 9.1 nor the unchanged-ancestor sharing of
4.1 survives. Dense positions and private order labels do not escape as stable
identity.

### 6.3 Parser answers

Parser answers belong to the `semantic` interface, but they are neither
concrete syntax nor fields stored in a second AST. They are relations derived
from the complete typed projection: which definition a reference resolves to,
which of several same-label definitions wins, a footnote's number and ordinal,
and where a cross-link or embed occurs and how the parser classified it. A
source-faithful node stores what was written; a fact that holds *between*
nodes, or that is computed over the whole document, has no node field to live
at. A consumer asks for that fact by `MarkupID` through the `Document` queries
of 4.1.

The immutable published-document owner therefore has one tree and one class
of derived, non-tree storage:

```text
published document owner
├── Source
├── Unified CST                           // the only tree
└── persistent semantic relation indexes // derived accelerators

concrete -> Unified CST
semantic -> Document / Markup + answer queries
```

A relation index is logically a pure derived function of the unified CST,
schema, and parse options through the typed semantic projection. The index
pins sites, normalized keys, buckets, winners, document order, and reverse
edges needed to answer queries efficiently; the answer returned for a
`MarkupID` is a query result over that revision, not a materialized per-node
answer snapshot.

A relation index is prepared at publication for every document a session can
commit from. That is not a preference: publication compares old and new
observable answers to emit the `ANSWERS` parts (step 4 below), which needs
both sides materialized at commit time, so a document that becomes a delta's
`before` was never free to defer. A one-shot document that no session will
commit from may derive its indexes lazily from its immutable owner-held
values. Individual query results may be memoized in either case. None of these
choices is observable: enabling, disabling, filling, compacting, or
rebalancing an index or memo cache cannot change a `Document`, revision, or
`Delta`. A lazy query must not consult the live session, a newer document,
private mutable parser state, or external content.

The session may keep mutable site lists, label interning, inverted indexes,
and other derivation machinery used to prepare the next commit. Those are
transaction-local caches, not published relation truth and not a query target
for any retained `Document`. The published relation-index generation uses
stable semantic identities rather than raw pointers whose target is replaced
by reparsing. A sequence-preserving commit whose relations do not change
shares the exact persistent index roots even when the session patches its
private site pointers. When relations do change, the new generation
path-copies only the affected sites, buckets, and ordered runs. It must never
copy or persist one answer value per semantic node.

Only projected semantic nodes participate. Public answer keys and values use
`MarkupID`; concrete token identities, trivia, `MissingToken`,
`UnexpectedToken`, and `ErrorRegion` never escape through an answer. A
recovered syntax island can receive semantic answers only when its CST
projects a typed `Markup` node. Its concrete recovery detail remains on the
`concrete` interface and in syntax diagnostics.

The relation indexes contain:

- link/reference definitions and occurrences;
- normalized definition buckets and deterministic winners;
- reference resolutions, and on the definition side their absence: a
  `ReferenceDefinition` or `FootnoteDefinition` nothing refers to. A negative
  resolution has no node on the reference side to carry it — a link reference
  exists only where its definition does, because a bare bracket with no
  definition is prose (amended 2026-08-02, when the two reference models were
  unified; `CrossLink` and `Embed` are the exception and keep their nodes,
  since their targets live outside the document and resolving them is the
  consumer's);
- footnote definitions, references, ordering, and labels;
- cross-link and embed occurrences; and
- the forward, reverse, and document-order indexes required to answer those
  relations without scanning the CST.

An answer addressed to one semantic node contributes to that node's
`ANSWERS` projection. An ordered document-wide answer such as
`Document.footnotes()` contributes to the root `Document` node's `ANSWERS`
projection. This gives every observable answer exactly one diff address; no
answer-store identity or separate revision domain is required.

Their record types and answer types are defined in `sessions-and-deltas.md`,
which section 16 requires to move from the current session scope to this
contract's immutable published-document ownership. A one-shot document and an
incrementally committed document must expose the same answers, and a retained
old `Document` must keep answering its old values after later commits and
session close (4.2). Each parser-owned site has a stable typed identity.
Negative results are explicit immutable values so a later definition
insertion can be discovered without pretending "nothing was read."

The public surface names no type for these indexes. The queries of 4.1 are the
capability, and how the immutable owner accelerates them is private
structure (2) — the same split 7.1 makes when it keeps the coordinate index
out of `Source`. The `concrete` interface exposes neither
the relation indexes nor their storage. A public store member would be readable
by no one in any case: every answer is reached by semantic identity, and
nothing can be projected from the store itself. It carries no separate public
revision scope either, because a consumer that projects an answer holds it
against the `MarkupID` it belongs to, and that identity is what the diff entry
names.

Grouping the relation indexes behind one private handle on the
published-document owner
is the expected implementation, and this contract does not constrain the
layout inside it. They share a lifetime, a refcount, and one share-or-copy
decision per commit; the record kinds differ enough — a normalized label map
with duplicate buckets and a deterministic winner, a first-use ordering with
per-reference ordinals, and ordered occurrence and reverse-reference lists —
that each keeps its own structure within the bundle.

Those structures must be persistent across adjacent revisions, for the reason
6.1 and 6.2 give for text and child sequences. A commit that changes no
relation structure of a given kind shares it with its predecessor outright;
one that changes a relation path-copies only the affected part. Rebuilding an
index per commit would make every commit `O(document)` in the relation
population and defeat 11.1 — and a streamed document would pay that once per
chunk.

A relation change may change one or more query results. The commit compares
old and new answers only for identities reached through the affected relation
frontier, and emits `ANSWERS` on every `Markup` whose observable result
changed. It does not compare or snapshot every answer in the document. The
parser-owned inverted indexes make that work proportional to the affected
nodes rather than the number that could have been affected.

Publication is ordered and transactional:

0. update the document's two definition label sets — footnote labels and
   link-reference labels, which normalize the same way but do not share a key
   space — from the reparsed region, and, through the mention index, name the
   further regions whose bracketed forms change between prose and a
   `FootnoteReference`, `LinkReference`, or `ImageReference` because a label
   appeared or disappeared;
1. build the candidate unified CST for those regions under that definition set
   and establish surviving semantic `MarkupID`s;
2. derive the changed semantic-site frontier from the old and new projections;
3. persistently update only affected relation sites, buckets, winners, order
   indexes, and reverse indexes;
4. compare old and new observable answers, add `ANSWERS` to each affected
   `MarkupID` (including the root for document-wide answers), and advance the
   corresponding local and subtree traces; and
5. publish the CST owner, relation-index roots, traces, and `Delta` as one
   immutable document revision observed through the semantic projection, or
   publish none of them on failure.

Step 0 exists because step 1 would otherwise need step 3's answer: whether
`[^x]` is a `FootnoteReference` or prose is decided by a label the document
may define anywhere (§0). It is not a document scan. The definition set is
maintained incrementally, so the step costs the reparsed region plus one
lookup per label whose defined-ness flipped; the mention index is keyed by
mention rather than by resolution, which is what lets it name blocks whose
brackets were prose until now. Both are independent of document size.

The rest of the order is not an implementation pipeline that creates
intermediate public trees. It defines the dependency and atomicity law. A
fresh parse computes the same final answer values from the final CST, while an
incremental commit may reuse the unaffected persistent index structures.

Parser answers are values, not diagnostics. Syntax diagnostics read concrete
recovery directly. Semantic diagnostics may use projected values and parser
answers, but a diagnostic record is not an answer, cannot change resolution or
numbering, and cannot cause an `ANSWERS` diff unless an answer value itself
changed.

These records describe Markdown parsing only. An embed occurrence contains
the authored target and parser-defined classification; it does not load or
parse the target.

## 7. Exact source model

### 7.1 Stored bytes

```text
Source {
    [byte] content
}
```

Whatever index makes 7.2 resolve is private storage, not a member of `Source`:
the capability is public, the structure is not (2). 7.2 no longer names one,
and no longer states a bound over it.

The source owns the exact committed stored bytes, including bytes that are not
valid UTF-8 when the selected source profile permits them. Canonical Markdown
decoding and recovery are deterministic functions of those bytes and the
frozen profile.

**UTF-8 IS ASSUMED AND NEVER VALIDATED.** It is an obligation of the caller,
not a precondition this engine enforces. The engine does not scan the input,
does not replace an invalid sequence, and does not reject one. The substrate
stores what it is given, byte for byte, and hands the same bytes back.

That is a decision about WHERE the question is answered, and the answer is
"not here". Validating would mean rejecting, and rejecting a document because
of one byte in it is a policy this engine has no standing to set. Replacing
would mean a LOSSY PARSE, which is worse than either: the result is not a
degraded document but a DIFFERENT one, and it can be a plausible one — a GBK
document read as UTF-8 has pairs that are invalid, and pairs like `0xC4 0xA1`
that are accidentally well-formed and decode to a wrong character with nothing
to report. Neither is the engine's call. What the engine owes is that the
bytes it was handed survive it.

**The guarantee is stated over legal input only: valid UTF-8 in, valid UTF-8
out.** Input that is not UTF-8 has no defined behaviour here — not a
guaranteed degradation, not a documented failure mode, not a supported
encoding. It is out of scope, and every clause that tried to describe what
happens to it is void.

**`SourceProfile` is removed with them.** A source stored bytes under one of
two profiles, `STRICT_UTF8` or `PERMISSIVE_BYTES`, and the profile selected
whether an edit's neighbourhood was validated and a violation failed the
commit. With no validation there is one behaviour under two names, and the
truncated-final-code-point exception it was built around — a well-formed
prefix at end-of-source that a later chunk completes — needs no exception to
state: a source stores what arrives, and a chunk boundary is not a fact about
the document. 8.2's rule that streaming is ordinary editing with no separate
path is now unconditional rather than conditional on a profile, which is what
that section wanted in the first place.

**U+0000 is unaffected.** A NUL is valid UTF-8; the replacement CommonMark
requires of it is a rule about canonical text, not a statement about which
bytes may be stored, and it stays.

This is not a deviation from the C engine's ancestry — it restores it. cmark
validates only on request and its default is to pass an invalid sequence
through untouched; goldmark reads bytes and assumes UTF-8 without checking.
Turning validation on unconditionally is what this project had done, without
recording why, and `UPSTREAM.md` now records the return.

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

**What resolution must satisfy is stated over the OUTPUT, and the mechanism is
not prescribed.** An extent resolves to the place its bytes are at, and a
change anywhere else in the document does not change what any other extent
resolves to. A node therefore holds no coordinate, which is the whole of 7.3
and the whole of what a consumer can observe.

An earlier revision of this section prescribed the mechanism as well: it
required resolution in `O(log n)` over the source-bearing units, and named a
persistent aggregate sequence keyed by private order-maintenance labels and
carrying subtree byte sums as the structure that satisfies it. That
requirement is removed, and the reason is that **it was never traced to a
consumer.** The property a consumer asks for is that an edit affect only the
nodes the edit relates to, and that is delivered by 9.1 — position is not a
component of `proj`, so a positional shift emits nothing whatever work
produced it. An implementation that walks the document to re-derive positions
after an edit satisfies every observable requirement in this contract; it is
merely slower, and how much slower is a question of measured milliseconds
rather than of contract.

What the prescription cost, when it was followed, is recorded in
`docs/reviews/2026-08-07-requirement-audit.md`: a document-wide per-byte
ownership partition, the sequence over it, an aggregate tree over that, the
persistence of that tree, and the label space that keys it — none of which any
stated requirement reaches.

Every `CoordinateProfile` is deterministic and schema-versioned. `NATIVE` is
data, never a callback or platform object.

### 7.3 Source movement is not a change

Numeric source positions are revision-specific query values. They are not node
identity, canonical semantic field content, or a diff entry.

A prefix insertion:

- preserves every later node's `MarkupID`;
- preserves their `revision.self` and `revision.subtree`;
- preserves parse and derived answer values;
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
which is not valid UTF-8 (7.1).

Edits name the current pending source coordinate space. A binding may expose
single-edit and batch conveniences, but they normalize to one deterministic
non-overlapping edit set before parsing. Overlap, overflow, or a stale source
base fails without publishing a partial source or AST. Bytes ending in a
truncated code point are not a failure at all: the substrate stores what it is
given (7.1), and the next chunk continues it.

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

A chunk boundary that falls inside a multi-byte character needs no case. The
substrate stores the truncated tail like any other bytes and the parse carries
it through (7.1). This is not a provisional AST: the document is an ordinary
complete document that happens to describe bytes ending mid-character, it is
exactly what a fresh parse of those same bytes produces, and it stays valid
and readable forever whether or not another chunk arrives. The completing
chunk, if there is one, is an ordinary append, and 13's chunk
equivalence covers it.

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
    VALUE | TEXT | CHILDREN | ANSWERS | DESCENDANT
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
proj(n) = (VALUE, TEXT, CHILDREN, ANSWERS, DESCENDANT)

VALUE       kind, scalar fields, singular child edges, source shape
TEXT        canonical text bytes
CHILDREN    the list-valued child edge: membership and order
ANSWERS     this node's parser answers, asked of the document (4.1)
DESCENDANT  the projections of everything below it
```

`ANSWERS` includes every node-addressed semantic query value and, on the root
`Document`, every document-wide ordered answer. It does not include concrete
recovery, syntax diagnostics, relation-index layout, or cache state. Thus a
change to `Document.footnotes()` has a stable address even if no ordinary
node field changed, while lazily filling the index emits nothing.

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
because position is not in `proj` at all. Nothing in `proj` is measured in
source coordinates, which is what makes that statement structural rather than
a property to be maintained: 6.1 removed the one part that had been.

**A node whose CST changed but whose projection did not emits nothing**, for
the same reason a compaction does: `proj` is the whole membership test, and a
respelled delimiter or a trivia edit changes no component of it. The concrete
difference narrows the work a commit must do; it is not itself the frontier
(§0, 11.1).

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
inverse: a consumer that needs the replaced bytes kept them, from the document
it was handed before this commit. It carries no node identity and describes no
consumer state.

### 9.3 Why these six parts and no more

`DiffPart` is closed, and the rule that closes it is a cost rule, not a
taxonomy:

> a part exists if and only if a consumer that ignored it would either be
> wrong, or pay more than `O(1)`.

`TEXT` is `O(length)`. `CHILDREN` is `O(width)`. `ANSWERS`
is the cost of a parser answer query (4.1). A consumer that ignored `DESCENDANT`
while materializing a parent-linked structure would be *wrong*: it would
retain an ancestor holding a stale child, and a value-diffing UI below it
would never reach the change. Everything else about a node — its kind, its
scalar fields, its singular child edges, its source shape — is `O(1)` to
reproject, so it is one part, `VALUE`, and needs no field address. Splitting
`VALUE` further would let a consumer skip `O(1)` work at the cost of a wider
vocabulary in every binding forever.

Two facts about the canonical node inventory remove the parameters:

- no canonical node has two text-valued fields, so `TEXT` needs no field
  address. This is a rule about the inventory, not an accident of it: **at
  most one field per kind is a `Utf8Text`** — the kind's content text, spelled
  `literal` — and every other string field is a plain scalar carrying decoded
  characters. Seven kinds carry one; the rest carry none and therefore never
  carry this part. Without that rule the claim is false, because `Link` and
  `Image` each pair a destination with a title and `ReferenceDefinition`
  carries both beside its label, and CommonMark resolves escapes and entities
  inside all of them. Naming the source span of any decoded field — the
  content text included — is the sub-node extent 7.2 defers; until it exists,
  a consumer that needs it resolves the owning node's extent and searches
  within it; and
- no canonical node has two list-valued child edges — `Table` pairs one
  singular `header` with one list `rows`, `DirectiveBlock` pairs one singular
  `label` with one list `content`, and every other kind has at most one of
  `content`, `items`, `rows`, or `cells` — so `CHILDREN` needs no role
  parameter.

A change to a singular child edge is a change to the owning node's local
value, and is therefore `VALUE`: relinking one child reference is `O(1)`.

One scalar in the inventory is not local: `List.tight` is false when any item
is separated from its neighbour by a blank line, so it is a fold over the item
sequence rather than a reading of the list's own bytes. Two things follow, and
both are normative. A grandchild edit that flips tightness emits `VALUE` on
the `List` and not only `DESCENDANT`, which is the one exception to the shape
14.5.11 otherwise describes. And the fold must be maintained as an aggregate
on the persistent item sequence — "some item is loose" is a monoid, so it
rides the `O(log W)` path copy 6.2 already pays — because recomputing it by
walking the items would make an `O(1)` edit cost `O(W)` and break 14.5.11's
bound for every list in the document. No other field in the inventory has this
shape; a future one that does inherits both rules.

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
resolved-link view watches for the `ANSWERS` part on the identities it
holds and re-runs the parser query (4.1) for those. Negative and empty results
are real immutable values in the document, so a later definition insertion is
discovered as an ordinary `ANSWERS` change rather than by rescanning.

**Detached projection.** A consumer may project each commit into a
self-contained value tree of its own — resolving at projection time every
parser answer it will need — and then release the document and the session
outright. What it keeps is ordinary immutable values keyed by `MarkupID`,
holding no parser reference, so it costs nothing beyond its own tree, crosses
threads and actors as a value, and outlives the parse. Nothing above that
projection can tell that a parser was ever involved: a resolved destination
and a footnote number are a string and an integer in the tree, not a call
back into anything.

This is the shape for a host that renders many independently parsed units and
edits only a few at a time — a chat transcript whose finished messages never
change again, a feed, a notebook. Only the units under active edit keep a
session; every other unit is a value. It is not a separate parser mode or a
second output: the projection consumes the same `diffs`, and rebuilding one
unit from its document remains available whenever the consumer still holds
one (9.6).

Detaching removes the retained parser, not the projection work. The tree is
still rebuilt along each commit's spine, which is the `O(changed * depth)`
term 9.1 already bounds, and a consumer that detaches gives up the option of
querying the document later — every answer it did not project is gone with
the document. Which units to detach and when is a consumer decision, and the
parser is not told.

## 11. Complexity contract

### 11.1 Parser commit cost

**The requirement is about the FRONTIER, not the work.** A commit must leave
unchanged every node the edit does not relate to: the nodes whose projection
differs are the ones `Delta` reports (9.1), and a consumer comparing values —
a pull-style UI diffing its own component tree — sees exactly those. That is
what "an edit affects only the edited part" means, and it is a statement about
what a commit PUBLISHES.

**It is not a statement about what a commit DOES.** An earlier revision of this
section required `session.commit()` to cost a bound independent of document
size, and that requirement is removed: it appeared here as an assertion, with
no consumer traced to it anywhere in this contract, and every clause below
that was justified by "what makes the bound hold" inherited the same standing.
A commit that walks the document to re-derive what it must publish satisfies
the frontier requirement exactly; it costs a walk. Whether a given walk is too
slow is a measurement, and a measurement is not a clause — what this contract
may no longer do is treat a size-dependent term as a violation on its own.

What remains genuinely required is that no commit be QUADRATIC in the document,
and that the published frontier be bounded by the edit rather than by the
document. `session.commit()` is measured independently of any consumer:

- edited stored bytes;
- reparsed grammar ownership regions;
- concrete token/trivia records created, retained, and copied;
- definition-set updates and mention-index probes (6.3, step 0);
- identity-matching frontier;
- changed canonical AST records and trace stamps;
- semantic relation-index maintenance;
- relation-index records and bytes copied;
- persistent nodes/bytes copied; and
- `Delta` construction.

The unit a localized edit reparses is the **ownership region**, and it is
defined rather than left to taste — but the reason is CORRECTNESS and not
cost. Inline syntax is non-local within a leaf, so a leaf cannot be reparsed
in fragments and still produce the tree a fresh parse would; a container's
marker material is what decides whether its children are its children at all.
The region is the smallest span that can be reparsed alone and give the same
answer. That it is also the smallest span a commit needs to touch is a
consequence, not the definition.

A region is one of:

- a leaf block — `Paragraph`, `Heading`, `CodeBlock`, `HTMLBlock`,
  `FormulaBlock`, `ThematicBreak`, `ReferenceDefinition` — together with its
  complete inline child sequence, because inline syntax is non-local within a
  leaf and cannot be reparsed in fragments;
- the inline sequence of a `TableCell` or a block `DirectiveLabel`, each of
  which owns the source backing that sequence; or
- the marker material of one `BlockQuote`, `List`, `ListItem`, `Table`,
  `TableRow`, `DirectiveBlock`, or `FootnoteDefinition` — its `>` runs, bullet
  or ordinal markers, delimiter row, fence lines, or `[^label]:` opener —
  which belongs to that container's own region and not to any child's.

`FootnoteDefinition` belongs to the third class and not the second: its
`content` is **block** content (`canonical-ast.md`), so it is a container whose
children are their own regions, and what it owns directly is the `[^label]:`
opener that introduces it. Classifying it by its content category is what
gives that opener a region to be rebuilt from; an edit to the label would
otherwise have nowhere to write its region-relative concrete records. Only
`TableCell` and a block `DirectiveLabel` own an inline sequence without being
a leaf block, which is why the second class has exactly two members.

Inline containers are never regions. `Emphasis`, `Strong`, `Strikethrough`,
`Link`, `Image`, `LinkReference`, `ImageReference`, and an inline `Directive`'s
label are materialized during the surrounding leaf's parse, so they stay inside
it rather than copying and reparsing the same bytes.

Regions nest, and a region's concrete offsets are relative to that region, so
an edit inside a paragraph rewrites the paragraph's concrete records and
nothing else while every enclosing `ListItem`, `List`, and `BlockQuote` keeps
its marker records untouched however deep the nesting runs. This is worth
having and it is not what makes anything hold: records that were rewritten
would still be correct, because a record is not part of `proj` (9.1) and
rewriting one publishes nothing.

An edit may legitimately widen from its innermost region to an enclosing one —
an edit inside an unclosed fence, a raw HTML block, or a directive region
reparses forward to the end of that construct, and an edit that changes a
block boundary reparses the neighbours that boundary joins or splits. It must
not repeat a whole-document parse once per changed node.

`|diffs|` is the changed frontier plus its deduplicated ancestor spine (9.1),
bounded by `O(changed * depth)` and independent of document width and size: a
one-block insertion into a document of any size produces its own entry plus one
per enclosing container. **That is the bound this section is about**, and it is
about what is published. `|diffs|` and delta application must be independent
of unrelated document nodes.

Two things are required of ORDINARY ACCESS rather than of a commit, and they
are requirements because a consumer pays them on every read: projecting a
`Document` or `Markup` value allocates nothing in the core and walks no CST,
and a semantic traversal steps over no syntax-only token. Both are zero, not
bounded — they are what §0's separately addressable typed child edge buys, and
a projection that walks the concrete tree to answer a semantic question has
given that away.

A sentence here required structural edits to path-copy `O(log n)` persistent
sequence nodes. It is removed with the rest: nothing in this contract reaches a
consumer that can observe how a commit stores what it publishes, and 7.2 no
longer prescribes a persistent sequence to path-copy.

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
indexes. It must not scan every unit that contains a reference.

Projecting `Document` and `Markup` is `O(1)` per accessed value and allocates
nothing in the core. A semantic traversal visits semantic nodes, not every
syntax-only token hidden by the projection, which is what the separately
addressable typed child edge of §0 buys. CST capture adds work only where the
existing parse recognizes or preserves concrete material; it must not add a
second whole-document AST-construction pass.

Introducing the CST must not move the FRONTIER this section bounds. Because
concrete offsets are region-relative (§0), a token is never a leaf of a
document-wide structure and no edit elsewhere can reach one: regions reparsed
and `|diffs|` are the same with the CST as without it, and the concrete
material shows up only as a constant factor on the records created inside the
reparsed region.

A change that a concrete difference alone would have propagated — trivia,
delimiter spelling — still costs its region's reparse, which is unavoidable
and localized, and then publishes nothing (9.1). The projection comparison
that discards it is proportional to the region, not to the document.

A commit must not copy one answer value per semantic node. A truly global
renumbering may still require `O(affected)` answer comparisons, trace updates,
and diff entries, because those observable query results genuinely changed; it
does not require materializing those results in the document. The paragraph
that stood here stated this as a bound on copying persistent paths so that an
old document could keep its answers, which is 4.2's removed clause wearing a
cost.

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
9. Public API and implementation audits find one physical unified CST and two
   interfaces over it. `concrete` exposes nodes, tokens, trivia, recovery, and
   source mapping; `semantic` projects `Document` and `Markup`. Every public
   AST node, typed edge, identity, extent, revision, and source map resolves
   from that ownership; there is no parallel AST node hierarchy, CST-to-AST
   remap, or synchronization pass. `Document.concrete` reaches that owner from
   an incrementally committed document, not only from a one-shot parse.
10. Recovery fixtures prove that unmatched core Markdown candidates become
    literal content without `MissingToken`, `UnexpectedToken`, or
    `ErrorRegion`, while every committed bounded-island failure recovers only
    inside its specified boundary and preserves all authored source.
11. Diagnostic fixtures classify bounded recognition/recovery failures as
    syntax diagnostics and projected-value or cross-node failures as semantic
    diagnostics. Neither class changes the other layer's tree or identity.
12. Reference Semantic IR and Frame Tree adapters consume only the public
    semantic projection and parser answers: they do not reparse source or use
    private parser state. Public API audits find no Semantic IR or Frame Tree
    type, value, cache, revision, or update surface in Markdown Core.
13. One-shot, incremental, cache-disabled, and lazily indexed one-shot
    documents expose equal parser answers for equal semantic projections.
    A document a session commits from carries a prepared relation index at
    publication (6.3).
14. Public answer APIs accept and return semantic identities only. Concrete
    tokens and recovery nodes have no parser answers, and syntax diagnostics
    are unchanged when semantic relation indexes are disabled or rebuilt.

### 14.2 Identity and trace gates

1. Prefix insertion preserves every eligible old suffix `MarkupID`.
2. Delete/reinsert allocates fresh node and auxiliary identities.
3. Duplicate-equal sibling matching is deterministic and cannot let a new
   sibling steal an old survivor's identity.
4. Local-only, subtree-only, text-only, projection-only, edge-only, and
   answer-only changes advance exactly their specified stamps.
5. A -> B -> A advances the affected stamps strictly without identity reuse.
6. Canonical no-op reuses the exact document and revisions.
7. A node view resolves only through the document it came from; a
   foreign-domain identity traps.

### 14.3 Source and storage gates

1. Arbitrary legal stored-byte edits remain fresh-parse equivalent under the
   frozen decode/recovery profile.
2. Stored-byte, scalar, UTF-16, line/column, and binding-native projections
   resolve exactly on the published document.
3. `Document.scope` answers exactly on a pinned large document, and a prefix
   insertion changes what no later node PUBLISHES — no diff entry, no changed
   projection. What it costs to answer is measured, not bounded here.
4. Text and child collections change only their changed frontier plus
   inserted/removed content.
5. Long append traces do not degrade quadratically.
7. Splitting one multi-byte character across two commits
   produces, after the completing commit, the document a fresh parse of the
   final bytes produces (13), and the intermediate document remains valid and
   readable after the session closes.

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
7. A definition or footnote flip emits `ANSWERS` on exactly the identities
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
12. A change to a document-wide ordered answer emits `ANSWERS` on the root
    `Document`; a node-addressed answer change emits it on exactly that node.
    Filling or compacting an answer cache emits no entry.
13. A concrete-only edit publishes an empty `diffs`: trailing whitespace, and
    a delimiter respelled from `*x*` to `_x_`. Both reparse their region and
    neither emits an entry or a spine.
14. A prefix insertion emits no entry for any later node, and the later
    nodes' `Document.scope` results move by exactly the inserted length. No
    projection part is measured in source coordinates, so there is nothing a
    prefix insertion could make differ.
15. Inserting a definition emits `ANSWERS` on exactly the identities whose
    answers changed and converts exactly the bracketed forms that now resolve;
    the blocks reparsed and `|diffs|` are the same for a document of any size.

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

**14.7 was the complexity gates, and it is removed.** It reported persistent
nodes and bytes copied, persistent index bytes path-copied, and every term a
second time against an AST-only baseline "with the CST permitted to move only
the records-created term (11.1)" — measurements of a bound 11.1 no longer
states, taken over a persistence 7.2 no longer prescribes. What it required
rather than reported has moved to 11.1, beside the frontier it belongs to:
`|diffs|` and delta application independent of unrelated nodes, and zero
allocations and zero CST walks on ordinary access. Benchmarks stay; they are
measurements, and a measurement that no clause reads is a number, not a gate.

### 14.7 Streaming gates

1. Run human typing, paste, IME, remote, and LLM chunk traces through only
   ordinary byte edits and `commit`.
2. Randomize chunk boundaries and commit cadence, including boundaries that
   fall inside a multi-byte character, under both source profiles. Every such
   commit succeeds, and the trace is also run to completion *without* the
   completing chunk, since a stream may simply stop there (7.1).
3. Compare every final document with a fresh parse — including a final
   document whose bytes end in a truncated code point, which is a legal
   document and must compare equal to a fresh parse of exactly
   those bytes.
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
| `ChildOrdinal` | zero-based position within one child list; a non-negative integer | here |
| `Commit` | what `Session.commit()` returns: `{document, delta}` | 2 |
| `ConcreteID` | stable identity of one concrete node or token: `(domain, ordinal)` | 0 |
| `ConcreteNode` | one unified-CST node: kind, children in source order, identity, extent | 0 |
| `ConcreteToken` | one syntax-only token or trivia run: kind, flags, extent | 0 |
| `ConcreteTree` | the unified CST of one document; what `concrete` exposes | 0 |
| `ErrorRegion` | a recovered span inside a committed bounded island | 0 |
| `MissingToken` | a token the grammar required and the source did not supply | 0 |
| `CoordinateProfile` | closed selector for the space a `Scope` resolves in | 7.2 |
| `Delta` | the difference between two documents, at both of a document's levels | 9 |
| `Diff` | one node whose projection differs, plus which parts differ | 9 |
| `DiffPart` | closed vocabulary of which part of a node differs | 9.1 |
| `DiffParts` | a set of `DiffPart`; a six-flag bitmask | 9.1 |
| `Document` | one immutable parsed unit; the semantic projection's public root | 4 |
| `DocumentDomain` | the opaque scope that identities and revisions live in | 5.1 |
| `DocumentVersion` | which published document: `(domain, revision)` | 5.1 |
| `EncodedOffset` | zero-based offset in a projected coordinate space, never storage | 7.2 |
| `ExtentOrdinal` | positive integer, unique within one domain | here |
| `FootnoteAnswer` | what `Document.footnote` answers for one identity | sessions-and-deltas.md, 4.1 |
| `Markup` | one canonical AST node | canonical-ast.md |
| `MarkupID` | stable identity of one logical node: `(domain, ordinal)` | 5.2 |
| `MarkupKind` | which canonical node kind a `Markup` is | canonical-ast.md |
| `MarkupOrdinal` | positive integer, unique within one domain, never reused | here |
| `MarkupRevision` | a node's revision pair: `(self, subtree)` | 4 |
| `MarkupTrack` | a node's identity, revision pair, and extent | 4 |
| `Offset` | zero-based byte offset; the context names which buffer | here |
| `ParseOptions` | the parse-time options that can affect AST truth | canonical-ast.md |
| `Position` | one endpoint of a `Scope`; widened per coordinate profile | canonical-ast.md, 7.2 |
| `Resolution` | what `Document.resolution` answers for one identity | sessions-and-deltas.md, 4.1 |
| `Revision` | positive, monotonic counter within one domain; the only revision type | 5.1 |
| `SchemaVersion` | the frozen AST-schema version a document conforms to | here |
| `Scope` | a resolved source region: `(start: Position, end: Position)` | canonical-ast.md |
| `Session` | the single mutable owner of one document's pending source | 8.1 |
| `Source` | a document's committed bytes and how they are read | 7.1 |
| `SourceEdit` | one byte-level edit: a span to replace, and its replacement | 8.1 |
| `SourceExtent` | one source extent, identity only: `(domain, ordinal)` | 7.2 |
| `Span` | a half-open run of bytes: `(start, end)` of `Offset` | 8.1 |
| `UnexpectedToken` | a token the grammar did not admit at that position | 0 |
| `Utf8Text` | a node's text field: canonical UTF-8, and nothing beside it | 6.1 |

Every scalar above, in one place. These are the definitions; they get no
section of their own because there is nothing more to say about them.

```text
ChildOrdinal     = non-negative integer  // position within one child list
DiffParts        = set of DiffPart       // a six-flag bitmask
DocumentDomain   = opaque token          // compared for equality, never read
EncodedOffset    = non-negative integer  // in a projected coordinate space
ExtentOrdinal    = positive integer      // unique within one domain
MarkupOrdinal    = positive integer      // unique within one domain, not reused
Offset           = non-negative integer  // a byte offset into a named buffer
Revision         = positive integer      // the only revision scalar (5.3)
SchemaVersion    = positive integer      // the frozen AST-schema version
Utf8Text         = a run of valid UTF-8
```

Types this contract deliberately does **not** have, each removed for a reason
stated where it would have appeared: a lifecycle tag, a wrapper type or role
tag on a child edge (6.2), a parent or position member on `Diff` (9.4), a
per-field address (9.3), a delta schema member (9), a source identity or
boundary affinity on an extent (7.2), a coordinate index member on `Source`
(7.1), an answer-store member on `Document` (6.3), and every consumer
registry, route, interest, contract, target, and acknowledgement type
(3, 14.1.3).

## 16. Rollout gate

The capability ships only when:

1. `canonical-ast.md` and session contracts adopt this exact one-document
   ownership model;
2. every binding exposes equivalent identities, traces, source values,
   `Delta`, and rebuild behavior, and the delta-facing surface in each binding
   is `Delta` plus `DiffPart` — no registry, subscription, interest, or
   acknowledgement type is layered around it (3, 14.1.3);
3. the old four-array delta, its separate ordered-entry table, and
   whole-snapshot scope materialization are absent rather than deprecated;
4. one-shot, incremental, cache-disabled, and fresh-parse fixtures agree;
5. native and binding fault injection proves atomic publication;
6. release telemetry enforces the parser, diff-list-size, application,
   storage, and memory bounds separately; and
7. no parser package acquires rendering, layout, workspace, target-loading,
   framework, or application-model responsibilities; and
8. the session-owned answer APIs and mutation-bounded borrowed answer arrays
   are replaced by the self-contained `Document` queries of 4.1; one-shot and
   incremental documents retain the same immutable answer capability.

Markdown Core remains independently useful as a Markdown parser. Its public
parser model is one unified CST exposed by `concrete` and one self-contained
typed AST projection exposed by `semantic`; the latter is its only semantic
product and the complete adapter boundary for Semantic IR and Frame Tree.
