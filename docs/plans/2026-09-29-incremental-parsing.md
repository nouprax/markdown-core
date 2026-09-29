# Incremental parsing for editing and streaming

Status: proposed design. Nothing in this document is implemented. It
specifies how Markdown Core turns an edit of its source into a new AST while
re-reading as little source and replacing as few AST values as the language
allows, for two workloads:

- **Random edits** from a code editor: replace any range with any text.
- **Tail streaming** from a language model: append text to the end.

The consumers are SwiftUI, Jetpack Compose and React. They reconcile a view
tree against a value tree by identity and equality, so the result of an edit is
**a new `Document` and nothing else**. There is no public diff, patch, change
list or event stream. A consumer that compares the new document with the
previous one finds every unchanged subtree to be the same value (the same
object where the platform has objects), finds every node that persists under
the same identifier, and finds a changed node to be unequal to its predecessor.

Sections 1–3 state requirements and ground them in the current parser. Section
4 is the public model. Sections 5–6 are the engine and binding design. Sections
7–10 cover complexity, testing, rollout and the decisions only the owner can
make. Section 11 records rejected alternatives.

## 1. Requirements

- **R1 Equivalence.** After any sequence of edits, the session's document is
  equal to `Document.parse` of the session's text in every field except
  identifiers. The canonical debug dump, which prints no identifiers, is
  byte-identical. Incrementality never changes what the language means.
- **R2 Bounded re-reading.** The work of an edit is proportional to the
  damaged region, the bounded lookahead around it, and the declarations whose
  resolution it changes, not to the document.
- **R3 Minimal replacement.** A node is a new value only if its value
  changed. Its ancestors are new values because their child collections
  changed. Every other node of the new document is the previous document's
  value, reused.
- **R4 Stable identity.** Every node has an identifier, unique within its
  document, that survives every edit that does not remove the node. An
  identifier never changes kind.
- **R5 No diff.** The API returns a `Document`. Any delta that crosses an
  internal boundary (C to Kotlin or ECMAScript) is transport, not API.
- **R6 One algorithm.** Streaming is an insertion at the end. A full parse is
  an insertion into an empty session. There is no streaming parser, no
  fallback parser and no size threshold that selects a different algorithm
  (see `AGENTS.md`).
- **R7 Transactions.** An edit either commits a new document and text, or
  fails and leaves the session exactly at its previous version.
- **R8 Explicit retention.** What a session retains between edits, its owner
  and its size are specified. Parse scratch still never survives a
  transaction.
- **R9 Concurrency.** Published documents stay immutable and `Sendable`. A
  session has one writer.

Non-goals: error recovery, a different dialect, rendering, and a public API
for editing the AST itself.

## 2. The current parse, stage by stage

`markdown_core_parser_parse` in `core/blocks.c` runs one transaction:

| Stage | Where | What it reads | What it decides |
| --- | --- | --- | --- |
| S0 Envelope | `read_document_prefix` (`elements/document.c`) | Leading Properties lines | `Document.metadata` |
| S1 Blocks | `S_parse_source` → `S_process_line`: `check_open_blocks`, `open_new_blocks`, `add_text_to_container` | One physical line at a time through the input index, plus lookahead (`markdown_core_parser_lookahead_*`) and claimed ranges (`claimed_cursor`) | The block tree on one open-node spine; leaves accumulate content; reference definitions, headings, footnote and specimen definitions register |
| S2 Close | `finalize_document`, `S_parse_block_inputs` | Open spine; queued cell inputs | Container facts computed from children (list tightness, definition scopes); grid and multiline cell bodies |
| S3 Prepare | `prepare_document` | Specimen and heading registries | Heading labels declared as implicit references; reference map complete |
| S4 Finish walk | `S_finish_parse` → `walk_owned_trees` | Every owned root once | Inline parse of each container at ENTER, inline completion, anchor reservation, Text consolidation, finish steps |
| S5 Document | `finish_document` | Registries | Footnote order and `inline-N` ids, specimens, heading anchors and their reference targets |
| S6 Passes | `postprocess_func` (Autolink) | Each finished root | Bare email links |
| S7 Bindings | Swift `DocumentBuilder`; Kotlin and ES decode MCB2 | The finished C tree | One immutable value tree per parse; the C document is freed |

Three properties of this design are what make incrementality possible
without a second parser:

- (1) S1 is a streaming line machine. Between two lines its whole state is the
   open spine, each open node's continuation facts and the open leaf's
   accumulated content. `docs/architecture/block-containers.md` already
   requires that no body is copied into a second input and no subtree is
   reparsed.
- (2) Every inline root is parsed once, from its own content, against a
   reference map that is complete before inline parsing starts. Heading
   suspension (`docs/architecture/heading-resolution.md`) already proves that
   an inline state can stop and resume without a second recognizer.
- (3) Document-wide facts are collected as registrations (source-ordered
   entries with borrowed nodes) and resolved once. They are not rediscovered
   by tree searches.

What is missing is (a) a record of what each decision read, (b) a way to
restart the line machine from the middle, (c) resolution that can be
recomputed for part of a registry, (d) identity, and (e) value storage in the
bindings that can share subtrees between two documents.

