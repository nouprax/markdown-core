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
| S1 Blocks | `S_parse_source` → `S_process_line`: `check_open_blocks`, `open_new_blocks`, `add_text_to_container` | One physical line at a time through the input index, plus lookahead (`markdown_core_parser_lookahead_*`) and claimed ranges (`claimed`) | The block tree on one open-node spine; leaves accumulate content; reference definitions, headings, footnote and specimen definitions register |
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

What is missing is (a) a record on each node of what its parse depended on,
(b) a re-parse that takes unchanged subtrees of the old tree whole, (c)
resolution that can be recomputed for part of a registry, and (d) identity.
Tree-sitter has (a) and (b) for any grammar: each subtree records its parse
state and how far past its end the lexer looked, and a re-parse walks the old
tree beside the input and reuses every subtree those records prove
unchanged. Section 5 applies the same algorithm and the same tree storage to
the line machine.

The stages after S1 are cmark's: a walk over the finished document (S4), a
walk per pass (S6) and, since step 1, a walk that numbers nodes and writes
their extents. Tree-sitter has none of these. A node is complete when the
parser makes it, and nothing visits the tree after the parse. Section 5.8
makes every node complete when it is made, which removes those walks.

## 3. Dependency inventory

Incremental parsing is correct only if every way that one part of the source
affects a node elsewhere is known and handled. This is the complete inventory
for the current dialect. Each row names the mechanism in section 5 that
handles it.

| Dependency | Examples | Direction | Mechanism |
| --- | --- | --- | --- |
| Carried block state | Open list item widths, blockquote and callout prefixes, definition body indentation, directive fence stack, last-line-blank flags | Earlier line → later lines | Entry state on every node; a node is taken only under an equal entry (5.3, E3) |
| Lookahead | Setext underline, table separator and caption, definition term marker, multiline and grid tables, directive closer, Properties closing fence | Later lines → earlier decision | Reach on every node (5.1, 5.3) |
| Retroactive write | A separate-line block identifier sets the preceding block's anchor | Later line → closed node | Reach raised by the write service (E2) |
| Container facts from children | `List.tight`, list layout, definition and definition-list scopes | Children → container | Summaries folded in the children tree (5.3, E4) |
| Unclosed opaque leaves | Fenced code, HTML block, comment, formula block and directive block without a closer run to the end | Opener → rest of document | No special case: nothing after the opener is taken until the live state equals an old entry again, which is the language's meaning (7.2) |
| Registry lookups in inline parsing | `[label]` and heading labels in the reference map (`link.c`, `heading.c`), `[^label]` in the footnote label map (`footnote.c`), `@id` in the specimen id index (`citation.c`) | Any definition → any occurrence | Lookup dependency index (5.7) |
| Document ordinals | `inline-N` footnote ids, heading anchor `-N` suffixes, footnote and specimen order | Earlier declarations → later values | `inline-N` and the lifted footnote and specimen sequences are removed from the model (4.5); anchor suffixes by recomputation by family (5.7) |
| Shared resources | A definition's destination, title and attributes read by every occurrence | Definition → occurrences | Lookup dependency index (5.7) |
| Absolute coordinates | Every `Scope` after an inserted or deleted line | Every earlier byte → every later scope | Positions leave the AST (4.3) |
| Mapped cell inputs | Grid and multiline cell bodies | Table → cells | Grid and multiline geometry is a fold over the table's lines (E6) |

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
  in the order the parse completes them (5.8). Two fresh parses of the same
  text are equal, identifiers included.
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
  the session's coordinate unit (4.4). Each query computes absolute offsets
  in one walk over the extents, and the line and unit conversion scans the
  source. This cost is paid only by the query.
- Walker callbacks no longer carry a scope.
- A scope is a function of the byte range alone, with one rule for every
  node: a range ending right after line `L-1`'s terminator ends at `L:0`,
  so `SoftBreak`, `LineBreak` and a `Citation` that end on a line terminator
  end there too, and a zero-byte document is `1:1..1:0`, as a document of one
  newline is. A grid or multiline cell whose part of line `L` is blank used
  to end at `L:0` while denoting the end of the cell's segment on line
  `L-1`, a mid-line byte that an ordinary `(L-1):col` end can also name.
  That cell end is therefore reported as its real last byte, `(L-1):col`,
  and `canonical-ast.md` drops the cell-local sentinel.
