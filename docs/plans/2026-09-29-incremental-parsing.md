# Incremental parsing for editing and streaming

Status: rollout steps 0 to 3 (section 9) are on `main`: the public model,
sessions with a whole-document restart, and Reference nodes, pieces and
content runs. Steps 4 to 7 follow tree-sitter's algorithm and storage. This
document specifies how Markdown Core turns an edit of its source into a new
AST while re-reading as little source and replacing as few AST values as the
language allows, for two workloads:

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
| S7 Bindings | Swift `DocumentBuilder`; Kotlin and ES decode MCB3 | The finished C tree | One immutable value tree per parse; the C document is freed |

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
| Carried block state | Open list item widths, blockquote and callout prefixes, definition body indentation, directive fence stack, last-line-blank flags | Earlier line → later lines | Entry on every node; a node is taken only under an equal entry (5.3, E3) |
| Lookahead | Setext underline, table separator and caption, definition term marker, multiline and grid tables, directive closer, Properties closing fence | Later lines → earlier decision | Reach on every node (5.1, 5.3) |
| Retroactive write | A separate-line block identifier sets the preceding block's anchor | Later line → closed node | Reach raised by the write service (E2) |
| Container facts from children | `List.tight`, list layout, definition and definition-list scopes | Children → container | Summaries folded in the children tree (5.3, E4) |
| Unclosed opaque leaves | Fenced code, HTML block, comment, formula block and directive block without a closer run to the end | Opener → rest of document | No special case: nothing after the opener is taken until the live state equals an old entry again, which is the language's meaning (7.2) |
| Registry lookups in inline parsing | `[label]` and heading labels in the reference map (`link.c`, `heading.c`), `[^label]` in the footnote label map (`footnote.c`), `@id` in the specimen id index (`citation.c`) | Any definition → any occurrence | Reverse index of each key (5.7) |
| Document ordinals | `inline-N` footnote ids, heading anchor `-N` suffixes, footnote and specimen order | Earlier declarations → later values | `inline-N` and the lifted footnote and specimen sequences are removed from the model (4.5); anchor suffixes by recomputation by family (5.7) |
| Shared resources | A definition's destination, title and attributes read by every occurrence | Definition → occurrences | Reverse index of each key (5.7) |
| Absolute coordinates | Every `Scope` after an inserted or deleted line | Every earlier byte → every later scope | Positions leave the AST (4.3) |
| Mapped cell inputs | Grid and multiline cell bodies | Table → cells | Grid and multiline geometry is a fold over the table's lines (E5) |

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
- **Deterministic for a fresh parse.** `Document.parse` numbers nodes from 1:
  each node's owner numbers the nodes it holds when it completes, and the
  root numbers itself when it completes (5.8). Two fresh parses of the same text are equal,
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

Before step 1, every node stored an absolute `Scope`. With absolute scopes,
typing Enter on line 3 changes the value of every node after line 3, and R3
cannot hold for any edit that adds or removes a line.

A scope has one consumer: side-by-side editing. It maps an element the user
points at back to its source and draws its selection on the canvas. That
feature is not on the hot path of parsing or rendering, and it needs a scope
only when it asks for one. So `Markup.scope` is removed, and no node stores a
line, a column or an absolute offset.

What a node does carry is the raw extent the engine keeps for its own work,
copied as is, because a binding answers scope queries in UI code where the
parser is no longer reachable. Parse offsets and scopes are different things:
the parser works with offsets in the bytes it reads, and a scope is the set of
source ranges an editor highlights.

```text
Extent(lead: Int32, span: UInt32)       offsets in the parser's input
    lead:   signed, from the end of the previous node in the same relation
            (or the owner's start, for the first node) to this node's start
    span:   of this node's range
Run(lead: Int32, span: UInt32, length: UInt32)
                                        offsets in the source
    lead:   from the end of the previous run (or the node's start, for the
            first run) to this run's start
    span:   source bytes the run reads
    length: content bytes it becomes
```

- **One rule.** Every extent is a byte offset in the input of the parser
  that made the node. The block parser reads the source text, so a block's
  extent is an offset in the source. The inline parser reads its root's
  content, which starts at offset 0, so an inline node's extent is an offset
  in that content. Each parser, its reuse (5.3, 5.6) and identity matching
  (5.9) work in the offsets of their own input and need no mapping.
- **Runs.** A node whose source is not one contiguous range, or whose first
  relation is an inline root's content (a block's inline content, a
  callout's title, a definition's term), carries `runs`: the source it read,
  in order. Each run is `length` content bytes read from `span` source
  bytes.
  - A run whose span is its length reads each content byte from one source
    byte; any other reads all of its content from all of its source, as
    `\|` in a table cell is two source bytes and one content byte, and a tab
    in a grid cell is one source byte and the spaces it becomes.
  - A run of length 0 is source the node reads that gives no content: an
    opening fence, a heading's underline, the indentation of a paragraph's
    later lines, or a whole line of a node without inline content.
  - Between the first run and the last, the runs cover exactly the node's
    own source, so the source between two runs is not the node's: the
    container prefixes between a leaf block's lines (E5) inside a
    blockquote, callout or list item, and the other columns between a grid
    or multiline table cell's lines.
  - A run of length 0 at either end of the list has a gap beside it, so a
    node of the document itself without inline content, such as a fenced
    code block, has no runs, and containers have none of their own.
  - The element that reads the source records the runs as it reads, so no
    other code knows how an element turns source into content.