## 3. Dependency inventory

Incremental parsing is correct only if every way that one part of the source
affects a node elsewhere is known and handled. This is the complete inventory
for the current dialect. Each row names the mechanism in section 5 that
handles it.

| Dependency | Examples | Direction | Mechanism |
| --- | --- | --- | --- |
| Carried block state | Open list item widths, blockquote and callout prefixes, definition body indentation, directive fence stack, last-line-blank flags | Earlier line → later lines | Block checkpoints and convergence (5.3) |
| Lookahead | Setext underline, table separator and caption, definition term marker, multiline and grid tables, directive closer, Properties closing fence | Later lines → earlier decision | Read frontier (5.3) |
| Retroactive write | A separate-line block identifier sets the preceding block's anchor | Later line → closed node | Read frontier through the write service (5.4) |
| Container facts from children | `List.tight`, list layout, definition and definition-list scopes | Children → container | Spine re-finalization (5.3) |
| Unclosed opaque leaves | Fenced code, HTML block, comment, formula block and directive block without a closer run to the end | Opener → rest of document | No special case: the damage runs until the state converges, which is the language's meaning (7.2) |
| Registry lookups in inline parsing | `[label]` and heading labels in the reference map (`link.c`, `heading.c`), `[^label]` in the footnote label map (`footnote.c`), `@id` in the specimen id index (`citation.c`) | Any definition → any occurrence | Lookup dependency index (5.7) |
| Document ordinals | `inline-N` footnote ids, heading anchor `-N` suffixes, footnote and specimen order | Earlier declarations → later values | Registry recomputation by family (5.7) |
| Shared resources | A definition's destination, title and attributes read by every occurrence | Definition → occurrences | Lookup dependency index (5.7) |
| Absolute coordinates | Every `Scope` after an inserted or deleted line | Every earlier byte → every later scope | Relative geometry (4.3) |
| Mapped cell inputs | Grid and multiline cell bodies | Table → cells | A table is one leaf unit (5.3) |

A dialect change that adds a dependency not covered by one of these
mechanisms must add a row here and its mechanism, the same way a new kind
updates `canonical-ast.json`.

## 4. Public model

### 4.1 Identity

Every `Markup` gains `id: MarkupID`, an opaque integer below 2^53 so that
ECMAScript can hold it as a `number`.

- Swift: `MarkupID: Hashable, Sendable`; every kind conforms to
  `Identifiable`.
- Kotlin: `@JvmInline value class MarkupID(val value: Long)`.
- ECMAScript: `readonly id: number`, usable directly as a React `key`.
- C: `markdown_core_node_id(node)` returns `uint64_t`.

Rules:

- **Unique within a document.** No two nodes of one document share an id,
  across every owned relation (content, labels, captions, titles, terms,
  bodies, affixes, footnotes, specimens, metadata).
- **Deterministic for a fresh parse.** `Document.parse` numbers nodes from 1
  in canonical walk order. Two fresh parses of the same text are equal,
  identifiers included.
- **Stable in a session.** A node reused or matched by an edit keeps its id
  (5.9). A node the edit creates takes the next unused id of the session.
  Ids of removed nodes are never reused by the same session.
- **Kind-stable.** An id denotes one kind for its whole life. A paragraph that
  becomes a Setext heading is a new node with a new id. Every platform
  already renders different kinds with different view types, so keeping the
  id would buy nothing and would weaken the invariant.
- **Scoped to a lineage.** Ids from different sessions or fresh parses are not
  comparable. Nothing may use an id as a key across documents that did not
  come from one session.

### 4.2 Equality

Equality is **deep value equality including `id`**: two nodes are equal when
they have the same kind, id, scalar fields, geometry (4.3) and pairwise equal
children in every relation.

- Every implementation first compares references: a reused subtree is the same
  object, so comparing two versions of a document costs the size of the
  changed paths and their siblings, not the document.
- The engine keeps an invariant (5.9) that makes the fast path nearly always
  decisive: within one session, a node that has the same id as its
  predecessor but is a different object has a different value.
- Hashing uses `id` only, so it is O(1) and consistent with equality.
- Swift: every kind is `Hashable`; `any Markup` gets an `isEqual(_:)`
  helper. Kotlin: `equals` and `hashCode` on every kind, with the reference
  check first. ECMAScript has no equality protocol; React's `Object.is`
  and `React.memo` see structural sharing directly. An exported
  `markupEquals(a, b)` gives the deep comparison for tests.

### 4.3 Positions leave node values

Today every node stores an absolute `Scope`. If that stays, typing Enter on
line 3 changes the scope, and therefore the value, of every node after line
3, and R3 cannot hold for any edit that adds or removes a line. Excluding
scope from equality is not an option: a view that shows a node's position
would then be skipped after the position changed.

The design therefore makes each node's stored geometry **relative**, so that
it changes only when the node itself or its immediate neighbourhood changes:

```text
Offset(lines: Int32, column: Int32)
    lines == 0: column is a column delta on the same line
    lines != 0: column is the absolute column on the target line

Span(lead: Offset, extent: Offset)
    lead:   from the anchor to this node's start
    extent: from this node's start to this node's end
```