- The canonical dump prints absolute scopes by this rule, so it is a scope
  query and takes the source like one: `document.dump(in: source)` and `document.dump(node, in: source)`
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
- **Edit ranges.** Both ends of an edit range are offsets into the text in
  the session's unit, and the batch's ranges are disjoint. An edit whose start
  is after its end, whose end is past the text, or that overlaps another edit
  of the batch is rejected as out of bounds. An offset inside a scalar, at a
  continuation byte in UTF-8 or between the two units of one scalar in
  UTF-16, is rejected with its own status, inside a scalar. Nothing is rounded
  to a nearby boundary, because that would silently edit a different range.
  The engine treats the text exactly like a valid stream, with no validation
  and no conversion: replacement text is stored as given, a UTF-8 offset is a
  byte offset, and a UTF-16 offset counts each byte as the units of the scalar
  it begins, which is how scope queries count columns.
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
  with their own shift. Damage is per edit, and one parse covers every edit
  of the batch (5.3).
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
- `Document` carries the parser's footnote and specimen tables: the ids of
  every definition in source order, as the C registries (5.7) hold them. A
  binding receives them with the document (6.2) and resolves them to its
  nodes while it builds the tree (6.1). `document.footnotes` and
  `document.specimens` list those definitions in source order, and
  `document.footnote(for: label)` and `document.specimen(for: label)` return
  the first one whose stored label equals the referent's (the parser has
  already normalized both), as the spec defines today.
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
| Tree | The document as shared immutable nodes (5.11); each node holds its id, its extent (4.3) and its parse record | O(nodes) |
| Registries | Reference, heading, anchor, footnote and specimen declarations in source order; label winners | O(declarations) |
| Lookup index | Registry key → inline roots that looked it up, hit or miss | O(lookups) |

The tree is the record of the parse, as it is in tree-sitter, where each
subtree carries its parse state and how far past its end the lexer looked,
and nothing else survives a parse. Every block node records three things when
it is built:

- its **entry**: the carried state (E3) of its parent at the start of its
  lead, where its previous sibling ended;
- its **head**: its own state after its opening line, and its **head end**,
  the furthest byte the decisions on that line read;
- its **reach**: how far past its end any decision about it read, including
  lookahead, claimed ranges, the line that closed it and every later line
  that wrote into it (E2). A container's reach covers its children's.

The input index keeps a high-water mark of the last byte any decision has
read, so heads and reaches are measured, not declared: an element cannot
forget one. Inline nodes record an entry and a reach the same way (5.6). A
fresh parse writes these few words per node and nothing else.

The text, each container's children and the source-ordered registries are
sequences whose elements have source extents. They share one structure: a
balanced tree whose elements store their length (for text, a chunk's bytes;
for children, a child's lead and span; for a registry entry, the byte
distance from the previous entry) and whose internal nodes store the sums.
For children, the internal nodes also store the furthest reach past their
range's end and the combined summary of their children (E4). Absolute
offsets, line numbers and UTF-16 offsets are prefix sums. Finding an offset,
inserting, deleting, shifting everything after an edit, finding the first
child whose reach meets an edit and combining the summaries of a run of
children all cost O(log n), wherever the edit is, so alternating edits at
opposite ends of a document cost the same as edits in one place. There is one
such structure, not one per consumer. (A gap buffer, or a sorted array with
one lazy shift, would move Θ(n) bytes or keys whenever consecutive edits are
far apart.) The internal nodes of a children tree are storage, like
tree-sitter's hidden repetition nodes: walks, the canonical dump and the
bindings see the children in order.

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
lives for one edit's parse. Caches keyed by line are therefore never stale.

