# Incremental parsing for editing and streaming

Status: proposed design. Nothing in this document is implemented. It
specifies how Markdown Core turns an edit of its source into a new AST while
re-reading as little source and replacing as few AST values as the language
allows, for two workloads:

- **Random edits** from a code editor: replace any range with any text.
- **Tail streaming** from a language model: append text to the end.

The consumers are SwiftUI, Jetpack Compose and React. They reconcile a view
tree against a value tree by identity and equality, so the result of an edit is
**a new `Document`**, a plain immutable value like the one a fresh parse
returns. A consumer that compares it with the document it already holds finds
every unchanged subtree to be an equal value, every node that persists under
the same identifier, and every changed node unequal to its predecessor. That
comparison is the consumer's framework's own work; this design gives it the
ids and equality it needs and nothing else.

Sections 1–3 state requirements and ground them in the current parser. Section
4 is the public model. Sections 5–6 are the engine and binding design. Sections
7–10 cover complexity, testing, rollout and the decisions only the owner can
make. Section 11 records rejected alternatives.

## 1. Requirements

- **R1 Equivalence.** After any sequence of edits, the session's document is
  equal to `Document.parse` of the session's text in every field except
  identifiers. The canonical debug dump, which prints no identifiers, is
  byte-identical. Incrementality never changes what the language means.
- **R2 Bounded re-reading.** The engine's work for an edit is proportional to the
  damaged region, the bounded lookahead around it, and the declarations whose
  resolution it changes, not to the document.
- **R3 Minimal replacement.** A node is a new value only if its value
  changed. Its ancestors are new values because their child collections
  changed. Every other node of the new document equals the previous
  document's node with the same id; in the C document it is that node,
  reused.
- **R4 Stable identity.** Every node has an identifier, unique within its
  document, that survives every edit that does not remove the node. An
  identifier never changes kind.
- **R5 Values only.** An edit returns the new `Document`. A binding builds it
  from the C document the same way it builds a fresh parse (6.1).
- **R6 One algorithm.** Streaming is an insertion at the end. A full parse is
  an insertion into an empty session. There is no streaming parser, no
  fallback parser and no size threshold that selects a different algorithm
  (see `AGENTS.md`).
- **R7 Errors.** Out of memory at any stage throws an error. Nothing is
  rolled back.
- **R8 Concurrency.** Published documents stay immutable and `Sendable`. A
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
recomputed for part of a registry, and (d) identity.

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
| Document ordinals | `inline-N` footnote ids, heading anchor `-N` suffixes, footnote and specimen order | Earlier declarations → later values | `inline-N` and the lifted footnote and specimen sequences are removed from the model (4.5); anchor suffixes by recomputation by family (5.7) |
| Shared resources | A definition's destination, title and attributes read by every occurrence | Definition → occurrences | Lookup dependency index (5.7) |
| Absolute coordinates | Every `Scope` after an inserted or deleted line | Every earlier byte → every later scope | Positions leave the AST (4.3) |
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
  bodies, affixes, footnote referents, metadata).
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
- **Scoped to a session.** Ids from different sessions or fresh parses are not
  comparable.

#### The list identity contract

The id is designed for SwiftUI's `ForEach` and `List`, and the same contract
serves Compose `key` in lazy lists and React `key`. Those APIs require three
things of the ids in one collection, and the rules above give each one:

| Framework requirement | Guarantee |
| --- | --- |
| Ids in one collection are unique in every render. SwiftUI's behaviour with duplicates is undefined. | Ids are unique across the whole document, so they are unique in any collection taken from it: `content`, a list's `items`, a table's rows, footnotes, or a heterogeneous array a consumer builds from several relations. Every published document is complete, so there is no intermediate state with a duplicate. |
| An id names the same element across updates, so its view state (focus, scroll anchor, expansion, animation) carries over. | An id persists while its node persists (R4, 5.9). |
| An id that leaves never comes back as something else, or a new element inherits a removed element's state. | A session never reissues a retired id. A kind change is a new id, so the view type built for an id never changes. |