The anchor of the first node in a relation is its owner's start. The anchor
of every later node in the same relation is the previous node's end.
Definition bodies continue the anchor chain across their groups. Document
footnotes and specimens anchor to the document start and then to each other.
The encoding is exact arithmetic on the current coordinates, so the native
sentinels (`1:1..0:0`, ends at column 0, spanning grid cells that end beyond
their row) round-trip unchanged. Nothing is normalized.

With this encoding an edit inside node N changes N's extent and its
ancestors' extents (they are new values anyway) and, when N's end moves on
its last line, the lead of the sibling that starts on that line. An edit in
the gap between two siblings, such as an added blank line, changes only the
second sibling's lead; its subtree is reused. Nodes after
the edit that start on a later line keep their lead and extent. Their values
do not change.

Absolute scopes remain available, unchanged in meaning:

- The walker passes the absolute `Scope` with each callback. It computes it in
  O(1) per step from the span chain it is already traversing.
- `Document.scope(of: node)` answers any node's absolute scope. Its index is
  built on first use for that document version in one linear walk of the
  relative values, with no parsing.
- `Document.node(at: Position)` descends the tree by spans, for editor hit
  testing.
- The canonical dump prints absolute scopes exactly as today, so every
  conformance fixture and golden stays byte-identical.

`Markup.scope` is removed from node values and `Markup.span` is added. This
is a breaking change to the canonical AST contract and to every binding, and
the owner decides it (section 10, D1). The alternative, keeping absolute
scopes in nodes, keeps the API but makes every edit that changes the line
count replace every node after it. Tail streaming is unaffected either way,
because an append never moves an earlier position.

### 4.4 Sessions

A session owns a text, its current document and the retained parse state.

```swift
public final class MarkdownSession {           // one writer; not Sendable
    public init(_ source: String = "") throws
    public var document: Document { get }       // immutable, Sendable
    public var text: String { get }
    @discardableResult
    public func replace(_ range: Range<Int>, with text: String,
                        unit: TextUnit = .utf16) throws -> Document
    @discardableResult
    public func append(_ text: String) throws -> Document
    @discardableResult
    public func apply(_ edits: [TextEdit]) throws -> Document
}
```

Kotlin has the same shape as an `AutoCloseable` class. ECMAScript exports
`class MarkdownSession` with `dispose()` (and a `FinalizationRegistry`
backstop), because its state lives in WebAssembly memory. C exposes
`markdown_core_session_new`, `_replace`, `_append`, `_document` and `_free`.
C views borrow from the session until its next edit.

- **Units.** Editors report UTF-16 offsets (UIKit `NSRange`, Compose
  `TextFieldValue`, CodeMirror and Monaco). Bindings default to UTF-16 and
  also accept UTF-8. C takes UTF-8 bytes. The text tree (5.1) converts in
  O(log n + line length).
- **Batches.** `apply` takes disjoint edits in the coordinates of the text
  before the batch and parses once, for multi-cursor edits and bulk
  replacements. The transaction keeps every edit as its own piece of the
  position mapping (5.2), so bytes between two edits stay surviving bytes
  with their own shift. Damage is per edit; regions whose restart and
  convergence windows overlap are merged, and the others are re-read
  independently in source order (5.3).
- **Partial UTF-8.** C `append` may split a scalar. The bytes of an
  incomplete trailing sequence are **pending**: they are not part of the
  session's text, `markdown_core_session_text` does not return them, and the
  document is the parse of the text without them. When a later append
  completes the sequence, the whole scalar enters the text in that edit. The
  session's text is therefore always valid UTF-8, the existing precondition
  holds for every parse, and R1 compares against the text as defined here.
  An append that makes the pending bytes impossible to complete (a byte that
  cannot continue the sequence) is rejected as an invalid argument, and the
  session is unchanged. Binding strings are whole scalars, so bindings never
  have pending bytes.
- **`Document.parse`** is unchanged in signature. It is a session that
  inserts the whole source once and is then discarded.

## 5. Engine

### 5.1 What a session retains

| State | Contents | Size |
| --- | --- | --- |
| Text tree | The source as a balanced tree of bounded byte chunks; each subtree records its byte, line-terminator and UTF-16 counts | O(source) |
| Tree | The live C tree: relative spans, ids, versions | O(nodes) |
| Block ledger | One entry per block node at any depth: start offset, node id, entry frontier, read end, spine snapshot (5.3) | O(blocks + changed frames) |
| Registries | Reference, heading, anchor, footnote and specimen declarations in source order; label winners | O(declarations) |
| Lookup index | Registry key → inline roots that looked it up, hit or miss | O(lookups) |
| Frontier | The suspended block parser at the last line boundary, when the document ends in open blocks (5.5) | O(open spine + open leaf content) |
| Inline ledger | Per inline root: stable prefix end (5.6) | O(inline roots) |