The parser reads the text tree through the input index a line at a time, as
tree-sitter's lexer reads its input a chunk at a time. A line inside one chunk
is borrowed. A line that spans chunks gets one contiguous view through the
mechanism that already provides normalized views for NUL-bearing lines, so no
scanner sees a chunk boundary. The view lives for the edit, like other
scratch. An edit reads only the lines it reads again.

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

### 5.3 Blocks: re-parse against the old tree

An edit parses the new text from its start with the ordinary line machine and
a cursor over the old tree, as tree-sitter re-parses a file against its old
tree. Wherever a node's record proves that reading it again would give it
back, the node is taken whole instead of read.

**Changed nodes.** An old node is changed when its range, from the start of
its lead to its end plus its reach, meets or touches a replaced range of the
batch (5.2). A container that holds a changed node is changed, because its
range and reach cover its children's. The cursor finds a container's first
changed child, and the run of unchanged children before the next changed
one, from the container's children tree in O(log children).

**The cursor.** The cursor is a stack of (old node, child position, old
offset), like tree-sitter's reusable node, and it moves only forward. A
**boundary** is a point where the live parse has no open block below its
innermost open container: the start of a reopened container's children, the
end of a taken run, and the point inside a read line where the line has
closed the previous child and opens a new one. At a boundary at position `p`
the cursor presents, outermost first, the old nodes whose lead starts at the
image of `p`, and for each one of three things happens:

- **Take.** The node is not changed; its entry, its parent's entry and so on
  up to the document equal the carried state of the live spine at each
  depth; and, at a boundary inside a read line, the block the line opened
  has the node's head. The node is taken whole, with the run of unchanged
  siblings after it, and the parent folds the run's combined summary (E4)
  into its carried state. The block the line opened is dropped, and the
  parse continues after the run without reading any line of it. The rest of
  the run needs no comparison: equal state before a node and identical bytes
  through its reach give equal state after it, which is its next sibling's
  entry.
- **Reopen.** The node is a changed container whose entries are equal as for
  a take, and either its head end is before its first changed byte or, at a
  boundary inside a read line, the block the line opened has its head. The
  container goes back on the live spine with its head state, without its
  opening line being read again, and the cursor descends to its children.
  From step 6 a changed leaf whose entries are equal also reopens, at its
  first changed line (E5).
- **Read.** Otherwise the line machine reads the line with
  `S_process_line`, as a fresh parse does.

**Closing.** When the cursor has taken the last child of a reopened
container, the container's state equals the old container's at the same
point, so it closes where the old one did, on a line its last child's reach
already covers, and that line is not read. A reopened container whose last
lines are read closes when the line machine closes it. Either way its
summary is recombined from its children tree in O(changed children × log
children). That is what makes `List.tight` correct, and cheap, when an edit
adds a blank line between two early items of a 10,000-item list and every
later item is taken.

**Leads.** The first node of a taken run that follows a read node gets its
`lead` (4.3) from that node's end. The lead differs only when the edit
touched the gap before the node, and then that node alone becomes a new value
with the same id, with all of its children shared.

**Tables.** A pipe table is a container: its header and delimiter rows are
its head, which fixes its columns, and its rows are its children. Grid and
multiline tables decide their geometry from all of their lines, which is a
fold (E6), and each of their rows records that geometry as its entry. Cells
are internal inputs of the table's transaction, as now.

**Several edits.** A batch (4.4) is one parse. The cursor sees every edit of
the batch, and the nodes between two edits are taken like any others.

**Degenerate cases are the same algorithm.** A fresh parse has no old tree,
so every line is read. An edit in the Properties envelope changes the
document's first block. An opener of an unclosed fence is followed by no
taken node until something closes it, because the rest of the document
really did change meaning.

**Registrations.** The registries are source-ordered sequences held by the
session (5.7), so the registrations of a taken node stay as they are and
those of the old nodes that were read are replaced. Until the lookup index
exists (step 5), every inline root depends on every key: when the
registrations of the read nodes differ from the ones they replace, every
inline root is parsed again.

### 5.4 The element contract