- The engine stores extents and runs on every C node, and the
  bindings copy them verbatim, like any other field. No unit conversion and no line counting
  happens while parsing or publishing.
- `lead` is signed because a relative offset between two ranges has no sign
  of its own: ranges may overlap or nest in any way the spec defines, and the
  encoding does not assume otherwise. An inline note's `Footnote` covers
  `^[content]` while its owning `Citation` covers only the content
  (`canonical-ast.md`), so the note's `lead` is −2. No node needs a rule of
  its own for this.
- Neither number changes when text before the node shifts. An edit inside a
  node changes its own `span` (it is a new value anyway). An edit in the gap
  before a node, such as an added blank line, changes that node's `lead`.
  Every other node keeps its value. Extents and runs are part of
  equality (4.2), so a reused node's extent is always the right one.
- **Scopes.** `document.scope(of: node, in: source) -> [Scope]` returns the
  source ranges of the node, which is what an editor draws as the node's
  selection. `document.node(at: Position, in: source)` hits a node
  only inside one of its ranges. Both take the source text the document was
  parsed from, which the side-by-side editor already holds (`session.text`
  for a session's current document). They return today's editor line and
  column conventions and sentinels, in the session's coordinate unit (4.4).
  - A node's ranges are one window of source less the gaps between the runs
    that place it. A block's window is its range, and the runs are its own.
    An inline node's window runs from where its first content byte was read
    to where its last was, and the runs are its root's.
  - Every binding computes them with this one walk over the runs, so no
    binding repeats an element's syntax (closing sequences, cell padding,
    column geometry).
  - The line and unit conversion scans the source. This cost is paid only by
    the query.
- Walker callbacks no longer carry a scope.
- A range's line and column ends follow one rule for every node: a range
  ending right after line `L-1`'s terminator ends at `L:0`. So `SoftBreak`,
  `LineBreak` and a `Citation` that end on a line terminator end there too,
  and a zero-byte document is `1:1..1:0`, as a document of one newline is. A
  grid or multiline cell's run on a line where its part is blank is empty
  there, and `canonical-ast.md` drops the cell-local sentinel.
- The canonical dump prints the ranges by this rule, so it is a scope query
  and takes the source like one: `document.dump(in: source)` and
  `document.dump(node, in: source)` on every binding, and
  `markdown_core_document_dump(document, source, ...)` in C.

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
  replacements. The edit pass applies each edit of the batch on its own
  (5.2), so bytes between two edits stay surviving bytes with their own
  shift. One edit pass and one parse cover every edit of the batch (5.2,
  5.3).
- **`Document.parse`** keeps its signature apart from the unit parameter. It
  is a session that inserts the whole source once and is then discarded.

### 4.5 Definitions stay where they are written

Before step 1, `Document.footnotes` and `Document.specimens` owned every
footnote and specimen definition, lifted out of the place where it was
written, and an inline note `^[body]` got a generated id `inline-N` so its
`Citation` could name the lifted `Footnote`. That lift is what gave
definitions their special cases: the generated ordinal (every later inline note changes when one is
inserted, and an authored `[^inline-3]` label renames a note elsewhere),
positions measured along a list whose entries are scattered across the
document, and identity decided in a list that is not where the parser
produced anything.

The design keeps every definition in the tree where it was written, and the
document answers lookups over its content instead of owning the nodes:

```text
CitationReferent = bib(key, mode) | footnote(FootnoteTarget) | specimen(label)
FootnoteTarget   = label(String) | note(Footnote)

Destination      = url(String) | cross(path, anchor) | reference(label)

Footnote(label: String?, content: [Markup])
Specimen(label: String?, start: Int?, content: [Markup])
Reference(label: String, dest: Destination, title: String?)
```

- A referenced definition `[^x]: body` is a `Footnote` block in the content
  relation where it was read, like any other block. A specimen definition is
  the same.
- A link reference definition `[label]: destination "title"` is a
  `Reference` leaf block where it was read, with its normalized label and
  the attributes it supplies in its inherited `attributes` field. The
  paragraph that held it ends before it, and a paragraph that held only
  definitions is only its `Reference` nodes. Renderers output nothing for a
  `Reference`.
- A reference link or image, `[text][label]`, `[label][]` or `[label]`, names
  its definition the way a `Citation` names a footnote: its `dest` is
  `reference(label)` with the normalized label, and its `title` and
  `attributes` are its own, empty when it wrote none. The definition's
  destination, title and inherited attributes are read through the lookup
  below, as a footnote's content is. A label that nothing defines leaves the
  brackets as literal text, as the spec requires.