The text, the block ledger and the source-ordered registries are sequences
whose elements have source extents. They share one structure: a balanced
tree whose elements store their length (for text, a chunk's bytes; for a
ledger or registry entry, the byte distance from the previous entry) and
whose internal nodes store the sums. Absolute offsets, line numbers and
UTF-16 offsets are prefix sums. Finding an offset, inserting, deleting, and
shifting everything after an edit all cost O(log n), wherever the edit is, so
alternating edits at opposite ends of a document cost the same as edits in
one place. There is one such structure, not one per consumer. (A gap buffer,
or a sorted array with one lazy shift, would move Θ(n) bytes or keys whenever
consecutive edits are far apart.)

Every other workspace in `docs/architecture/parser-input-storage.md`
(lookahead facts, table geometry, source-order scratch, delimiter pools)
stays scoped to the transaction that re-parses the damaged region. Caches keyed
by line are therefore never stale.

Reading source now goes through the input index for every consumer, as it
already does for the driver, lookahead, Properties and tables. The index
resolves a line against the text tree. A line inside one chunk is borrowed. A
line that spans chunks gets one contiguous view through the mechanism that
already provides normalized views for NUL-bearing lines, so no scanner sees a
chunk boundary. The view lives for the transaction, like other scratch.

### 5.2 From an edit to damage

An edit replaces bytes `[a, b)` of the old text with `n` bytes. Damage is
first widened to whole physical lines: from the start of the line containing
`a` (or the previous line when `a` follows a CR, since an inserted LF can
join CR and LF into one terminator) to the end of the line containing the new
`a + n`.

The position mapping is defined on **bytes**, and only on bytes that survive.
A byte at old offset `x` survives when it is not inside a replaced range. Its
image is exact:

```text
x < a    → x
x >= b   → x + n - (b - a)
a <= x < b: replaced, no image
```

A batch is a list of disjoint replacements. The mapping is piecewise: a
surviving byte's image is its offset plus the sum of the length changes of
the replacements before it. There is no interval and no ambiguous image;
inserted bytes are the preimage of nothing. Identity (5.9) is defined through
this mapping alone.

### 5.3 Blocks: restart, re-parse, converge

**Recording.** While S1 runs, the input index keeps a high-water mark of the
last line any decision has read, including lookahead and claimed ranges. Each
block records two values in the ledger:

- its **entry frontier**: the high-water mark when the block opened;
- its **read end**: the high-water mark when the block closed, raised
  afterwards by any retroactive write that targets it (5.4).

A block's read end therefore covers its closing line, its lookahead and every
later line that wrote into it. The mark is automatic: an element does not
declare what it read, so a new element cannot forget to.

**Restart.** A restart point is the start of a ledger block `R` such that:

- (1) `R` starts at or before the damaged start line;
- (2) `R`'s entry frontier is before the damaged start line, so nothing
   decided before `R` read the damage;
- (3) no block that closed before `R` has a read end at or after the damaged
   start line.

The engine takes the latest such `R` by walking the ledger backwards from the
damage. Condition 3 is what moves the restart before a paragraph whose Setext,
table or definition lookahead reached the edited line. The restart is a block
at any depth, so an edit in the thirtieth item of a list restarts at that item
(or at the block in it), not at the list.

**Spine snapshots.** An open container's carried state changes while it is
open: list continuation, for example, reads and updates the list's
last-line-blank flag on every line, and that flag decides whether a later blank
line is consumed. A container's node therefore holds the state at the end of
the old parse, not at `R`. So each ledger entry stores the carried state of
its whole spine at the moment the block opened, as a **spine snapshot**: an
immutable list of frames, innermost first, each holding one open container's
id and its carried facts (E3). Snapshots share frames: a new frame is made
only for a container whose facts differ from the frame the previous snapshot
used, together with the frames inside it. Storage is one frame per block plus
the frames of changed containers, and a work counter gates it against deep
nesting with changing flags.

Restarting at `R` reopens `R`'s ancestors, which are exactly the open spine
at that line: each ancestor is marked open and its carried facts are restored
from `R`'s snapshot, not read from its final node data. `R` and everything
after it in the ancestors' child chains are detached and kept as reuse
candidates. Every one of these changes goes through the transaction journal
(5.11), so a failed edit can restore them. The line machine then runs from
`R`'s first line with the ordinary `S_process_line`. No other entry point
exists.

**Convergence.** After the damaged end, at the start of each new block at
line `j`, the engine looks up the old ledger entry that starts at the mapped
old line `j'`. It converges when all of these hold:

- (1) such an old block `O` exists and was not damaged;
- (2) the live new spine and `O`'s spine snapshot are equal: the same kinds at
   every depth and equal carried facts through each element's `carry_equal`
   hook, including last-line-blank flags. The comparison is exact and walks
   the spine; a hash only filters. Frames shared between the two sides
   compare by identity;
- (3) the new high-water mark is at most `j`, and `O`'s entry frontier is at
   most `j'`, so no decision on either side is still reading across the
   boundary.

The line machine is a deterministic function of its state and the bytes it
reads. With equal state and identical bytes from `j` on, the old parse's
future is the new parse's future. So at convergence the engine stops reading:
`O`, its following siblings and every later sibling of each spine ancestor are
spliced back from the candidates, and their ledger entries are shifted, not
rebuilt.