The engine can take and reopen only what elements make deterministic. These
are requirements on every element, each checked by an audit script in
`scripts/audit/` in the style of `parser-boundaries.mjs`:

- **E1 Reads go through the index.** Source reads, including lookahead and
  claimed ranges, go through the input index, so the high-water mark sees
  them and every head end and reach is measured. An element never keeps a
  raw pointer into the source across lines.
- **E2 Retroactive writes go through one service.** Changing a node that has
  closed (the separate-line block identifier is the current case) uses
  `markdown_core_parser_write_closed(parser, node)`, which copies the node
  when it is shared (5.11) and raises its reach to the current line. The
  audit forbids other writes to closed nodes.
- **E3 Carried state is a head and a fold.** Per-parse element state
  (`state_size`) is one of three things: a cache that the parse may drop; a
  declaration registry that moves to the session (5.7); or carried block
  state. A container's carried state where a child can start is a function of
  its head, the word `carry_save` returns after its opening line, and the
  combined summary (E4) of its closed children, whose leads hold the blank
  lines before them. Nothing else carries information from one line to a
  later one. Equal heads and equal summaries are equal states, so every
  comparison in 5.3 compares words.
- **E4 Container finalize is a fold of child summaries.** It reads children
  and recorded facts and writes the container's own fields, and running it
  twice gives the same node. Each container kind declares a per-child summary
  and an associative combine, and its fields are a function of the combined
  summary, which its children tree keeps (5.1). `List.tight` is one: a
  child's summary is (starts after a blank line, contains a blank between its
  own children), and the list is loose when any child contains one or any
  child after the first starts after one. Extents of definitions and lists
  combine from their children's extents.
- **E5 Leaf finalize keeps the accumulation.** A leaf's value is produced from
  its accumulated content without destroying it, and the closed leaf keeps
  its content and content map, so it can reopen at a later line: its content
  is cut at that line and reading continues there. Paragraph already records
  its consumed reference-definition prefix as a persistent fact;
  trailing-whitespace trimming becomes a length, not a truncation of the
  buffer.
- **E6 A decision over a node's own lines is a fold.** When a decision reads
  all of a node's lines (the column geometry of a grid or multiline table),
  each line has a summary, the node keeps the summaries in a children tree
  of its lines, and the decision is a function of their combined summary,
  made when the node closes. The rows built from the decision record it as
  their entry. An edit reads the lines it changed, recombines in O(log
  lines), and takes every row whose lines are unchanged while the decision
  is unchanged.

### 5.5 Streaming is an edit at the end

An append inserts at the end of the text. A node that was open when the
input ended read to the end, so its reach touches the insertion, and the
changed nodes are exactly the open spine. The cursor reopens the spine's
containers, whose heads are before the end, takes their closed children in
runs, and reads from the last open leaf. Until step 6 that leaf is read
whole; from step 6 it reopens at its last line (E5), so an append costs the
last line, the new bytes and the inline work of 5.6. Blocks that the
appended text closes close normally.

Every published document is a finished parse of the session's text. A
paragraph that is still growing keeps its id from its first line to its last
by the matching rule (5.9).

### 5.6 Inline parsing

An inline root is parsed again when its block was read again, or when a
registry winner it looked up changed (5.7). Every other root keeps its inline
tree.

From step 6 a root that is parsed again is parsed against its old inline tree
by the algorithm of 5.3. Each inline node records its **entry**, the
delimiter state at its start, and its **reach**, the furthest content offset
any decision about it read:

- a delimiter run that can still open or close, a citation token, a field
  and an unclosed bracket reach the end of the content, because a later
  closer can still pair with them;
- a token decision that stopped at the end of the content (an unmatched
  backtick run, an unclosed HTML or comment token, a formula without a
  closer, or any scanner that stopped at the slice limit rather than at a
  byte) reaches the end of the content;
- a line's trailing whitespace reaches the next line, because it is a hard
  break inside a paragraph and is trimmed at its end.