- An inline note is a `Footnote` owned at its call site by its referent,
  `footnote(note(Footnote))`, with a null label. A note inside a note is
  ordinary nesting. There is no generated id, no reservation against authored
  labels and no `-K` rule.
- `Footnote.id` and `Specimen.id` are renamed `label`, because every node now
  has `id: MarkupID` (4.1).
- `Document` carries the parser's footnote, specimen and reference tables:
  the ids of every definition in source order, as the C registries (5.7)
  hold them. A binding receives them with the document (6.2) and resolves
  them to its nodes while it builds the tree (6.1). `document.footnotes`,
  `document.specimens` and `document.references` list those definitions in
  source order. `document.reference(for: label)` returns the label's winner,
  a `Reference` or, after every `Reference`, a heading whose label it is,
  whose destination is its anchor. `document.footnote(for: label)` and
  `document.specimen(for: label)` return
  the first one whose stored label equals the referent's (the parser has
  already normalized both), as the spec defines today.
- Definitions therefore follow the tree's ordinary identity rules (5.9),
  with no rule of their own.
- Walks and the canonical dump visit a definition where it was written. A
  renderer that prints notes at the end of the page reads the lookup, and
  display numbering stays the renderer's job, as the spec already says.

This changes `canonical-ast.md`, `dialect/footnotes.md`, `dialect/specimens.md`,
`dialect/links-and-images.md`, `dialect/attributes.md`, the canonical dump of documents with
definitions and their fixtures, and every binding (D2).

## 5. Engine

### 5.1 What a session retains

| State | Contents | Size |
| --- | --- | --- |
| Text tree | The source as a balanced tree of bounded byte chunks; each subtree records its byte, line-terminator and UTF-16 counts | O(source) |
| Tree | The document as shared immutable nodes (5.11); each node holds its id, its extent and runs (4.3) and its parse record | O(nodes) |
| Registry | Facts of every kind, each held by the node that declares it, with its order label (5.7) | O(declarations + lookups) |
| Key index | Each key → its declaring facts in tree order and its reverse index, the inline roots that looked it up, hit or miss | O(declarations + lookups) |

The tree is the record of the parse, as it is in tree-sitter, where each
subtree carries its parse state and how far past its end the lexer looked,
and nothing else survives a parse. Every block node records two things when
it is built, the counterparts of tree-sitter's `parse_state` and
`lookahead_bytes`:

- its **entry**: the carried state (E3) of its parent at its start, after
  its lead, where the line machine asks the cursor for it (5.3);
- its **reach**: how far past its end any decision about it read, including
  lookahead, claimed ranges, the line that closed it and every later line
  that wrote into it (E2). A container's reach covers its children's, as a
  tree-sitter parent's lookahead covers its children's.

The input index keeps a high-water mark of the last byte any decision has
read, as tree-sitter's lexer does, so reaches are measured, not declared: an
element cannot forget one. A leaf block's lines (E5) and inline nodes (5.6)
record an entry and a reach the same way. A fresh parse writes these few
words per node and nothing else.

The text and each container's children are sequences whose elements have
extents. They share one structure: a balanced tree whose elements store
their length (for text, a chunk's bytes; for children, a child's lead and
span) and whose internal nodes store the sums. For children, the internal
nodes also store the furthest reach past their range's end, the combined
summary of their children (E4) and the last fact their nodes declare (5.7). Absolute
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

### 5.2 The edit pass

An edit replaces bytes `[a, b)` of the old text with `n` bytes. A byte at old
offset `x` survives when it is not inside a replaced range, and its image is
exact:

```text
x < a    → x
x >= b   → x + n - (b - a)
a <= x < b: replaced, no image
```

A batch is a list of disjoint replacements, and a surviving byte's image is
its offset plus the sum of the length changes of the replacements before it.
Inserted bytes are the preimage of nothing. This mapping defines identity
(5.9).

Before the parse, the session applies the batch to the old tree, as
`ts_tree_edit` applies an edit to a tree-sitter tree. The pass descends from
the root along every node whose range, from the start of its lead to its end
plus its reach, meets or touches a replaced range, and for each such node:

- it shifts the node's own extent: an edit inside the lead shortens or
  lengthens the lead, an edit that starts in the lead and reaches into the
  span moves the start to the edit's end, and an edit inside the span
  changes the span, exactly as `ts_subtree_edit` adjusts padding and size;
- an empty node, which has no byte, takes the image of its start;
- it marks the node **changed**;
- it copies the node first when it is shared (5.11), so the published old
  document is unchanged and only the root-to-edit paths are copied.