**Re-finalization.** The spine containers are still the new parse's open
nodes. Their closing facts come from the old parse's corresponding closes,
which convergence proves identical, and then each spine container runs its
finalize hook over its full children. Finalize for containers is required to
be a pure function of children and recorded facts (5.4), which is what makes
`List.tight` correct when the edit added a blank line between two early items
and every later item was reused.

**Units that are always whole.** A leaf is re-read whole when damaged: a
paragraph, a code block, an HTML block, and a table with its caption and
mapped cell inputs. Cells are internal inputs of the table's transaction, as
now. Value deduplication (5.9) then keeps every unchanged row and cell.

**Several damaged regions.** A batch (4.4) can damage several regions. The
engine handles them in source order with the same procedure: restart before
the first, converge after it, then restart before the next. When a region's
restart point falls before the previous region's convergence, the two are one
region. Convergence is never taken inside a region that is still to be
re-read.

**Degenerate cases are the same algorithm.** A fresh parse restarts at the
document with nothing to converge with. An edit in the Properties envelope
restarts at the document because the envelope is the first block. An opener
of an unclosed fence converges nowhere until the fence closes, because the
rest of the document really did change meaning.

### 5.4 The element contract

The engine can reuse and restart only what elements make deterministic.
These are requirements on every element, each checked by an audit script in
`scripts/audit/` in the style of `parser-boundaries.mjs`:

- **E1 Reads go through the index.** Source reads, including lookahead and
  claimed ranges, go through the input index, so the high-water mark sees
  them. An element never keeps a raw pointer into the source across lines.
- **E2 Retroactive writes go through one service.** Changing a node that has
  closed (the separate-line block identifier is the current case) uses
  `markdown_core_parser_write_closed(parser, node)`, which raises the node's
  read end to the current line. The audit forbids other writes to closed
  nodes.
- **E3 Carried state is declared.** Per-parse element state (`state_size`) is
  one of three things: a cache that the transaction may drop; a declaration
  registry that moves to the session (5.7); or carried block state, which is
  stored on the open node, saved into spine snapshots by `carry_save`,
  restored by `carry_restore` and compared by `carry_equal`. Nothing else may
  carry information from one line to a later one.
- **E4 Container finalize is idempotent.** It reads children and recorded
  facts and writes the container's own fields. Running it twice gives the same
  node.
- **E5 Leaf finalize does not consume accumulation.** It produces the node's
  value from the accumulated content without destroying that content, so the
  frontier (5.5) can publish a provisional value and keep accumulating.
  Paragraph already records its consumed reference-definition prefix as a
  persistent fact; trailing-whitespace trimming becomes a length, not a
  truncation of the buffer.

### 5.5 Tail streaming: the frontier checkpoint

When the document ends inside open blocks, which is almost always true during
streaming, the session keeps the block parser suspended at the start of the
last physical line: the open spine, each open leaf's accumulated content and
content map, and the index's high-water mark. This is the one checkpoint that
is not a block start, and it satisfies the same restart conditions: an edit
at or after its line restarts there.

Publishing a document from a suspended parser finalizes the open spine
**provisionally**. Leaves produce values by E5 without consuming their
buffers. Containers produce values by E4. The published nodes get ids and are
matched by the ordinary rules at the next edit, so a paragraph that is still
growing keeps its id from its first line to its last.

An append of `c` bytes then costs: re-reading the previously unterminated last
line and the new bytes through `S_process_line`, a provisional finalize of the
open leaf, inline work from the leaf's stable prefix (5.6), and path copying.
When the appended text closes blocks, they close normally and their
provisional values are replaced by final ones only where the values differ.

### 5.6 Inline parsing

An inline root is re-parsed when its leaf was re-read, or when a registry
winner it looked up changed (5.7). Everything else keeps its inline tree.

For a leaf that was re-read only by extension at its end (the frontier leaf
during streaming, or an edit at the end of a paragraph), the inline parse
restarts at the leaf's **stable prefix end**, recorded at the end of its
previous inline parse. It is the smallest content offset of:

- a delimiter-stack entry that could still pair or be claimed: a marker that
  can open, a citation token, a field, an unclosed bracket;
- a token decision that reached the end of the content: an unmatched backtick
  run, an unclosed HTML or comment token, a formula without a closer, or any
  scanner that stopped at the slice limit rather than at a byte;
- the start of the last line, because a line's trailing whitespace is a hard
  break inside a paragraph and is trimmed at its end.

Before that offset the stack holds only boundary entries, and the delimiter
model already summarizes those as one floor per range
(`docs/architecture/inline-delimiters.md`). The state at the offset is
therefore reconstructible exactly: an empty stack with that floor. Inline
nodes wholly before the offset are reused, the Text node that straddles it is
rebuilt, and parsing continues to the end. This is the same inline parser,
starting at an offset.

Typical streamed prose closes its delimiters within a few words and its Text
nodes are split per line by SoftBreak, so the per-chunk inline work is about
the size of the current line. An early opener that never closes keeps the
stable prefix at that opener: the cost then grows with the distance from it,
which is inherent, because a later closer can still pair with it.

### 5.7 Registries and resolution