An inline node is changed when its range plus its reach meets the changed
bytes of the content. The parser reads the content from its start with a
cursor over the old inline children: an unchanged node whose entry equals the
live delimiter state is taken with its run, and everything else is read. The
state after a taken run equals the old state there, by the argument of 5.3.
The entry is small because the delimiter model already summarizes the
entries that can no longer pair as one floor per range
(`docs/architecture/inline-delimiters.md`).

Typical streamed prose closes its delimiters within a few words and its Text
nodes are split per line by SoftBreak, so the per-chunk inline work is about
the size of the current line. An early opener that never closes is read
again on each append, together with the nodes whose entry it changes when a
new closer pairs with it.

### 5.7 Registries and resolution

The S1 registrations and S3 declarations move from the parse to the
session and become source-ordered sequences (5.1). An entry holds the
declaration's values and its node's id. An edit replaces exactly the entries
of the nodes it read, which is one contiguous range per registry and edit,
and keeps the entries of taken nodes. Then:

- **Winners.** For each normalized label whose entries changed, the first
  definition in source order is recomputed, with explicit definitions before
  implicit heading targets as today. The same happens for footnote labels and
  specimen ids.
- **Lookup dependencies.** During inline parsing every registry query records
  `(registry, key) → inline root`, whether it hit or missed. A miss matters as
  much as a hit: adding `[x]: /u` turns every `[x]` into a Link. When a
  winner changes, its dependents are parsed again (5.6). The index
  holds edges in both directions: each inline root owns the list of keys it
  queried, and each key the set of root ids that queried it. Parsing a
  root again first removes all of its old edges and then records the new ones;
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
  mutable object and rewrites its URL in place. A taken node is shared with
  the old tree (5.11), so in a session that write would change Links the
  engine takes as unchanged, and the new document's Links would disagree
  with a fresh parse. The session therefore builds the heading target's
  value once its anchor is final, and the Links that looked it up are parsed
  again against the new value through the lookup index.
- **Definition lookups.** The footnote and specimen label registries are
  source-ordered sequences like the others; a changed winner's dependents
  are parsed again. Definitions themselves stay in the tree, so nothing is
  spliced into the document and no ordinal is recomputed.

### 5.8 Nodes are complete when they are made

A tree-sitter node is complete when the parser reduces it: its children, its
size and its padding are set then, and nothing changes it afterwards. The
engine makes its nodes the same way, so no stage walks the tree after the
parse.

- **Blocks complete when they close.** Closing a block runs everything that
  decides it: the element's close (a formula block's literal; a code block
  whose info names a formula becomes a FormulaBlock), the container's fold of
  its children (E4: list layout, definition scopes), its id, and the extents
  (4.3) of the nodes it holds, which keep their absolute places until then.
  The document root's extent is measured from 0. A paragraph that held only
  definitions is not added to its parent.
- **Inline roots complete when their parse ends.** A block's inline content
  and each inline field of a block (a definition's term, a callout's title, a
  table's caption, a directive's label) is an inline root. The closing block
  adds it to the parse's list of roots with its absolute start. After S3,
  each root on the list is parsed. Delimiters decide nesting only when a
  closer pairs, and the language gives an inline node meaning from what
  encloses it: an escaped space inside a word body, a Text outside a Link for
  email autolinks, and runs of Text that become one. So an inline node is
  complete when its root's parse ends, and the root completes its own tree
  then, in one pass over that tree: consolidation, completion, email
  autolinks, extents and ids. The root's owner measures the root's content
  from the start it recorded. A paragraph whose only content is a standalone
  formula becomes a FormulaBlock at that point.
- **Absolute positions belong to the parse.** A node holds only its extent
  once it is complete. Everything that needs an absolute position after that
  (source-ordered registrations, a root on the list, table geometry) records
  it when it is made.
- **Document facts come from the registries.** S5 resolves anchors and
  builds the footnote and specimen lookup tables from the source-ordered
  registries, whose entries name complete nodes.

A fresh parse is the block parse, the inline parse of each root, and S5.
`check-finish-hook-shapes.mjs` becomes the audit of these hooks: a close step
reads its block and the block's children, and a completion reads one inline
root.