Usage is direct. `Markup` refines `Identifiable` with `id: MarkupID`, so a
collection of a concrete kind works as `ForEach(list.items) { … }`. A
heterogeneous `MarkupCollection<any Markup>` uses the key path, because an
existential does not itself conform to `Identifiable`:

```swift
ForEach(document.content, id: \.id) { block in
    BlockView(block)          // switch on the concrete kind
}
```

### 4.2 Equality

Equality is **deep value equality including `id`**: two nodes are equal when
they have the same kind, id, scalar fields, extent (4.3) and pairwise equal
children in every relation.

- Consumers reconcile with ids and this equality: SwiftUI with `Identifiable`
  and `Equatable`, Compose with keys and `equals`, React with keys and
  `React.memo(component, (a, b) => markupEquals(a.node, b.node))`. A node the
  edit did not change is equal to its predecessor and has its id, so each
  framework skips it; it is not the same object, because each binding builds
  every document whole (6.1).
- Hashing uses `id` only, so it is O(1) and consistent with equality.
- Swift: every kind is `Hashable`; `any Markup` gets an `isEqual(_:)`
  helper. Kotlin: `equals` and `hashCode` on every kind, with the reference
  check first. ECMAScript has no equality protocol, so it exports
  `markupEquals(a, b)`.

### 4.3 Nodes carry raw extents; scopes are computed on request

Today every node stores an absolute `Scope`. If that stays, typing Enter on
line 3 changes the value of every node after line 3, and R3 cannot hold for
any edit that adds or removes a line.

A scope has one consumer: side-by-side editing, which maps an element the
user points at back to its source range. That feature is not on the hot path
of parsing or rendering, and it needs a scope only when it asks for one. So
`Markup.scope` is removed and no node stores a line, a column or an absolute
offset. What a node does carry is the raw extent the engine itself keeps for
its own work, copied as is, because a binding answers scope queries in UI
code where the parser is no longer reachable:

```text
Extent(lead: Int32, span: UInt32)       bytes of UTF-8 source
    lead:   signed, from the end of the previous node in the same relation
            (or the owner's start, for the first node) to this node's start
    span:   of this node's source range
```

- The engine stores exactly these two numbers on every C node, and the
  bindings copy them verbatim, like any other field. No unit conversion and
  no line counting happens while parsing or publishing.
- `lead` is signed because a relative offset between two ranges has no
  sign of its own: ranges may overlap or nest in any way the spec defines,
  and the encoding does not assume otherwise. An inline note's `Footnote`
  covers `^[content]` while its owning `Citation` covers only the content
  (`canonical-ast.md`), so the note's `lead` is −2. No node needs a rule of
  its own for this.
- Neither number changes when text before the node shifts. An edit inside a
  node changes its own `span` (it is a new value anyway); an edit in the
  gap before a node, such as an added blank line, changes that node's `lead`.
  Every other node keeps its value. Extents are part of equality (4.2), so a
  reused node's extent is always the right one.