Inline extents are content offsets (4.3), so the pass stops at inline
roots: a root it meets is read again, and its inline parse finds its edit
from the source edits through its content runs (5.6). Nodes after an edit keep their extents, which are
relative, and are not visited. A container's children tree finds the children whose range plus
reach meets an edit in O(log children) from the reaches its internal nodes
hold (5.1). The edited old tree is in the coordinates of the new text, so the
cursor (5.3) and identity matching (5.9) read positions from it directly, and
an old node's start in it is the image of its first surviving byte.
Line boundaries need no rule of their own: the reading of a line terminator
is part of a node's reach.

### 5.3 Blocks: re-parse against the old tree

An edit parses the new text from its start with the ordinary line machine and
a cursor over the edited old tree (5.2), as tree-sitter re-parses a file
against its edited old tree. Wherever a node can be reused, it is taken whole
instead of read.

**The cursor.** The cursor is a stack of (old node, child position, offset),
like tree-sitter's reusable node, and it moves only forward. Before the line
machine starts a block, at the point where the line's container prefixes have
been matched and no block is open below the innermost open container, it asks
the cursor for the old node that starts at that position, as tree-sitter asks
for a reusable node before it lexes:

- **Take.** The node is not changed and its entry equals the carried state
  of the live innermost container. The node is taken whole, with the run of
  unchanged siblings after it, the container folds the run's combined
  summary (E4) into its carried state, and the parse continues after the run
  without reading any line of it. The rest of the run needs no comparison:
  equal state at a node's start and identical bytes through its reach and
  its next sibling's lead give equal state at that sibling's start, which is
  its entry. A run ends only at a node after which the parse reads the next
  lines as the old one did: not at one whose closing line refused a block
  start because it was open, not at one that was open over blank lines after
  its end, and not at one a later line may still write into (E2).
- **Descend.** Otherwise the cursor moves to the node's first child, as
  tree-sitter breaks a changed node down, and the line machine reads the line
  with `S_process_line`, as a fresh parse does. A changed container is built
  again by the line machine: its opening line is read, and its unchanged
  children are taken when the parse reaches their starts. A changed leaf
  block takes its unchanged lines the same way (E5). The cursor skips old
  nodes that start before the parse position, like tree-sitter's reusable
  node.

Every node the parse builds is completed by the line machine as in a fresh
parse (5.8), from its children and the summaries of the runs it took (E4),
as a tree-sitter parent is reduced from its children.

**Tables.** A pipe table is a container whose rows are its children.
Grid and multiline tables decide their geometry from all of their lines,
which is a fold (E5), and each of their rows records that geometry as its
entry. Cells are internal inputs of the table's transaction, as now.

**Several edits.** A batch (4.4) is one edit pass and one parse. The nodes
between two edits are taken like any others.

**Degenerate cases are the same algorithm.** A fresh parse has no old tree,
so every line is read. An edit in the Properties envelope changes the
document's first block. An opener of an unclosed fence is followed by no
taken node until something closes it, because the rest of the document
really did change meaning.

**References.** A `Reference` begins where the paragraph it was read from
begins, so the cursor offers an old `Reference` when the line is about to
begin a paragraph.

**Facts.** Facts belong to the nodes that declare them (5.7), so a taken
node keeps its facts, and the facts of the old nodes that were read leave
with those nodes.

### 5.4 The element contract

The engine can take only what elements make deterministic. These
are requirements on every element, each checked by an audit script in
`scripts/audit/` in the style of `parser-boundaries.mjs`:

- **E1 Reads go through the index.** Source reads, including lookahead and
  claimed ranges, go through the input index, so the high-water mark sees
  them and every reach is measured. An element never keeps a
  raw pointer into the source across lines.
- **E2 Retroactive writes go through one service.** Changing a node that has
  closed (a separate-line block identifier, a table's trailing caption) uses
  `markdown_core_parser_write_closed(parser, parent, end)`, which extends the
  open parent's last child to `end` and raises its reach to what the line has
  read. No run of taken blocks ends at a block a later line may write into
  (5.3), so that block is the parse's own and changes in place. The audit
  forbids other writes to closed nodes.
- **E3 Carried state is a word.** Per-parse element state
  (`state_size`) is one of three things: a cache that the parse may drop; a
  declaration registry that moves to the session (5.7); or carried block
  state. A container's carried state where a child can start is a function of
  what its opening line decided, the word `carry_save` returns, and the
  combined summary (E4) of its closed children, whose leads hold the blank
  lines before them. Nothing else carries information from one line to a
  later one. That state is a few words, recorded as each child's entry, so
  every comparison in 5.3 compares words.
- **E4 Container finalize is a fold of child summaries.** It reads children
  and recorded facts and writes the container's own fields, and running it
  twice gives the same node. Each container kind declares a per-child summary
  and an associative combine, and its fields are a function of the combined
  summary, which its children tree keeps (5.1). `List.tight` is one: a
  child's summary is (starts after a blank line, contains a blank between its
  own children), and the list is loose when any child contains one or any
  child after the first starts after one. Extents of definitions and lists
  combine from their children's extents.