For a root parsed against its old inline tree (5.6), taken inline nodes are
already complete, and completion runs over the nodes the parse read and the
Text before and after each read run, which consolidation may merge with them.

### 5.9 Identity matching and value deduplication

A taken node is the old node (5.11), so it and its whole subtree keep their
ids wherever the parse puts them. Each node that was read is matched to an
old node. This is the one comparison of the old and new trees, the
counterpart of tree-sitter's `ts_tree_get_changed_ranges`: it walks the two
trees together and steps over every subtree they share by reference, so it
visits only what the parse read.

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
- A read node took a new id when it was completed (5.8). A matched node takes
  its old node's id instead. Read children of an unmatched owner keep their
  new ids. A paragraph that moves
  into a new blockquote is read again, because its entry changed, and is a
  new node, as it is to every UI framework.

Then, in post-order, each matched read node `N` is compared with its `O`:
equal kind, equal scalars, equal extent, and every child relation holding the
same objects. If they are equal, `N` is released and `O` is shared in its
place. Within the C session, a node that differs from its predecessor as an
object therefore differs as a value, which is what R3 measures.

### 5.10 Why the result equals a fresh parse

- Blocks: every line the edit reads is read by the fresh parse's line
  machine. A taken node is exact by induction along the parse: at a take,
  the live state equals the old state at the start of the node's lead (equal
  entries and E3), the bytes from there through its reach are unchanged,
  and the line machine is a deterministic function of its state and the
  bytes it reads (E1, E2), so it would build the node again and leave the
  same state after it. A reopened container's head state is the one its
  unchanged opening line produced. A container's fields are a fold of its
  children's summaries (E4), and a decision over a node's lines is a fold of
  its lines' summaries (E6).
- Inline trees: a kept root's content and lookup answers are unchanged. A
  root parsed again runs the same parser against the same registries, and
  its taken nodes are exact by the same induction.
- Resolution: winners, families and ordinals are recomputed over complete
  source-ordered registries with the same rules.
- Finish: per-root steps on unchanged roots gave the same results before.

This argument is also the test oracle (section 8).

### 5.11 Session state and memory

The session's state is the text tree, the tree, the registries and the
lookup index. Between edits the parser holds nothing else: like
tree-sitter's `TSParser`, which is reset after every parse, its spine,
cursor, input index and scratch live for one edit, and its lasting storage
is the node pool's free slots.

The tree is stored the way tree-sitter stores its syntax trees:

- **Nodes are shared immutable values.** A node holds a reference count, its
  id, kind, extent, parse record and fields, and its children tree. It has no
  parent, previous-sibling or next-sibling link and no absolute position, so
  one subtree can sit in the old tree and the new one at once. Walks carry
  their path on an explicit stack, and no operation recurses along tree edges
  (D3).
- **Taking is a reference.** Taking a node retains it, in O(1). Taking a run
  of children retains the few internal nodes of the old children tree that
  hold it and joins them into the live container's children tree, in
  O(log children).
- **Writes copy what is shared.** A change to a node referenced once happens
  in place. A change to a shared node first copies its header and its
  references to its children, retaining each, like `ts_subtree_make_mut`.
  An edit copies only the paths from the root to what changed.
- **Open blocks are builders.** A block is mutable only while it is open on
  the parser's spine. When it closes it is complete (5.8) and becomes a node
  with a reference count of one, with its children tree balanced as it was
  built. An inline root's block is complete except for its inline content,
  which its root's parse completes.
- **Release is iterative.** Releasing a node decrements its count. At zero,
  its children are released the same way from an explicit stack, and the
  freed slots return to the session's pool.

An edit retains the old root for its parse, builds the new root, makes it the
session's document and releases the old root: nodes that only the old tree
held are freed, and nodes shared with the new tree live on.

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
The message ends with the footnote and specimen tables (4.5).
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
- **Traversal.** Deep equality, the walker, conversion (6.1), scope
  queries and hit testing (4.3) and `description` use
  explicit work stacks. Hashing reads only the id. Kotlin (`equals`, `toString`) and
  ECMAScript (`markupEquals`) follow the same rule, because their stacks are
  finite too; their garbage collectors need no rule for release.