- `document.scope(of: node, in: source) -> Scope` and
  `document.node(at: Position, in: source)` compute a scope from the extents
  and the source text the document was parsed from, which the side-by-side
  editor already holds (`session.text` for a session's current document).
  They return today's editor line and column conventions and sentinels, in
  the session's coordinate unit (4.4). The first query on a document builds
  its absolute-offset index in one walk, published once under a lock because
  documents are `Sendable` (R8); the line and unit conversion scans the
  source. This cost is paid only by the query.
- Walker callbacks no longer carry a scope.
- A scope is a function of the byte range alone. The empty-document
  `1:1..0:0` and a top-level end at `L:0` (a range ending right after line
  `L-1`'s terminator) follow from the bytes. The one native sentinel that
  does not is a grid or multiline cell whose part of line `L` is blank: its
  end is reported as `L:0` but denotes the end of the cell's segment on line
  `L-1`, a mid-line byte that an ordinary `(L-1):col` end can also name. That
  cell end is therefore reported as its real last byte, `(L-1):col`, and
  `canonical-ast.md` drops the cell-local sentinel.
- The canonical dump prints absolute scopes as today (except for those table
  cells, whose fixtures change), so it is a scope query and takes the source
  like one: `document.dump(in: source)` and `document.dump(node, in: source)`
  on every binding, and `markdown_core_document_dump(document, source, ...)`
  in C.

This is a breaking change to the canonical AST contract and to every binding
(section 10, D1).

### 4.4 Sessions

A session owns a text, its current document and the retained parse state.

```swift
public final class MarkdownSession {           // one writer; not Sendable
    public init(_ source: String = "", unit: TextUnit = .utf16) throws
    public let unit: TextUnit                   // offsets in, columns out
    public var document: Document { get }       // immutable, Sendable
    public var text: String { get }
    @discardableResult
    public func edit(_ edits: [TextEdit]) throws -> Document
    @discardableResult
    public func append(_ text: String) throws -> Document
}

public struct TextEdit {
    public init(_ range: Range<Int>, with text: String)
    public init(_ range: Range<String.Index>, with text: String)
}
```

`edit` is the one way to change a range: a single replacement is a batch of
one edit.

Kotlin has the same shape as an `AutoCloseable` class. ECMAScript exports
`class MarkdownSession` with `dispose()` (and a `FinalizationRegistry`
backstop), because its state lives in WebAssembly memory. C exposes
`markdown_core_session_new`, `_edit`, `_append`, `_document` and `_free`.
C views borrow from the session until its next edit.

- **One coordinate unit.** A session, and every document it publishes, counts
  columns and offsets in one `TextUnit`, `.utf8` or `.utf16`, chosen when the
  session is created. The unit applies in both directions: the offsets an edit
  passes in, and every column a scope query returns (`Document.scope(of:in:)`,
  `Document.node(at:in:)`). An API never takes UTF-16 offsets and
  returns UTF-8 columns. `Document.parse` takes the same parameter. C
  defaults to UTF-8. Bindings default to UTF-16, because the editor surfaces
  on all three platforms count UTF-16 code units: UIKit and AppKit `NSRange`
  and TextKit, Android `Editable` and Compose `TextFieldValue`, Monaco,
  CodeMirror, and the default position encoding of the Language Server
  Protocol. Every binding also offers `.utf8`, and Swift additionally accepts
  `Range<String.Index>` for edits, which carries no unit.
- **Scalar boundaries.** Both ends of an edit range must fall on Unicode
  scalar boundaries in the session's unit: never on a UTF-8 continuation byte,
  never between the two halves of a UTF-16 surrogate pair. Replacement text
  must be well formed: valid UTF-8 in C, and no unpaired surrogate in a
  Kotlin or ECMAScript string. An edit that breaks either rule is rejected as
  an invalid argument. Nothing is rounded to a nearby boundary, because that
  would silently edit a different range.
- **Storage stays UTF-8.** The unit is how positions are counted, not how the
  text is stored. A binding takes its platform's own string. Swift's `String`
  is already UTF-8; Kotlin and ECMAScript strings are transcoded once at the
  boundary, as today. Storing UTF-16 in the engine would double the memory of
  ASCII text and rewrite every byte scanner of the grammar, and it would buy
  nothing, because conversion is cheap where the text is summarized. Editors
  that keep UTF-8 text and serve UTF-16 clients do the same: Zed's rope keeps
  UTF-16 summaries beside byte counts, and tree-sitter's edit coordinates
  follow whichever encoding its input uses. Here the text tree (5.1) keeps
  UTF-16 counts per chunk, so an offset converts in O(log n + line length).
- **Columns in the unit.** Extents are bytes (4.3). A column is converted to
  the session's unit only when a scope query asks for it. The canonical dump
  and the conformance fixtures stay in UTF-8 columns.
- **Batches.** `edit` takes disjoint edits in the coordinates of the text
  before the batch and parses once, for multi-cursor edits and bulk
  replacements. The batch keeps every edit as its own piece of the
  position mapping (5.2), so bytes between two edits stay surviving bytes
  with their own shift. Damage is per edit; regions whose restart and
  convergence windows overlap are merged, and the others are re-read
  independently in source order (5.3).
- **`Document.parse`** keeps its signature apart from the unit parameter. It
  is a session that inserts the whole source once and is then discarded.

### 4.5 Definitions stay where they are written

Today `Document.footnotes` and `Document.specimens` own every footnote and
specimen definition, lifted out of the place where it was written, and an
inline note `^[body]` gets a generated id `inline-N` so its `Citation` can
name the lifted `Footnote`. That lift is what gives definitions their special
cases: the generated ordinal (every later inline note changes when one is
inserted, and an authored `[^inline-3]` label renames a note elsewhere),
positions measured along a list whose entries are scattered across the
document, and identity decided in a list that is not where the parser
produced anything.

The design keeps every definition in the tree where it was written, and the
document answers lookups over its content instead of owning the nodes:

```text
CitationReferent = bib(key, mode) | footnote(FootnoteTarget) | specimen(label)
FootnoteTarget   = label(String) | note(Footnote)

Footnote(label: String?, content: [Markup])
Specimen(label: String?, start: Int?, content: [Markup])
```

- A referenced definition `[^x]: body` is a `Footnote` block in the content
  relation where it was read, like any other block. A specimen definition is
  the same.
- An inline note is a `Footnote` owned at its call site by its referent,
  `footnote(note(Footnote))`, with a null label. A note inside a note is
  ordinary nesting. There is no generated id, no reservation against authored
  labels and no `-K` rule.
- `Footnote.id` and `Specimen.id` are renamed `label`, because every node now
  has `id: MarkupID` (4.1).
- `Document` answers lookups instead of owning sequences:
  `document.footnote(for: label)` and `document.specimen(for: label)` return
  the first definition in source order whose stored label equals the
  referent's (the parser has already normalized both), as the spec defines
  today, and `document.footnotes` and `document.specimens` list every
  definition in source order as references into the tree. These are
  functions of `content`, so they are queries, not data: they take no part
  in equality and nothing transports them. Like `scope(of:in:)` (4.3), the first query on a document builds its
  label index in one walk, published once under a lock, and only the query
  pays for it. In C the session's label registries (5.7) already hold the
  same answer.
- Definitions therefore follow the tree's ordinary identity rules (5.9),
  with no rule of their own.
- Walks and the canonical dump visit a definition where it was written. A
  renderer that prints notes at the end of the page reads the lookup, and
  display numbering stays the renderer's job, as the spec already says.

This changes `canonical-ast.md`, `dialect/footnotes.md`, `dialect/specimens.md`,
the canonical dump of documents with definitions and their fixtures, and every
binding (D2).

## 5. Engine

### 5.1 What a session retains

| State | Contents | Size |
| --- | --- | --- |
| Text tree | The source as a balanced tree of bounded byte chunks; each subtree records its byte, line-terminator and UTF-16 counts | O(source) |
| Tree | The live C tree: ids and each node's extent (4.3) | O(nodes) |
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

Line counts and UTF-16 counts must compose: CRLF is one line terminator, and
a scalar counts as one or two UTF-16 units only when it is whole. The text
tree keeps one boundary invariant for both: **no chunk boundary falls inside a
UTF-8 scalar or between a CR and the LF after it**. Edit endpoints are already
scalar boundaries (4.4). Every operation that creates a boundary (an edit's
split and join, and rebalancing) checks the bytes around it: a split point on
a continuation byte moves back to its scalar's lead byte (at most 3 bytes),
and a boundary between CR and LF moves the LF into the CR's chunk. The chunk
size bound allows for those few bytes. The check is local, so the invariant
costs O(1) per boundary created. With it, each chunk counts its own
terminators and UTF-16 units exactly, a prefix sum always ends on a scalar
boundary, and summed counts equal the parser's line numbering for CR, LF and
CRLF alike.

Every other workspace in `docs/architecture/parser-input-storage.md`
(lookahead facts, table geometry, source-order scratch, delimiter pools)
stays scoped to the edit that re-parses the damaged region. Caches keyed
by line are therefore never stale.

Reading source now goes through the input index for every consumer, as it
already does for the driver, lookahead, Properties and tables. The index
resolves a line against the text tree. A line inside one chunk is borrowed. A
line that spans chunks gets one contiguous view through the mechanism that
already provides normalized views for NUL-bearing lines, so no scanner sees a
chunk boundary. The view lives for the edit, like other scratch.

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
candidates. The line machine then runs from
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
rebuilt. The first spliced node in each relation now follows a re-parsed
predecessor, so its `lead` (4.3) is recomputed from that predecessor's end; it
differs only when the edit touched the gap before it, and then that node
alone becomes a new value with the same id and all of its children reused.

**Re-finalization.** The spine containers are still the new parse's open
nodes. Their closing facts come from the old parse's corresponding closes,
which convergence proves identical. Each container's child summaries (E4) are
kept in its ledger entries, in the same summed balanced tree as the rest of
the ledger (5.1): internal nodes hold the combined summary of their range. An
edit replaces the summaries of the re-read children and recombines up the
tree, so a spine container is re-finalized in O(changed children × log
children), not by walking every child. That is what makes `List.tight`
correct, and cheap, when the edit added a blank line between two early items
of a 10,000-item list and every later item was reused.

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
  one of three things: a cache that the parse may drop; a declaration
  registry that moves to the session (5.7); or carried block state, which is
  stored on the open node, saved into spine snapshots by `carry_save`,
  restored by `carry_restore` and compared by `carry_equal`. Nothing else may
  carry information from one line to a later one.
- **E4 Container finalize is a fold of child summaries.** It reads children
  and recorded facts and writes the container's own fields, and running it
  twice gives the same node. Each container kind declares a per-child summary
  and an associative combine, and its fields are a function of the combined
  summary. `List.tight` is one: a child's summary is (starts after a blank
  line, contains a blank between its own children), and the list is loose
  when any child contains one or any child after the first starts after one.
  Extents of definitions and lists combine from their children's extents.
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

The S1 registrations and S3 declarations move from the parse to the
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
  retiring a root removes its edges. The
  index therefore holds exactly the current document's lookups, and an edge
  never names a retired node. A heading's
  declarability depends only on its own content ("a valid declaration cannot
  depend on a reference lookup", `heading-resolution.md`), so this settles in
  one round, with no fixed point.
- **Anchors by family.** Generated anchors interact only through their
  spelling. A family is the set of spellings with the same stem after
  stripping trailing `-N` groups. An edit recomputes, in source order, only the
  families of changed headings and changed explicit anchors, with the same
  reservation and suffix-cursor algorithm as today. The resource's occurrences
  follow through the lookup index.
- **Resources are values.** A shared resource (a definition's destination,
  title and attributes, or a heading target) is not an AST node and has no
  identity: it is a value, equal to another resource exactly when its fields
  are equal, and occurrences that share one only share storage for an equal
  value (interning by content). A declaration that changes therefore yields a
  different value; there is nothing to update. Today
  `markdown_core_headings_finish` instead treats the heading target as a
  mutable object and rewrites its URL in place. Reuse (5.9) assumes that a
  node's content cannot change without the node being rebuilt, so in a
  session that write would change Links the engine keeps as unchanged, so
  the new document's Links would disagree with a fresh parse. The session therefore builds the heading target's value
  once its anchor is final, and the Links that looked it up are re-parsed
  against the new value through the lookup index.
- **Definition lookups.** The footnote and specimen label registries are
  source-ordered sequences like the others; a changed winner queues its
  dependents. Definitions themselves stay in the tree, so nothing is spliced
  into the document and no ordinal is recomputed.

### 5.8 Finish steps and passes

The finish walk runs on the inline roots and block subtrees that were re-read
or re-resolved, never on reused ones. Consolidation, script-escape
completion, list layout, formula promotion and the Autolink email pass all act
within one root, so their results for a reused root are already in the reused
tree. `check-finish-hook-shapes.mjs` gains the rule that a finish step or pass
reads only its root and the registries, which is what lets them run per root.

Within a root re-parsed from its stable prefix (5.6), finish resumes at the
prefix too; otherwise every streamed chunk would re-finish the whole
paragraph, and a long paragraph would cost quadratic work. Finished nodes
wholly before the prefix are kept as they are. The finish walk visits the
nodes the inline parse rebuilt (the suffix and the path of containers that
straddle the prefix) plus, at each level of that path, the one finished
sibling immediately before them. That is exact because of a second rule the
audit enforces: **a finish step's result for a node depends only on that node
and its immediately preceding sibling**, after the step has run on that
sibling. Text consolidation merges a node into its predecessor, the email
pass scans one consolidated Text, and the others read one node. The left
sibling at the prefix ends no later than the start of the last line, where
SoftBreak splits Text, so resumed finish work is of the same order as the
inline re-parse. A step that needs a wider window must say so in its hook
shape, and the stable prefix then moves back by that window.

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
  Siblings in one parsed relation have disjoint ranges, so an anchor image
  lies in at most one candidate.
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
equal scalars, equal extent, and every child relation holding the same
objects.
If they are equal, `N` is released and `O` stays. Within the C session, a
node that differs from its predecessor as an object therefore differs as a
value, which is what R3 measures and the work counters (8) count.

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

### 5.11 Session state

An edit changes session state in place: the text tree, the live tree, the ledger, the registries, the lookup index, the
frontier and the inline ledger. Nodes and elements it removes are freed when
it completes.

## 6. Bindings

### 6.1 Bindings build values from the C document

After an edit, a binding converts the session's C document into platform
values node by node, exactly as it converts a fresh parse today, and returns
the result. The C document is its only input. The conversion's cost is the
platform's cost of building an immutable value, which the engine's design
does not count or optimize (D5). Nodes the edit left unchanged reach the
consumer as equal values with unchanged ids (4.2), which is what SwiftUI,
Compose and React reconcile.

### 6.2 Wire format MCB3

Kotlin and ECMAScript receive a parse as one message. MCB3 extends MCB2
(`docs/architecture/wire-format.md`) and keeps its post-order stack model.
Every node record adds `u64 id` and the node's `Extent` in place of `Scope`.
Every message is a whole document. The magic becomes `MCB3` because the
record layout changes.

### 6.3 Swift storage

The Swift AST was originally a tree of per-node objects, as Kotlin and
ECMAScript still are. #240 (issue #233) replaced it with one flat
`StoredMarkup` array owned by a `MarkupStore`
(`docs/architecture/swift-storage.md`) for one reason: ARC released the tree
recursively, and a 65,536-level chain overflowed the stack. The flat store
fixed that symptom by changing the data model, and brought the indirection
of stored field references and store ownership with it.

The design restores the tree: one immutable final class record per node that
holds its scalars and references to its children's records, with liveness by
ARC. The stack bound is kept by fixing its cause instead, with one rule
stated once and applied to every operation that follows tree edges: **no
operation recurses over tree edges**.

- **Release.** Every record inherits one internal base, `MarkupRecord`, that
  holds all of the node's owned relations in storage only the base can empty.
  Its `deinit` moves its own children into a local array and drains it: for
  each child it takes out, if `isKnownUniquelyReferenced` holds, it first
  moves that child's children onto the array, so when the child is dropped its
  own `deinit` has nothing to release. A child still referenced elsewhere
  (retained by a view) is only
  released, which ends at a count decrement. Stack depth is constant in tree
  depth; the array holds at most the nodes being freed. Moving children out is
  the only mutation, and it happens only to a record that nothing else
  references, inside `deinit`. Records are therefore immutable to every
  observer and `Sendable` (`@unchecked`, with the invariant stated at its one
  use and an audit that no other code writes the storage).
- **Traversal.** Deep equality, the walker, conversion (6.1), the scope
  index and hit testing (4.3), the label index (4.5) and `description` use
  explicit work stacks. Hashing reads only the id. Kotlin (`equals`, `toString`) and
  ECMAScript (`markupEquals`) follow the same rule, because their stacks are
  finite too; their garbage collectors need no rule for release.
- **Gate.** The existing 30,000 and 65,536-level tests extend from release to:
  releasing a deep document while a view still holds one of its subtrees,
  equality of two deep documents that differ only at the deepest leaf,
  walking, the first `scope(of:in:)` query (which builds the offset index),
  `node(at:in:)` and `description`, on every binding. They
  run on a thread with a small fixed stack, so a recursion regression fails
  deterministically instead of depending on the platform's default stack size.

Kotlin and ECMAScript already hold one object per node, so this brings Swift
to the same model instead of keeping a Swift-only layout. The cost is one
allocation per node on a fresh parse where there used to be one per document;
the benchmark gate measures it.

### 6.4 Kotlin

Every kind gets reference-first `equals` and id-based `hashCode`. Collections
stay read-only `kotlin.collections.List`. The package ships a Compose
stability configuration file that lists the AST types as stable, because the
Compose compiler treats classes from a module without the Compose compiler as
unstable. No Compose dependency is added.

### 6.5 ECMAScript

Values stay plain readonly objects. Lists key by id, and `React.memo` takes
`markupEquals` as its comparator (4.2). The session holds a WebAssembly handle and must be disposed. A
`Document` never holds a handle, as now.

## 7. Complexity

### 7.1 Bounds

Let `d` be the depth of the edit point, `F` the sum of child counts along the
changed paths, `L` the size of the re-read leaves, `W` the lookahead window
before convergence, and `k` the size of inline roots invalidated by
resolution changes.

| Operation | Block work | Inline work | Resolution | C tree changes |
| --- | --- | --- | --- | --- |
| Append `c` bytes inside an open paragraph | O(c + last line) | O(c + distance to stable prefix) | O(changed declarations) | O(d + F) |
| Append that closes and opens blocks | O(c + last line + closed leaves) | as above | as above | O(d + F) |
| Edit inside one closed leaf | O(L + W) | O(L) | O(changed declarations + k) | O(d + F) |
| Edit that changes container structure | O(blocks until convergence) | O(re-read leaves + k) | as above | O(changed nodes + F) |
| Fresh parse | O(n), as today | O(n) | O(n) | O(n) |

The text tree, the ledger and the registries add O(log n) per lookup,
insertion, deletion and shift, wherever the edit is. A binding builds each
published document whole (6.1); that O(nodes) is the platform's construction
cost and is outside these bounds. No bound depends on a size threshold.

### 7.2 Costs that are the language's, not the algorithm's

Some edits really do change most of the document, and the design does not
pretend otherwise:

- Opening an unclosed fence, HTML block, comment or directive block near the
  top changes the meaning of everything after it until something closes it.
- A definition referenced 10,000 times changes 10,000 Links when its
  destination changes.
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
  `Document.parse` of the session's text in the same unit. This runs in C
  and in each binding, in both units.
- **Streaming.** Every corpus document is fed in chunks of every size from
  one scalar up, and split at every scalar boundary for small documents.
  Every intermediate document must equal a fresh parse of the session's
  text.
- **Identity and minimality.** After every edit: ids are unique; no id
  changed kind; in C, every reused node equals the fresh-parse node at the
  same position and every matched node that is a new object differs in value
  from its predecessor; in every binding, every node equals the previous
  document's node with its id exactly when C reused it. For scripted edits
  the exact set of new C nodes is asserted
  (for example, typing in paragraph 5 of 1,000 replaces that paragraph, its
  Text nodes on the edited line and the Document). Scripted cases include
  unwrapping a nested inline note (the inner note is a new node, because its
  owner changed, 4.1), inserting a
  line at the top of a long document (only the Document and the edited
  paragraph are new values), and changing a heading anchor that Links target (every such Link is a new
  value carrying the new destination).
- **Work counters.** Deterministic counters, like the existing
  `input_line_work` and `delimiter_work`, gate lines re-read, inline bytes
  re-parsed, child summaries recombined, finish nodes visited and C nodes
  replaced per edit against the bounds of 7.1,
  including adversarial shapes: a stray early opener, a 10,000-item list edited
  in the middle, 1,000 nested block quotes, a definition with thousands of
  references.
- **Definition queries.** After every edit, `footnotes`, `specimens` and
  `footnote(for:)` and `specimen(for:)` for every label in the text equal
  those of a fresh parse, and fixtures with nested, duplicate, anonymous and
  unreferenced definitions check each binding's answers against the winners
  of the C registries, since the canonical dump does not call the queries.
- **Allocation failures.** The allocator-seam OOM sweep fails every edit at
  every allocation boundary and asserts that the edit throws and that
  freeing the session leaks nothing.
- **Audits.** E1–E5 (5.4), the finish-step root rule (5.8), and the
  dependency inventory (section 3) are enforced by scripts in
  `scripts/audit/`.

## 9. Rollout

Each step is one pull request that leaves `main` releasable.

- [ ] **Step 1: Model.** Ids for fresh parses, deep equality and hashing, raw extents
   in nodes with on-demand scope queries, MCB3, and the Swift record
   storage, the coordinate unit (4.4), and definitions kept where written
   (4.5). The canonical dump and conformance fixtures change only for
   documents with footnote or specimen definitions and for grid and
   multiline table cells that end on a blank line part (4.3), whose
   `canonical-ast.md` rule changes in the same step.
- [ ] **Step 2: Sessions with a whole-document restart.** Session API on every platform,
   the text tree, identity matching,
   and value deduplication. The restart
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

- **D1 Positions. Decided 2026-09-29: raw extents, scopes on request.** Nodes
  carry only the engine's own byte extent (signed lead from the previous
  sibling, span), copied verbatim to every binding. `document.scope(of:in:)`,
  `document.node(at:in:)` and the canonical dump `document.dump(in:)` compute today's editor line and column scope from
  it on request, because side-by-side editing is the only consumer and is
  not on the hot path. Rejected: absolute scopes in nodes, which replace
  every node after an inserted line, and line and column spans in nodes,
  which make every parse and publish count lines and convert units.
- **D2 Definitions. Decided 2026-09-29: definitions stay where written.**
  Footnote and specimen definitions remain in the tree where they were
  written, an inline note's `Footnote` is owned at its call site, and the
  document answers label lookups as queries over its content instead of
  owning the definitions (4.5).
  Rejected: keeping `inline-N`, where inserting one note changed every later
  note; a separate `InlineNote` kind, which would express footnote semantics
  with a second model; and lifted definitions named by `MarkupID`, which kept
  special identity and position rules for the lifted list.
- **D3 Swift storage. Decided 2026-09-29: per-node records,** on the
  condition that no operation recurses over tree edges (6.3). This restores
  Swift's original tree, which #240 had flattened only to bound ARC release
  depth; iterative release bounds it directly.
- **D4 Coordinate unit. Decided 2026-09-29: one unit per session.** Edit
  offsets and returned columns use the same unit, UTF-16 by default in
  bindings and UTF-8 in C. Text is stored as UTF-8, and the C text tree keeps
  byte and UTF-16 counts so conversion happens once, in C (4.4). The rejected
  alternative was UTF-8 everywhere, which leaves every editor integration to
  convert `NSRange` and JavaScript offsets itself.
- **D5 Bindings. Decided 2026-09-29: every binding builds each document
  whole from C (6.1).** Unchanged nodes reach consumers as equal values with
  unchanged ids, and frameworks compare them with deep equality. Rejected:
  sharing objects between successive documents, which would give the
  binding state of its own (a table of previous objects, or the previous
  document as a second input) and the engine reports to maintain it.

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