- **E5 A leaf block is a fold of its lines.** A leaf block's lines are its
  hidden children, as tokens are a tree-sitter node's leaves: each line
  records its entry (the leaf's state before the line) and its reach, and
  the leaf keeps them in a children tree like any other. Each line has a
  summary, and every decision over the leaf's lines is a function of their
  combined summary, made when the leaf closes: a paragraph's content and its
  consumed reference-definition prefix, a code block's literal and its
  trailing-blank trimming as a length, and the column geometry of a grid or
  multiline table, whose rows record that geometry as their entry. A changed
  leaf is descended like a container: its unchanged lines are taken in runs,
  its changed lines are read, and the content of the taken lines is copied
  back into the leaf's content, in O(copied bytes), with no decision made
  again. A line on which a decision read the leaf's content before it (a
  table header or setext underline tried against the paragraph so far) is
  read again by every parse, as tree-sitter never reuses a node its parse
  state does not account for. The hidden lines are storage, like the internal nodes of a children tree:
  walks, the canonical dump and the bindings do not see them.

### 5.5 Streaming is an edit at the end

An append inserts at the end of the text. A node that was open when the
input ended read to the end, so its reach touches the insertion, and the
changed nodes are exactly the open spine. The parse reads each container of
the spine again from its opening line, takes its closed children in runs,
and descends into the last open leaf, whose lines before the last are taken
(E5). An append therefore costs the opening lines of the spine, the last
line, the new bytes and the inline work of 5.6. Blocks that the appended
text closes close normally.

Every published document is a finished parse of the session's text. A
paragraph that is still growing keeps its id from its first line to its last
by the matching rule (5.9).

### 5.6 Inline parsing

An inline root is parsed again when its block was read again, or when a
registry winner it looked up changed (5.7). Every other root keeps its inline
tree.

A root that is parsed again holds the old inline tree of the old root it
continues and is parsed against it by the algorithm of 5.3. Inline extents
are content offsets (4.3), and the root finds its edit from the step's source
edits through the content runs of the two roots:

- A content byte **continues** when it is the same surviving source byte.
  The old root's runs give the source byte each old content byte was read
  from, the step's edits (5.2) give that byte's image, and the new root's
  runs give the new content offset read from the image. A run that reads all
  of its content from all of its source continues whole when its source
  survives as one contiguous stretch no edit cuts, and is replaced
  otherwise.
- The content bytes that do not continue are the root's edits: disjoint
  replacements in the old content's offsets, a batch like a source batch.

The edit pass (5.2) applies those edits to the old inline tree as it applies
source edits to blocks, and the same cursor takes or descends. Identity
matching (5.9) reads inline anchors through the same mapping, so reuse and
identity share one model. Each
inline node records its **entry**, the
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

The parser reads the content from its start with the cursor over the old
inline children: an unchanged node whose entry equals the live delimiter
state is taken with its run, and the cursor descends into every other node. The
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

What a node declares to the whole document, and each question an inline
root asks of it, is a **fact**. The nodes that declare are:

- a `Reference` (4.5), declaring its label;
- a heading, declaring its label as a target and its anchor base;
- a node with an explicit anchor;
- a `Footnote` or `Specimen` definition;
- an inline root, declaring a **lookup** for each key it asked.

A fact belongs to the node that declares it and lives exactly as long as
that node. It is made when its node is made, and it is removed when its node
is released (5.11). A taken node keeps its facts, because it is the old node.
The registry stores no positions.

The **key index** maps each key to two collections:

- the key's declaring facts, as a balanced tree: `Reference` facts before
  heading targets, and each in tree order by their order labels;
- the inline roots that looked the key up, hit or miss, as a doubly linked
  list. This is the key's reverse index.

A key is a group and a label:

- a reference label, which `Reference` nodes and then heading targets
  declare;
- a footnote label;
- a specimen id;
- an anchor **family**: the spellings with the same stem after stripping
  trailing `-N` groups. Explicit anchors reserve spellings in it, and
  headings take their anchors from it.

A key's winner is the leftmost fact of its tree: its first fact in tree
order, with `Reference` nodes before heading targets. A fact is inserted at
its place by comparing order labels down the key's tree, and removed from
it, in O(log k) for a key with k facts, wherever the fact lies among them. A
fresh parse declares facts in tree order, so each lands at the right end of
its key's tree and is appended there, as a children tree is built (5.1). A
lookup joins or leaves the reverse index in O(1), because the reverse index
is read whole and needs no order.