- **Gate.** The existing 30,000 and 65,536-level tests extend from release to:
  releasing a deep document while a view still holds one of its subtrees,
  equality of two deep documents that differ only at the deepest leaf,
  walking, `scope(of:in:)`,
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

Let `d` be the depth of the edit point, `b` the largest child count along
the paths from the root to the changes, `L` the size of the read leaves, `W`
the lines read after the damage before the next take, and `k` the size of
inline roots invalidated by resolution changes.

| Operation | Block work | Inline work | Resolution | C tree changes |
| --- | --- | --- | --- | --- |
| Append `c` bytes inside an open paragraph | O(c + last line + d log b) | O(c + last line + open delimiters) | O(changed declarations) | O(d log b) |
| Append that closes and opens blocks | O(c + last line + closed leaves + d log b) | as above | as above | O(d log b) |
| Edit inside one closed leaf | O(L + W + d log b) | O(L) | O(changed declarations + k) | O(d log b) |
| Edit that changes container structure | O(lines read + d log b) | O(read leaves + k) | as above | O(read nodes + d log b) |
| Fresh parse | O(n), as today | O(n) | O(n) | O(n) |

The text tree, the children trees and the registries add O(log n) per
lookup, insertion, deletion and shift, wherever the edit is. A binding builds
each published document whole (6.1); that O(nodes) is the platform's
construction cost and is outside these bounds. No bound depends on a size
threshold.

### 7.2 Costs that are the language's, not the algorithm's

Some edits really do change most of the document, and the design does not
pretend otherwise:

- Opening an unclosed fence, HTML block, comment or directive block near the
  top changes the meaning of everything after it until something closes it.
- A comment block and the Properties envelope decide their opener by looking
  ahead to their closer, so the opener's reach runs to the closer and an edit
  inside them reads them from the opener.
- A definition referenced 10,000 times changes 10,000 Links when its
  destination changes.
- A leaf's literal is one string value, so a code block streamed to the end
  of a response produces a new literal of the whole block per chunk. The
  renderer re-highlights that block per chunk anyway.
- A grid or multiline table whose geometry changes rebuilds every row,
  because every cell's bounds changed.

## 8. Testing

The oracles below, the workloads they run on and the edit and stream
benchmarks are specified in [Gates for incremental parsing](2026-09-29-incremental-gates.md),
which also says at which rollout step each one becomes a gate.

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
- **Definition queries.** After every edit, `footnotes`, `specimens` and
  `footnote(for:)` and `specimen(for:)` for every label in the text equal
  those of a fresh parse, and fixtures with nested, duplicate, anonymous and
  unreferenced definitions check each binding's answers against the winners
  of the C registries, since the canonical dump does not call the queries.
- **Allocation failures.** The allocator-seam OOM sweep fails every edit at
  every allocation boundary and asserts that the edit throws and that
  freeing the session leaks nothing.
- **Audits.** E1–E6 (5.4), the close and completion hook rules (5.8), and the
  dependency inventory (section 3) are enforced by scripts in
  `scripts/audit/`.

## 9. Rollout