The S1 registrations and S3 declarations move from the transaction to the
session and become source-ordered sequences (5.1). An edit replaces exactly
the entries whose nodes were re-read, which is one contiguous range per
registry. Then:

- **Winners.** For each normalized label whose entries changed, the first
  definition in source order is recomputed, with explicit definitions before
  implicit heading targets as today. The same happens for footnote labels and
  specimen ids.
- **Lookup dependencies.** During inline parsing every registry query records
  `(registry, key) → inline root`, whether it hit or missed. A miss matters as
  much as a hit: adding `[x]: /u` turns every `[x]` into a Link. When a
  winner changes, its dependents are queued for inline re-parse. The index
  holds edges in both directions: each inline root owns the list of keys it
  queried, and each key the set of root ids that queried it. Re-parsing a
  root first removes all of its old edges and then records the new ones;
  retiring a root removes its edges. Both go through the journal (5.11). The
  index therefore holds exactly the current document's lookups, and an edge
  never names a retired node. A heading's
  declarability depends only on its own content ("a valid declaration cannot
  depend on a reference lookup", `heading-resolution.md`), so this settles in
  one round, with no fixed point.
- **Anchors by family.** Generated anchors interact only through their
  spelling. A family is the set of spellings with the same stem after
  stripping trailing `-N` groups. An edit recomputes, in source order, only the
  families of changed headings and changed explicit anchors, with the same
  reservation and suffix-cursor algorithm as today. A changed anchor updates
  its heading target resource, and the resource's occurrences follow through
  the lookup index.
- **Footnote and specimen order.** `Document.footnotes` and
  `Document.specimens` are the source-ordered registries, spliced. `inline-N`
  ids are ordinals, so inserting an inline note renumbers the inline notes
  after it and their Cites (7.2, D2).

### 5.8 Finish steps and passes

The finish walk runs on the inline roots and block subtrees that were re-read
or re-resolved, never on reused ones. Consolidation, script-escape
completion, list layout, formula promotion and the Autolink email pass all act
within one root, so their results for a reused root are already in the reused
tree. `check-finish-hook-shapes.mjs` gains the rule that a finish step or pass
reads only its root and the registries, which is what lets them run per root.

### 5.9 Identity matching and value deduplication

After re-parsing, each new node in the re-read region is matched to an old
node:

- Matching runs per owner relation between a new owner and the old node it
  matched, starting from the reopened spine, whose nodes kept their ids.
- Each old node has an **anchor byte**: the first byte of its source range
  that survived the edit. A node none of whose bytes survived has no anchor
  and cannot be matched; its id retires.
- An old node `O` can match a new node `N` when their kinds are equal and
  `N`'s source range contains the exact image of `O`'s anchor byte (5.2).
  Siblings in one relation have disjoint ranges, so an anchor image lies in
  at most one candidate.
- When `N` contains the anchors of several old siblings, it takes the
  earliest. Both sequences are in source order and the match is monotone, so
  it is linear in the region.
- Consequences, each from the one rule:
  - Typing at the start of a paragraph keeps its id: the old first byte
    survives and its image lies inside the extended paragraph.
  - Inserting `new\n\n` before a paragraph gives the new paragraph a new id
    and keeps the old paragraph's id: the old first byte's image is the start
    of the second paragraph, not inside the first.
  - Deleting a paragraph's first word keeps its id: its anchor moves to its
    first surviving byte.
  - Deleting a whole sibling retires its id and never hands it to the next
    sibling, whose own anchor lies inside it.
  - Merging two paragraphs by deleting the blank line keeps the first one's
    id; the second retires.
  - Bytes between the edits of a batch keep their own exact images, so nodes
    there match as if each edit were alone.
- Children of an unmatched owner get new ids. A paragraph that moves into a
  new blockquote is a new node, as it is to every UI framework.

Then, in post-order, each matched `N` is compared with its `O`: equal kind,
equal scalars, equal span, and every child relation holding the same objects.
If they are equal, `N` is released and `O` stays. This establishes the
invariant of 4.2: within a session, a node that differs from its predecessor
as an object differs as a value.

Every node carries a `version`, the session edit number at which its value
last changed. New and changed nodes take the current version. Reused and
deduplicated nodes keep theirs. An ancestor of a changed node is changed,
because its child collection changed.

### 5.10 Why the result equals a fresh parse

- Blocks: the restart state equals the fresh parse's state at `R`, by
  conditions 2 and 3 of the restart rule and E3. Re-reading is the same
  function. At convergence the future is identical by determinism, E1 and E2.
  Spine re-finalization is correct by E4.
- Inline trees: a reused root's content and lookup answers are unchanged. A
  re-parsed root runs the same parser against the same registries. A stable
  prefix restart is exact by the delimiter model's floor summary.
- Resolution: winners, families and ordinals are recomputed over complete
  source-ordered registries with the same rules.
- Finish: per-root steps on unchanged roots gave the same results before.

This argument is also the test oracle (section 8).

### 5.11 Transactions

An edit is a transaction over session-owned state: the text tree, the live
tree's links, flags and fields, the ledger, the registries, the lookup index,
the frontier and the inline ledger. Everything else a re-parse allocates is
scratch or new nodes, which a failure simply releases.

Every mutation of session-owned state goes through one journal. The journal
entry that can undo a mutation is reserved before the mutation happens, so
recording never fails after the state has changed. Examples: detaching a child
chain records the old links; marking a spine node open records its flags;
replacing a text range first moves the replaced chunks into the entry instead
of freeing them; splicing a ledger or registry range keeps the removed
elements in the entry.

- **Commit** happens once, after the new document is complete: the journal's
  retained old elements (replaced chunks, removed ledger entries, old nodes
  that did not survive) are released and the version advances.
- **Rollback** replays the journal in reverse. It allocates nothing and cannot
  fail, and afterwards the session's text, document, ids, versions and
  retained state are the previous version's, bit for bit.

Id allocation takes part: ids handed out by a failed transaction are returned,
so a failed edit does not skip ids either. The journal is the only mutation
path to session state; an audit rejects direct writes to it from parse code,
as E2 does for closed nodes.

## 6. Bindings

### 6.1 Materialization without a public diff

Each binding session keeps a table from id to its value object for the live
document. To publish a version, it walks the new C tree from the root. A node
whose version is not newer than the binding's last published version is taken
from the table with its whole subtree, and the walk does not descend into it.
Every other node is built from its fields and its children, which are table
hits or newly built nodes. The engine reports the ids retired by the edit so
the table can release them.

The cost is proportional to the changed nodes plus their children, which is
also what SwiftUI, Compose and React reconcile. The public result is one
`Document`.

### 6.2 Wire format MCB3

Kotlin and ECMAScript receive a parse as one message. MCB3 extends MCB2
(`docs/architecture/wire-format.md`) and keeps its post-order stack model:

- Every node record adds `u64 id` and relative `Span` in place of `Scope`.
- A new record `reuse(u64 id)` pushes the binding's existing value for that id,
  subtree included, and writes nothing else.
- A trailer lists retired ids.

A fresh parse is a message with no `reuse` records and no trailer. It is one
format, not two. The magic becomes `MCB3` because the record layout changes.

### 6.3 Swift storage

Swift currently copies each parse into one flat `StoredMarkup` array owned by a
`MarkupStore` (`docs/architecture/swift-storage.md`). A store belongs to one
parse, so two documents cannot share a subtree through it. The design replaces
it with one immutable final class record per node that holds its scalars and
references to its children's records. That gives exact sharing between
versions, `===` for the equality fast path, and liveness by ARC.

The flat store existed partly to keep destruction depth bounded. The records
keep that property with an iterative `deinit`: a record releasing its last
reference moves its uniquely referenced children to a thread-local work stack
instead of releasing them recursively. The existing 30,000 and 65,536 level
release tests stay as the gate. Kotlin and ECMAScript already hold one object
per node, so this brings Swift to the same model instead of keeping a
Swift-only layout. The cost is one allocation per node on a fresh parse where
there used to be one per document; the benchmark gate measures it (D3).

### 6.4 Kotlin

Every kind gets reference-first `equals` and id-based `hashCode`. Collections
stay read-only `kotlin.collections.List`. The package ships a Compose
stability configuration file that lists the AST types as stable, because the
Compose compiler treats classes from a module without the Compose compiler as
unstable. No Compose dependency is added.

### 6.5 ECMAScript

Values stay plain readonly objects. A reused subtree is the same object, so
`React.memo`, `useMemo` dependency arrays and keyed lists work without an
adapter. The session holds a WebAssembly handle and must be disposed. A
`Document` never holds a handle, as now.

## 7. Complexity

### 7.1 Bounds

Let `d` be the depth of the edit point, `F` the sum of child counts along the
changed paths, `L` the size of the re-read leaves, `W` the lookahead window
before convergence, and `k` the size of inline roots invalidated by
resolution changes.

| Operation | Block work | Inline work | Resolution | Materialization |
| --- | --- | --- | --- | --- |
| Append `c` bytes inside an open paragraph | O(c + last line) | O(c + distance to stable prefix) | O(changed declarations) | O(d + F) |
| Append that closes and opens blocks | O(c + last line + closed leaves) | as above | as above | O(d + F) |
| Edit inside one closed leaf | O(L + W) | O(L) | O(changed declarations + k) | O(d + F) |
| Edit that changes container structure | O(blocks until convergence) | O(re-read leaves + k) | as above | O(changed nodes + F) |
| Fresh parse | O(n), as today | O(n) | O(n) | O(n) |

The text tree, the ledger and the registries add O(log n) per lookup,
insertion, deletion and shift, wherever the edit is. No bound depends on a size threshold.

### 7.2 Costs that are the language's, not the algorithm's

Some edits really do change most of the document, and the design does not
pretend otherwise:

- Opening an unclosed fence, HTML block, comment or directive block near the
  top changes the meaning of everything after it until something closes it.
- A definition referenced 10,000 times changes 10,000 Links when its
  destination changes.
- Inserting an inline footnote renumbers every later inline note's `inline-N`
  id and its Cite. This follows from the current id contract (D2).
- A leaf's literal is one string value, so a code block streamed to the end
  of a response produces a new literal of the whole block per chunk. The
  renderer re-highlights that block per chunk anyway.
- A table at the tail is re-read whole per appended row, bounded by the
  table's size.

## 8. Testing

- **Differential oracle.** For every document in the benchmark corpus and
  fuzz inputs, random edit scripts (inserts, deletes, replacements at line and
  byte granularity, including CR/LF splits and NUL) are applied through a
  session. After every edit, the canonical dump must equal the dump of a fresh
  `Document.parse` of the session's text. This runs in C and in each binding.
- **Streaming.** Every corpus document is fed in chunks of every size from
  one byte up, and split at every byte offset for small documents, including
  inside UTF-8 scalars in C. Every intermediate document must equal a fresh
  parse of the session's text, which excludes pending bytes (4.4), and the
  session's text must equal the longest complete-scalar prefix of the bytes
  appended so far.
- **Identity and minimality.** After every edit: ids are unique; no id
  changed kind; every reused object equals the fresh-parse node at the same
  position; every matched node that is a new object differs in value from its
  predecessor. For scripted edits the exact set of new objects is asserted
  (for example, typing in paragraph 5 of 1,000 replaces that paragraph, its
  Text nodes on the edited line and the Document).
- **Work counters.** Deterministic counters, like the existing
  `input_line_work` and `delimiter_work`, gate lines re-read, inline bytes
  re-parsed and nodes materialized per edit against the bounds of 7.1,
  including adversarial shapes: a stray early opener, a 10,000-item list edited
  in the middle, 1,000 nested block quotes, a definition with thousands of
  references.
- **Transactions.** The allocator-seam OOM sweep runs every edit at every
  allocation boundary and asserts that the session's text, document, ids and
  retained state equal the previous version afterwards, and that the next edit
  succeeds.
- **Audits.** E1–E5 (5.4), the finish-step root rule (5.8), and the
  dependency inventory (section 3) are enforced by scripts in
  `scripts/audit/`.

## 9. Rollout

Each step is one pull request that leaves `main` releasable.

- [ ] **Step 1: Model.** Ids for fresh parses, deep equality and hashing, relative spans
   with walker and document scope resolution, MCB3, and the Swift record
   storage. The canonical dump and conformance fixtures do not change.
- [ ] **Step 2: Sessions with a whole-document restart.** Session API on every platform,
   the text tree, the journal and transactional edits, identity matching,
   value deduplication, versions and `reuse` materialization. The restart
   point is always the document and nothing converges: this is the degenerate
   case of the final algorithm, and it already gives R1, R3, R4 and R5, with
   O(n) parse work.
- [ ] **Step 3: Block restart and convergence.** The ledger, the high-water mark, E1–E4
   and their audits, spine re-finalization.
- [ ] **Step 4: Session registries.** Source-ordered registries, winners, lookup
   dependencies, anchor families, per-root finish steps.
- [ ] **Step 5: Frontier and inline restart.** The suspended frontier, E5, and the
   stable prefix.
- [ ] **Step 6: Gates.** Work-counter bounds and benchmark cases for streaming and random
   edits, in the existing benchmark workflow.

## 10. Decisions for the owner

- **D1 Positions.** Move absolute scopes out of node values into relative
  spans with `Document.scope(of:)` and walker scopes (recommended), or keep
  `Markup.scope`, and accept that any edit that changes the line count
  replaces every node after it.
- **D2 Inline footnote ids.** Keep `inline-N` ordinals (recommended for now:
  the cascade is limited to later inline notes and their Cites), or change the
  contract to an id that does not depend on earlier notes.
- **D3 Swift storage.** Per-node records (recommended: exact sharing and one
  model across bindings), or keep the flat store and add a cross-version
  segment scheme, which retains dead records until compaction.
- **D4 Edit units.** UTF-16 offsets by default in bindings with UTF-8
  available (recommended, matching editor APIs), or UTF-8 only.

## 11. Rejected alternatives

- **Returning a diff or patch.** Excluded by the requirement. Consumers
  already reconcile by identity and equality; a second protocol would be a
  parallel source of truth.
- **Re-parse everything, then reconcile the trees.** It achieves R3 and R4 but
  not R2, and it is O(n) per keystroke and per streamed token. It survives
  only as the test oracle and as rollout step 2, where it is the degenerate
  restart of the same algorithm.
- **Content-hash or path identifiers.** A hash changes on every edit of the
  node, so an edited paragraph would lose its view state. A path changes when
  an earlier sibling is inserted. Neither is stable.
- **A separate streaming parser.** Two parsers diverge. Streaming is an
  insertion at the end, and the frontier checkpoint makes it cheap without a
  second grammar.
- **Top-level blocks as the only reuse unit.** Model output is often one long
  list or quote, and such a unit would re-read all of it per token. Ledger
  entries at every depth cost one entry per block.
- **A general incremental parsing framework (GLR, packrat memoization).**
  Markdown's block grammar is a line machine with bounded lookahead. The
  existing parser's determinism is enough for convergence, and memoizing every
  rule would multiply memory for no additional reuse.