**Tree order without positions.** Facts carry order labels (Dietz and
Sleator's order maintenance), so two facts compare in O(1). The parse visits
the document in tree order, reading some nodes and taking others, and keeps
the last fact before its position. A new fact is labeled right after that
fact. A taken run gives its last fact from its children tree's sums, as it
gives its reach (5.1). An inline root records the last fact before it when
it is listed (5.8), so its inline facts are labeled in tree order too.

**One edit**, in order:

- **Block parse.** The parse declares the facts of the blocks it makes and
  marks their keys.
- **Release.** The old root is released. The old nodes the parse did not
  take are freed, and each removes its facts and marks their keys. These are
  exactly the nodes the parse read again, so this costs what the parse read.
  A root that was read again holds its old inline tree (5.6) until its own
  inline parse ends. That old tree's facts leave then, the same way.
- **Inline parse.** Each root that was read again is parsed. Its lookups
  are answered at once from the registry, whose definitions are complete
  after the release, and are added to the reverse index of each key asked.
- **Resolution.** For each marked key, the winner is found again.
  - **Lookups.** An occurrence names its definition by label (4.5), so
    what an inline parse reads from a key is whether it is defined. When
    that changed, each root on the key's reverse index that this edit did
    not parse is parsed again in place. The
    document is a snapshot of the session (4.4), and its node is the
    session's own once the old root is released, so the node is updated in
    place. Its extent and runs stay the same. The root leaves the reverse
    indexes of the keys it asked before and joins those of the keys this
    parse asks, which are answered at once from the registry.
  - **Anchors by family.** Each marked family is assigned again in tree
    order, with the same reservation and suffix-cursor algorithm as a
    fresh parse. A heading whose anchor changes takes its new anchor in
    place.

An inline parse in resolution changes only what its root read from the
registry. Its content and inline anchors stay the same, and the keys it asks
are answered from definitions that are complete after the release, so no
root is parsed twice and each stage runs once per edit. A definition's
destination, title or anchor changes only the definition node and the
document's tables, never an occurrence. Definitions stay in the tree, so nothing is spliced into the
document and no ordinal is recomputed.

### 5.8 Nodes are complete when they are made

A tree-sitter node is complete when the parser reduces it: its children, its
size and its padding are set then, and nothing changes it afterwards. The
engine makes its nodes the same way, so no stage walks the tree after the
parse.

- **Blocks complete when they close.** Closing a block runs everything that
  decides it: the element's close (a formula block's literal; a code block
  whose info names a formula becomes a FormulaBlock), the container's fold of
  its children (E4: list layout, definition scopes), and the ids and extents
  (4.3) of the nodes it holds, which keep their absolute places until then,
  in canonical field order. The document root numbers itself when it
  completes, and its extent is measured from 0. A paragraph's close makes a
  `Reference` for each reference definition it consumed and adds them to its
  parent before it. A paragraph left with no content is not added.
- **Inline roots complete when their parse ends.** A block's inline content
  and each inline field of a block (a definition's term, a callout's title, a
  table's caption, a directive's label) is an inline root. Closing the block
  adds it to the parse's list of roots with its absolute start; its owner
  completes, and numbers it, first. After S3,
  each root on the list is parsed. Delimiters decide nesting only when a
  closer pairs, and the language gives an inline node meaning from what
  encloses it: an escaped space inside a word body, a Text outside a Link for
  email autolinks, and runs of Text that become one. So an inline node is
  complete when its root's parse ends, and the root completes its own tree
  then, in one pass over that tree: consolidation, completion, email
  autolinks, extents and ids. The root completes last, from the start it
  recorded, and the content of a group it holds for its owner is published
  from where the owner starts. A paragraph whose only content is a standalone
  formula becomes a FormulaBlock at that point; the Formula it consumes was
  never numbered.
- **Absolute positions belong to the parse.** A node holds only its extent
  once it is complete. Everything that needs an absolute position after that
  (a root on the list, table geometry) records it when it is made.
- **Document facts come from the registry.** Resolution (5.7) assigns
  anchors and destinations, and the footnote and specimen lookup tables list
  their definitions' facts in tree order.

A fresh parse is the block parse, the inline parse of each root, and resolution.
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
counterpart of tree-sitter's `ts_tree_get_changed_ranges`: it walks the new
tree and the edited old tree (5.2) together, both in the coordinates of the
new text, and steps over every subtree they share by reference, so it visits
only what the parse read.

- Matching runs per owner relation between a new owner and the old node it
  matched, starting from the two document roots. A definition's bodies are
  each a relation of it, so the cursor (5.3) continues a body only from the
  old body at its place.
- Each old node has an **anchor byte**: the first byte of its source range
  that survived the edit. Its image is the node's start in the edited old
  tree. A node none of whose bytes survived has an empty span there, has no
  anchor and cannot be matched; its id retires.
- An old node `O` can match a new node `N` when their kinds are equal and
  `N`'s source range contains the image of `O`'s anchor byte (5.2).
- An inline node's anchor is its first content byte that continues (5.6),
  and its image is that byte's new content offset, so inline nodes match in
  the offsets of their root's content by the same rule.
- When `N` contains the anchors of several old siblings, it takes the
  earliest not yet passed, and the ones after it remain for the next new
  sibling: the cells of a grid table that span rows start inside the ranges
  of the cells before them. Both sequences are in source order and the match
  is monotone, so it is linear in the region.
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
  same state after it. A node the parse builds again is built by the same
  line machine from the same lines. A container's fields are a fold of its
  children's summaries (E4), and a decision over a leaf's lines is a fold of
  its lines' summaries (E5).