Each step is one pull request that leaves `main` releasable. Each step must
pass the gates that
[Gates for incremental parsing](2026-09-29-incremental-gates.md#7-activation-by-rollout-step)
activates for it.

- [ ] **Step 0: Gates.** The edit and stream workloads, the correctness
   harness with the `reparse` subject and its faulty-subject self-tests, and
   the edit and stream benchmarks reporting the reparse baseline, in the
   existing benchmark workflow.
- [ ] **Step 1: Model.** Ids for fresh parses, deep equality and hashing, raw extents
   in nodes with on-demand scope queries, MCB3, and the Swift record
   storage, the coordinate unit (4.4), and definitions kept where written
   (4.5). The canonical dump and conformance fixtures change only for
   documents with footnote or specimen definitions, for grid and multiline
   table cells that end on a blank line part, and for ends on a line
   terminator and the empty document under the one byte rule (4.3), whose
   `canonical-ast.md` rule changes in the same step.
- [ ] **Step 2: Sessions with a whole-document restart.** Session API on every platform,
   the text tree, identity matching,
   and value deduplication. Every line is read again and nothing is taken:
   this is the degenerate case of the final algorithm, and it already gives
   R1, R3, R4 and R5, with O(n) parse work.
- [ ] **Step 3: Shared subtrees.** The C tree becomes shared immutable nodes
   with reference counts, children trees, builders for open blocks, copy on
   write and iterative release, with no parent or sibling links (5.11). Walks
   carry their path on explicit stacks. The parser reads the text tree a line
   at a time (5.1). Nodes are complete when they are made (5.8): the finish
   walk, the pass walks and the numbering walk are removed, and identity
   matching is the one comparison of the old and new trees (5.9). Every line
   is still read again, as in step 2.
- [ ] **Step 4: Block reuse.** Node records and the high-water mark (5.1),
   the cursor's take, reopen and read (5.3), E1–E4 and E6 and their audits,
   summaries in children trees, streaming as an edit at the end (5.5), and
   session-held registrations with every inline root depending on every key.
- [ ] **Step 5: Session registries.** Winners, lookup dependencies, anchor
   families (5.7).
- [ ] **Step 6: Inline reuse.** Inline records and the cursor over old inline
   trees (5.6), leaf reopening (E5), and completion over read nodes (5.8).

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
  document carries the parser's footnote and specimen tables for label
  lookups instead of owning the definitions (4.5).
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

- **D6 Node storage. Decided 2026-10-01: shared subtrees.** The C tree is
  stored the way tree-sitter stores its trees: shared immutable nodes with
  reference counts, copy on write, balanced children trees and no parent or
  sibling links (5.11). Rejected: a single-owner tree reused in place, which
  keeps cmark's parent and sibling links.

## 11. Rejected alternatives

- **Returning a diff or patch.** Excluded by the requirement. Consumers
  already reconcile by identity and equality; a second protocol would be a
  parallel source of truth.
- **Re-parse everything, then reconcile the trees.** It achieves R3 and R4 but
  not R2, and it is O(n) per keystroke and per streamed token. It survives
  only as the test oracle and as rollout steps 2 and 3, where it is the
  degenerate case of the same algorithm.
- **Content-hash or path identifiers.** A hash changes on every edit of the
  node, so an edited paragraph would lose its view state. A path changes when
  an earlier sibling is inserted. Neither is stable.
- **A separate streaming parser.** Two parsers diverge. Streaming is an
  insertion at the end, and the open spine is all that an append changes
  (5.5).
- **Top-level blocks as the only reuse unit.** Model output is often one long
  list or quote, and such a unit would re-read all of it per token. Records
  on nodes at every depth cost a few words per block.
- **Memoizing every rule (packrat).** Markdown's block grammar is a line
  machine with bounded lookahead. Records on the nodes the parser already
  builds are enough to take everything that is unchanged, and memoizing every
  rule would multiply memory for no additional reuse.
- **A ledger of the parse beside the tree.** The first step 3 kept a ledger of
  per-line checkpoints, carried state and spine snapshots, replayed at
  finish, with a restart search and a convergence test. Writing it cost
  every fresh parse (one-shot `buffer_to_ast` median 1.078, maximum 2.42,
  against the 1.02 gate) and duplicated what the tree holds. Tree-sitter keeps
  those facts on its nodes, and so does this design (5.1).
- **Walks over the finished tree.** cmark finishes a document with a walk that
  parses inlines and runs finish steps, and step 1 added a walk that numbers
  nodes and writes extents. On a tree of shared nodes each walk costs a step
  through the children trees of every node, a cost cmark's sibling links do
  not have, and the walks only finish what the parser could finish when it
  made each node. Tree-sitter has no such walk (5.8).