- Inline trees: a kept root's content and lookup answers are unchanged. A
  root parsed again runs the same parser against the same registries, and
  its taken nodes are exact by the same induction.
- Resolution: winners and families are found again over the complete
  registry in tree order with the same rules, and a root whose answer
  changed is read again by the same inline parser.
- Finish: per-root steps on unchanged roots gave the same results before.

This argument is also the test oracle (section 8).

### 5.11 Session state and memory

The session's state is the text tree, the tree, the registries and the
key index. Between edits the parser holds nothing else: like
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
  its facts leave the registry (5.7), its children are released the same way
  from an explicit stack, and the freed slots return to the session's pool.

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
Every node record adds `u64 id`, the node's `Extent` in place of `Scope`,
and its runs (4.3). The message ends with the footnote, specimen
and `Reference` tables and the reference label table, each label with the id
of the node it resolves to (4.5).
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
the paths from the root to the changes, `h` the opening lines of the
containers on those paths, `L` the size of the read leaves, `W` the lines
read after the changed lines before the next take, and `k` the size of
inline roots invalidated by resolution changes.

| Operation | Block work | Inline work | Resolution | C tree changes |
| --- | --- | --- | --- | --- |
| Append `c` bytes inside an open paragraph | O(c + h + last line + d log b) | O(c + last line + open delimiters) | O(changed declarations) | O(d log b) |
| Append that closes and opens blocks | O(c + h + last line + d log b) | as above | as above | O(d log b) |
| Edit inside one closed leaf | O(changed lines + W + h + d log b) | O(L) | O(changed declarations + k) | O(d log b) |
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
- Defining or removing a label referenced 10,000 times turns 10,000
  bracket texts into Links or back.
- A leaf's literal is one string value, so a code block streamed to the end
  of a response produces a new literal of the whole block per chunk. The
  renderer re-highlights that block per chunk anyway.
- A grid or multiline table whose geometry changes rebuilds every row,
  because every cell's bounds changed.
- An edit that makes a table stop being one, or start being one, reads
  every row of it again, because every row's meaning changed.

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
  paragraph are new values), and changing a reference's destination or a heading anchor that Links
  target (only the definition and the Document are new values).
- **Definition queries.** After every edit, `footnotes`, `specimens`,
  `references`, `footnote(for:)`, `specimen(for:)` and `reference(for:)` for
  every label in the text equal
  those of a fresh parse, and fixtures with nested, duplicate, anonymous and
  unreferenced definitions check each binding's answers against the winners
  of the C registries, since the canonical dump does not call the queries.
- **Allocation failures.** The allocator-seam OOM sweep fails every edit at
  every allocation boundary and asserts that the edit throws and that
  freeing the session leaks nothing.
- **Audits.** E1–E5 (5.4), the close and completion hook rules (5.8), and the
  dependency inventory (section 3) are enforced by scripts in
  `scripts/audit/`.

## 9. Rollout

Each step leaves `main` releasable and must pass the gates that
[Gates for incremental parsing](2026-09-29-incremental-gates.md#7-activation-by-rollout-step)
activates for it. Steps 0 to 3 were one pull request each. Steps 4 to 7 land
in one pull request, because their costs are measured once block and inline
reuse both exist. Steps 4 to 7 report their benchmarks without gating on
them, and step 8 makes the whole engine meet the benchmark gates.

- [x] **Step 0: Gates.** The edit and stream workloads, the correctness
   harness with the `reparse` subject and its faulty-subject self-tests, and
   the edit and stream benchmarks reporting the reparse baseline, in the
   existing benchmark workflow.
- [x] **Step 1: Model.** Ids for fresh parses, deep equality and hashing, raw extents
   in nodes with on-demand scope queries, MCB3, and the Swift record
   storage, the coordinate unit (4.4), and definitions kept where written
   (4.5). The canonical dump and conformance fixtures change only for
   documents with footnote or specimen definitions, for grid and multiline
   table cells that end on a blank line part, and for ends on a line
   terminator and the empty document under the one byte rule (4.3), whose
   `canonical-ast.md` rule changes in the same step.
- [x] **Step 2: Sessions with a whole-document restart.** Session API on every platform,
   the text tree, identity matching,
   and value deduplication. Every line is read again and nothing is taken:
   this is the degenerate case of the final algorithm, and it already gives
   R1, R3, R4 and R5, with O(n) parse work.
- [x] **Step 3: Reference nodes, pieces and content runs.** Link reference
   definitions are `Reference` nodes where they were written, and reference
   links name their label (4.5). Inline extents are offsets in their root's
   content, leaf blocks in containers and grid and multiline cells carry
   pieces, inline roots carry content runs, scope queries answer `[Scope]`,
   and MCB3 carries pieces, runs and the reference tables (4.3, 6.2).
   Revised after the step: runs replace pieces as the one record of where
   a node's source lies (D1).
- [ ] **Step 4: Shared subtrees.** The C tree becomes shared immutable nodes
   with reference counts, children trees, builders for open blocks, copy on
   write and iterative release, with no parent or sibling links (5.11). Walks
   carry their path on explicit stacks. The parser reads the text tree a line
   at a time (5.1). Nodes are complete when they are made (5.8): the finish
   walk, the pass walks and the numbering walk are removed, fresh-parse ids
   are numbered in completion order (4.1), and identity matching is the one
   comparison of the old and new trees (5.9). Every line is still read again,
   as in step 2.
- [ ] **Step 5: Block reuse.** Entries and reaches and the high-water mark
   (5.1), the edit pass (5.2), the cursor's take and descend (5.3), E1–E5
   and their audits, summaries in children trees, streaming as an edit at
   the end (5.5), and session-held registrations with every inline root
   depending on every key.
- [ ] **Step 6: Session registries.** Facts held by their nodes, the key
   index with reverse lookups, order labels, winners and anchor families
   (5.7).
- [ ] **Step 7: Inline reuse.** Inline entries and reaches, content edits
   through content runs, the cursor over old inline trees (5.6), and
   completion over read nodes (5.8).
- [ ] **Step 8: Performance.** One refactor of the shared algorithms and
   data structures of steps 4 to 7 until every benchmark gate passes:
   flatness for the edit and stream families, never worse than reparsing,
   and the one-shot and session regression rules against the revision
   before step 4.

## 10. Decisions for the owner

- **D1 Positions. Decided 2026-09-29: raw extents, scopes on request.** Nodes
  carry only the engine's own byte extent (signed lead from the previous
  sibling, span), copied verbatim to every binding. `document.scope(of:in:)`,
  `document.node(at:in:)` and the canonical dump `document.dump(in:)` compute today's editor line and column scope from
  it on request, because side-by-side editing is the only consumer and is
  not on the hot path. Rejected: absolute scopes in nodes, which replace
  every node after an inserted line, and line and column spans in nodes,
  which make every parse and publish count lines and convert units.
  Revised 2026-10-04: inline extents are offsets in their root's content,
  the bytes the parser reads, and a block whose lines are separated by bytes
  that are not its own carries one piece per line. A scope is the list of
  source ranges an editor highlights, computed from extents and pieces (4.3).
  Rejected: inline extents in source bytes, which made the inline parser map
  every content offset back to the source.
  Revised 2026-10-05: each inline root records its content runs, from which
  every binding maps an inline node's range to the source with one walk
  (4.3), and an inline root finds its edit from the step's source edits
  through them (5.6).
  Revised 2026-10-06: runs are the one record of where a node's source lies.
  A run of length 0 is source that gives no content, and the source between
  two runs is not the node's, so pieces are removed and every scope is a
  window less the gaps between runs (4.3).
- **D2 Definitions. Decided 2026-09-29: definitions stay where written.**
  Footnote and specimen definitions remain in the tree where they were
  written, an inline note's `Footnote` is owned at its call site, and the
  document carries the parser's footnote and specimen tables for label
  lookups instead of owning the definitions (4.5).
  Revised 2026-10-04: a link reference definition is a `Reference` node
  where it was written too, and a reference link names it by label as a
  `Citation` names a footnote, so every declaration is a node, its facts live
  and die with it (5.7), and no occurrence copies a definition's values. Rejected: keeping `inline-N`, where inserting one
  note changed every later note; a separate `InlineNote` kind, which would express footnote semantics
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
  only as the test oracle and as rollout steps 2 and 4, where it is the
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
- **A ledger of the parse beside the tree.** A first block reuse kept a ledger of
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
- **Reopening a changed container from a recorded head.** An earlier version
  of this plan recorded each container's state after its opening line and
  put a changed container back on the spine without reading that line, with
  a rule of its own for closing it and another for the lead of the first
  node taken after a read one. Tree-sitter never reuses a changed node: it
  descends into it and builds the parent again from its reused children
  (5.3). Reading one opening line per changed container keeps one rule for
  every node, and the same rule covers a leaf's lines (E5).
- **Mapping old positions through the edits on every query.** An earlier
  version kept the old tree in old coordinates and mapped each position the
  cursor and matching asked about. Tree-sitter applies the edit to the old
  tree once, and every later step reads new coordinates from it (5.2).
- **The common prefix and suffix of a root's old and new content.** An
  earlier version of this plan found an inline root's edit by comparing its
  old and new content. The comparison disagrees with identity matching on an
  ambiguous alignment and merges a root's separate edits into one. The
  step's source edits through the content runs (5.6) give the exact edits,
  and reuse and identity share them.
- **Content mapping rules in each binding.** Each binding could map inline
  offsets back to the source by repeating each element's rules (trimmed
  closing sequences, cell padding, escaped pipes, tab expansion). That
  copies the grammar into three languages. The runs the parser records carry
  the mapping as data (4.3).
