# Incremental parsing for streaming

Status: rollout steps 0 to 8 (section 9) are on `main`: the gates, the public
model, sessions with a whole-document restart, Reference nodes and content
runs, node runs, sessions that only append, builders, the streaming session
with `finalize`, and the settle point of an open paragraph. On 2026-10-14 the owner
narrowed the requirement to one workload, appending tokens from a language
model, and this revision replaces the general-edit engine that steps 4 to 8
of the previous revision specified. Steps 4 to 8 now build a streaming
engine. On 2026-10-27 the owner added edits on a finalized session that
re-read only the damaged block (section 12); step E0 is next.

This document specifies how a Markdown Core session turns text appended to
its end into a new AST, while reading again only the text whose meaning is
not yet decided. The consumers are SwiftUI, Jetpack Compose and React. They
reconcile a view tree against a value tree by identity and equality, so the
result of an append is **a new `Document`**, a plain immutable value like the
one a fresh parse returns. A consumer that compares it with the document it
already holds finds every unchanged subtree to be an equal value, every node
that persists under the same identifier, and every changed node unequal to
its predecessor. That comparison is the consumer's framework's own work; this
design gives it the ids and equality it needs and nothing else.

Sections 1–3 state requirements and ground them in the current parser.
Section 4 is the public model. Sections 5–6 are the engine and binding
design. Sections 7–10 cover complexity, testing, rollout and the owner's
decisions. Section 11 records rejected alternatives. Section 12 specifies edits on a
finalized session.

## 1. Requirements

The owner's requirement, 2026-10-14:

> 我仔细理了一下我们的编辑需求，我们实际只有streaming的追加token更新一种需要高吞吐的实时编辑能力，因此，鉴于现在性能的现实，你可能需要彻底放弃现在的所有design，但是那个为修正表格的scope做的node runs的设计可能需要保留，我们需要正确的表达每个node的位置的能力。然后基于llm的token streaming追加场景设计一个高度优化的markdown解析器。

> 对于llm session streaming，你甚至可以在session finalize时再去做definition look up，因为token streaming过程中的文本跳变本来就是无法避免的。

> 也就是你需要加一个session.finalize的API，由调用方显式结束当前session。

- **R1 Equivalence.** After `finalize`, the session's document is equal to
  `Document.parse` of the session's text in every field except identifiers,
  and the canonical debug dump, which prints no identifiers, is
  byte-identical. Before it, each document an append returns is a
  **snapshot**: the document `Document.parse` returns for the text so far,
  except that definition lookups see only the definitions written before the
  node that asks (5.5). A snapshot depends only on the text, not on how it
  was split into appends.
- **R2 Bounded work per append.** An append's work is proportional to the
  appended bytes, the depth of the open blocks and the part of the text whose
  meaning the end of the input still decides, not to the text already
  streamed. Section 7.2 lists the constructs whose undecided part the
  language makes unbounded.
- **R3 Minimal replacement.** A node is a new value only if its value
  changed. Its ancestors are new values because their child collections
  changed. Every other node of the new document equals the previous
  document's node with the same id.
- **R4 Stable identity.** Every node has an identifier, unique within its
  document, that survives every append that does not remove the node. An
  identifier never changes kind.
- **R5 Values only.** An append returns the new `Document`. A binding builds
  it from the C document the same way it builds a fresh parse (6.1).
- **R6 One algorithm.** `Document.parse` is a session that receives the whole
  text in one append and is finalized. There is no second parser, no
  fallback and no size threshold that selects a different algorithm (see
  `AGENTS.md`).
- **R7 Errors.** Out of memory at any stage throws an error. Nothing is
  rolled back.
- **R8 Concurrency.** Published documents stay immutable and `Sendable`. A
  session has one writer.

Non-goals: changing text anywhere but at the end (removed on 2026-10-14, D7),
error recovery, a different dialect, rendering, and a public API for editing
the AST itself.

## 2. The current parse, stage by stage

`markdown_core_parser_parse` in `core/blocks.c` runs one transaction:

| Stage | Where | What it reads | What it decides |
| --- | --- | --- | --- |
| S0 Envelope | `read_document_prefix` (`elements/document.c`) | Leading Properties lines | `Document.metadata` |
| S1 Blocks | `S_parse_source` → `S_process_line`: `check_open_blocks`, `open_new_blocks`, `add_text_to_container` | One physical line at a time through the input index, plus lookahead (`markdown_core_parser_lookahead_*`) and claimed ranges (`claimed`) | The block tree on one open-node spine; leaves accumulate content; reference definitions, headings, footnote and specimen definitions register |
| S2 Close | `finalize_document`, `S_parse_block_inputs` | Open spine; queued cell inputs | Container facts computed from children (list tightness, definition scopes); grid and multiline cell bodies |
| S3 Prepare | `prepare_document` | Specimen and heading registries | Heading labels declared as implicit references; reference map complete |
| S4 Finish walk | `S_finish_parse` → `walk_owned_trees` | Every owned root once | Inline parse of each container at ENTER, inline completion, anchor reservation, Text consolidation, finish steps |
| S5 Document | `finish_document` | Registries | Footnote and specimen order, heading anchors and their reference targets |
| S6 Passes | `postprocess_func` (Autolink) | Each finished root | Bare email links |
| S7 Publish | `publish_document` (`elements/ast.c`) | The finished tree and, in a session, the previous document | Ids, extents and runs; identity matching |
| S8 Bindings | Swift `DocumentBuilder`; Kotlin and ES decode MCB3 | The finished C tree | One immutable value tree per parse |

Three properties of this design make a streaming parser possible without a
second parser:

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

What streaming needs in addition is (a) closing that writes values and
changes no parse state, so a snapshot can finish the tail on the session's
own blocks and leave the session's state as it was; (b) a rule that tells which
decisions the end of the input made, so that every other decision is made
once; (c) inline completion that waits for the definitions a fresh parse
would see; and (d) identity for the nodes that are still changing. Section 5
adds each of them to the line machine.

## 3. Dependency inventory

Streaming is correct only if every way that one part of the source affects a
node elsewhere is known and handled. This is the complete inventory for the
current dialect. Each row names the mechanism in section 5 that handles it.

| Dependency | Examples | Direction | Mechanism |
| --- | --- | --- | --- |
| Carried block state | Open list item widths, blockquote and callout prefixes, definition body indentation, directive fence stack, last-line-blank flags | Earlier line → later lines | Held by the open blocks' builders, which a snapshot leaves as the commit left them (5.2, 5.5) |
| Lookahead | Setext underline, table separator and caption, definition term marker, multiline and grid tables, directive closer, Properties closing fence | Later lines → earlier decision | A decision that sees the end of the input is a candidate, which a later line decides (5.1, 5.3) |
| Retroactive write | A separate-line block identifier sets the preceding block's anchor; a caption after a table | Later line → closed block | A closed block stays a builder until it settles (5.2) |
| Container facts from children | `List.tight`, list layout, definition and definition-list scopes | Children → container | Folded into the container's builder as each child settles (5.6) |
| Unclosed opaque leaves | Fenced code, HTML block, comment, formula block and directive block without a closer run to the end | Opener → rest of the text | The open leaf's builder holds them; its content is appended to, never copied (5.2) |
| Registry lookups in inline parsing | `[label]` and heading labels in the reference map (`link.c`, `heading.c`), `[^label]` in the footnote label map (`footnote.c`), `@id` in the specimen id index (`citation.c`) | Any definition → any occurrence | A snapshot sees the definitions written before the asking root; `finalize` sees all of them and parses again the roots whose answer changed (5.4, 5.10) |
| Document ordinals | Heading anchor `-N` suffixes | Every explicit anchor and every earlier heading → a heading's automatic anchor | A snapshot assigns anchors as `Document.parse` does over the text so far: explicit anchors reserve their names first, then automatic anchors are numbered in source order (5.5) |
| Shared resources | A definition's destination, title and attributes read by every occurrence | Definition → occurrences | Occurrences name the definition's label and read it through the document's lookup tables (4.5) |
| Absolute coordinates | Every `Scope` | Every earlier byte → every later scope | Nodes carry relative extents and runs (4.3) |
| Mapped cell inputs | Grid and multiline cell bodies | Table → cells | The table is a candidate until its closing border; pipe and simple tables are read a row at a time (5.7) |

A dialect change that adds a dependency not covered by one of these
mechanisms must add a row here and its mechanism, the same way a new kind
updates `canonical-ast.json`.

## 4. Public model

### 4.1 Identity

Every `Markup` has `id: MarkupID`, an opaque integer below 2^53 so that
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
- **Stable in a session.** A node that continues a node of the previous
  document keeps its id (5.9). A node that continues none takes the next
  unused id of the session. Ids of removed nodes are never reused by the same
  session.
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
they have the same kind, id, scalar fields, extent and runs (4.3) and
pairwise equal children in every relation.

- Consumers reconcile with ids and this equality: SwiftUI with `Identifiable`
  and `Equatable`, Compose with keys and `equals`, React with keys and
  `React.memo(component, (a, b) => markupEquals(a.node, b.node))`. A node the
  append did not change is equal to its predecessor and has its id, so each
  framework skips it; it is not the same object, because each binding builds
  every document whole (6.1).
- Hashing uses `id` only, so it is O(1) and consistent with equality.
- Swift: every kind is `Hashable`; `any Markup` gets an `isEqual(_:)`
  helper. Kotlin: `equals` and `hashCode` on every kind, with the reference
  check first. ECMAScript has no equality protocol, so it exports
  `markupEquals(a, b)`.

### 4.3 Nodes carry raw extents and runs; scopes are computed on request

A scope has one consumer: side-by-side editing. It maps an element the user
points at back to its source and draws its selection on the canvas. That
feature is not on the hot path of parsing or rendering, and it needs a scope
only when it asks for one. So no node stores a line, a column or an absolute
offset, and nothing counts lines or converts units while parsing or
publishing.

What a node does carry is the raw extent the engine keeps for its own work,
and its runs, the source that is its own. Both are copied as they are,
because a binding answers scope queries in UI code where the parser is no
longer reachable.

```text
Extent(lead: Int32, span: UInt32)       offsets in the parser's input
    lead:   signed, from the end of the previous node in the same relation
            (or the owner's start, for the first node) to this node's start
    span:   of this node's range
Run(lead: Int32, span: UInt32)          offsets in the source
    lead:   signed, from the end of the run before, or, for the first run,
            from the end of the source of the previous node in the same
            relation (or the start of the owner's source, for a relation's
            first node)
    span:   of this run
```

- **One rule for extents.** Every extent is a byte offset in the input of the
  parser that made the node. The block parser reads the source text, so a
  block's extent is an offset in the source. The inline parser reads its
  root's content, which starts at offset 0, so an inline node's extent is an
  offset in that content. Each parser and identity matching (5.9) work in the
  offsets of their own input and need no mapping.
- **Each relation is its own sequence.** Each typed field of an owner and each
  definition body is a relation of its own, and a table's rows, in head,
  content and foot order, are one relation.
- **Runs are a node's own source.** A node's `runs` are its own source, in
  source order, and every node has at least one.
  - Between its first run and its last, a node's runs cover exactly its own
    source: the source between two runs is not the node's, such as the
    container prefixes between the lines of a leaf block inside a blockquote,
    callout or list item, or the other columns between the lines of a grid
    or multiline table cell. Runs that touch are one run.
  - A node's own source on a line can be empty: a multiline table cell's is,
    at the line's end, on a line that ends before the cell's column. Empty
    own source is an empty run, which is one run with a run it touches.
  - A block's source starts and ends with its range.
  - An inline node's runs are its window of the source less the gaps between
    its root's runs. Its window runs from where its root read its first
    content byte to where it read its last, or is the empty range where its
    start was read when it holds no content.
  - The element that reads an inline root's content records, as it reads,
    which source each part of the content came from: `\|` in a table cell is
    two source bytes and one content byte, and a tab in a grid cell is one
    source byte and the spaces it becomes. The engine places each inline
    node's runs from that record, so no other code knows how an element turns
    source into content. The record is the engine's; the model carries only
    the runs.
- The engine stores extents and runs on every C node, and the bindings copy
  them verbatim, like any other field.
- `lead` is signed because a relative offset between two ranges has no sign
  of its own: ranges may overlap or nest in any way the spec defines, and the
  encoding does not assume otherwise. An inline note's `Footnote` covers
  `^[content]` while its owning `Citation` covers only the content
  (`canonical-ast.md`), so the note's `lead` is −2. No node needs a rule of
  its own for this.
- Extents and runs are relative, so a node's values do not depend on how much
  text precedes its owner, and a settled node's values never change while the
  stream grows (5.2). They are part of equality (4.2).
- **Scopes.** `document.scope(of: node, in: source) -> [Scope]` returns one
  scope per run of the node, in source order, which is what an editor draws
  as the node's selection. `document.node(at: Position, in: source)` hits a
  node only inside one of its runs. Both take the source text the document
  was parsed from, which the side-by-side editor already holds
  (`session.text` for a session's current document). They return today's
  editor line and column conventions and sentinels, in the session's
  coordinate unit (4.4).
  - Every binding computes them with one walk that sums the leads of the
    runs from the document root, so no binding repeats an element's syntax
    (closing sequences, cell padding, column geometry), and extents take no
    part.
  - The line and unit conversion scans the source. This cost is paid only by
    the query.
- Walker callbacks carry no scope.
- A range's line and column ends follow one rule for every node: a range
  ending right after line `L-1`'s terminator ends at `L:0`. So `SoftBreak`,
  `LineBreak` and a `Citation` that end on a line terminator end there too,
  and a zero-byte document is `1:1..1:0`, as a document of one newline is. A
  grid or multiline cell's run on a line where its part is blank is empty
  there.
- The canonical dump prints the ranges by this rule, so it is a scope query
  and takes the source like one: `document.dump(in: source)` and
  `document.dump(node, in: source)` on every binding, and
  `markdown_core_document_dump(document, source, ...)` in C.

Rollout step 4 brings runs to this form: until then, only blocks whose source
is not one range and inline roots carry runs, each with the content length it
became, and an inline node is placed through its root's runs (D1).

### 4.4 Sessions

A session owns a text, its current document and the open state of its parse.

```swift
public final class MarkdownSession {           // one writer; not Sendable
    public init(_ source: String = "", unit: TextUnit = .utf16) throws
    public let unit: TextUnit                   // columns of scope queries
    public var document: Document { get }       // immutable, Sendable
    public var text: String { get }
    @discardableResult
    public func append(_ text: String) throws -> Document
    @discardableResult
    public func finalize() throws -> Document
}
```

- **`append`** adds text to the end and returns a snapshot (5.5). The initial
  source is the first append.
- **`finalize`** ends the stream and returns the final document (5.10), which
  equals `Document.parse` of the text in everything but ids (R1). The caller
  ends the session explicitly; nothing ends it implicitly.
- **After `finalize`** the session is read-only: `document` and `text` stay
  readable, and `append` and `finalize` throw the finalized error. C answers
  `MARKDOWN_CORE_SESSION_FLUSHED`.

Kotlin has the same shape as an `AutoCloseable` class. ECMAScript exports
`class MarkdownSession` with `dispose()` (and a `FinalizationRegistry`
backstop), because its state lives in WebAssembly memory. C exposes
`markdown_core_session_new`, `_append`, `_finalize`, `_document` and `_free`.
C views borrow from the session until its next append, its `finalize` or its
release.

- **One coordinate unit.** A session, and every document it publishes, counts
  columns in one `TextUnit`, `.utf8` or `.utf16`, chosen when the session is
  created: every column a scope query returns (`Document.scope(of:in:)`,
  `Document.node(at:in:)`) and every position it takes. `Document.parse`
  takes the same parameter. C defaults to UTF-8. Bindings default to UTF-16,
  because the editor surfaces on all three platforms count UTF-16 code units:
  UIKit and AppKit `NSRange` and TextKit, Android `Editable` and Compose
  `TextFieldValue`, Monaco, CodeMirror, and the default position encoding of
  the Language Server Protocol. Every binding also offers `.utf8`.
- **Appended text is stored as given.** The engine treats the text exactly
  like a valid stream, with no validation and no conversion. A binding
  appends its platform's own string, whose scalars are whole; a C caller may
  split a scalar between two appends, and the bytes join as written.
- **Storage stays UTF-8.** The unit is how columns are counted, not how the
  text is stored. A binding takes its platform's own string. Swift's `String`
  is already UTF-8; Kotlin and ECMAScript strings are transcoded once at the
  boundary, as today. Storing UTF-16 in the engine would double the memory of
  ASCII text and rewrite every byte scanner of the grammar.
- **Columns in the unit.** Extents and runs are bytes (4.3). A column is
  converted to the session's unit only when a scope query asks for it. The
  canonical dump and the conformance fixtures stay in UTF-8 columns.
- **`Document.parse`** keeps its signature apart from the unit parameter. It
  is a session that appends the whole source once, is finalized and is then
  discarded (5.10).

### 4.5 Definitions stay where they are written

Before step 1, `Document.footnotes` and `Document.specimens` owned every
footnote and specimen definition, lifted out of the place where it was
written, and an inline note `^[body]` got a generated id `inline-N` so its
`Citation` could name the lifted `Footnote`. That lift is what gave
definitions their special cases: the generated ordinal (every later inline
note changes when one is inserted, and an authored `[^inline-3]` label
renames a note elsewhere), positions measured along a list whose entries are
scattered across the document, and identity decided in a list that is not
where the parser produced anything.

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
  the ids of every definition in source order, as the C registries hold
  them. A binding receives them with the document (6.2) and resolves them to
  its nodes while it builds the tree (6.1). `document.footnotes`,
  `document.specimens` and `document.references` list those definitions in
  source order. `document.reference(for: label)` returns the label's winner,
  a `Reference` or, after every `Reference`, a heading whose label it is,
  whose destination is its anchor. `document.footnote(for: label)` and
  `document.specimen(for: label)` return the first one whose stored label
  equals the referent's (the parser has already normalized both), as the
  spec defines today.
- Definitions therefore follow the tree's ordinary identity rules (5.9),
  with no rule of their own.
- Walks and the canonical dump visit a definition where it was written. A
  renderer that prints notes at the end of the page reads the lookup, and
  display numbering stays the renderer's job, as the spec already says.

This changed `canonical-ast.md`, `dialect/footnotes.md`,
`dialect/specimens.md`, `dialect/links-and-images.md`,
`dialect/attributes.md`, the canonical dump of documents with definitions
and their fixtures, and every binding (D2).

## 5. Engine

### 5.1 One rule

Every mechanism of the engine follows from one rule:

**A decision made after seeing the end of the input is provisional; every
other decision is final once it is made.**

- At the block level, every decision that did not see the end of the input
  is committed, the container steps of the line that is not complete yet
  included (5.3).
- At the inline level, the nodes of an open paragraph before its earliest
  provisional decision are settled (5.8).

A decision sees the end of the input when its lookahead reaches the end of
the input, when it asks for the last line before that line is complete, or
when it asks whether a line is the last one. Every such
question goes through one entry of the lookahead API, and
`scripts/audit/check-input-end.mjs` refuses any other comparison with the end
of the input.

### 5.2 What a session holds

| State | Contents |
| --- | --- |
| Text | One contiguous append-only byte buffer, which the parser reads directly |
| Line index | The geometry of each complete line (start, end, normalized view for NUL), append-only |
| Builder stack | One builder for each open block, from the document to the deepest |
| Settled nodes | Immutable nodes, owned by the session |
| Registry log | Every definition (reference, footnote, specimen, heading label) and anchor, in source order, with marks that it can roll back to |
| Completion queue | The closed blocks whose inline roots are not yet parsed |
| Previous tail | The previous document's tail nodes (5.5), used only to give the next document's tail its ids (5.9) |

**Builders and nodes.**

- An open block is a builder. It holds the block's kind, its continuation
  parameters (a list item's indentation, a fence), the content it has read,
  its runs, its closed children and the element's own open state.
- A block **settles** when its parent takes its next child or closes. A
  block that is closed but not settled is still a builder, because a
  trailing line can still change it: a caption after a table, or a block
  identifier after a list, callout or table.
- A block is one object for its whole life. Closing it writes its closed
  values into that object: its end, its content view and the values it folds
  from its children. Closing changes no parse state.
- While the stream runs, a settled node changes only when a later explicit
  anchor changes a heading's automatic anchor (5.5). `finalize` changes two
  kinds of settled node in place: inline roots whose answer a later
  definition changes, and headings whose anchor changes (5.10).

A snapshot does not copy the committed blocks. It writes the closed values
into the original objects and leaves the parse state as the commit left it.
Only when the provisional reading changes a committed block's kind or parse
state does it copy that one block, for example when `===` turns a paragraph
into a Setext heading. The published document is the committed tree plus the
provisional part, valid until the next call.

**Append-only buffers.** A builder's content bytes, its runs and its array of
closed children only grow at the end. Each builder records its committed
length; a snapshot writes past it, and the next call cuts it back to the
committed length. This holds because a session has exactly one live C
document at a time: a session's document borrows from it until its next
call, a document `Document.parse` returns owns its nodes, and a binding
converts a document into platform values before the call returns.

A published node views these buffers, so the literal of a growing code block
is not copied per append, and its runs and children are not either. Committed
bytes are written once and never move.

**Positions.** A node's extent and runs (4.3) are computed when it is first
published: from the end of its previous sibling, or from its parent's start
for a first child. The text only grows at the end, so neither that sibling's
end nor the parent's start changes again, and a settled node's position
values are final. An open block grows at its end, so each publish writes its
extent and runs again.

### 5.3 Commit

`append` adds the new bytes to the end of the text and then commits: the
line machine reads on the builder stack from the last step the last commit
read, which may lie inside the line that is not complete yet. The commit
takes every decision that does not see the end of the input (5.1): it reads
every complete line, and of the last line each step that reads only bytes
the line has. A line is complete when it ends with `\n`, or with `\r`
followed by another byte. Blocks that closed join the completion queue, and
so does each field a committed line wrote: a callout's title, a definition's
term, a block directive's label and a table's caption.

**Candidates.** Some constructs are decided only by later lines: the headers
of simple, multiline and grid tables, a caption before a table, a definition
list's term, a blank line in a definition's body, a block identifier line,
an unclosed grid table, a comment block and the Properties envelope at the
start of the document. When the decision of such a construct sees the end of
the input, the commit makes it a **candidate** and reads on with the main
reading, the reading in which every live candidate is rejected: the ordinary
blocks, committed line by line. Each candidate keeps its own state and is
fed each complete line once, until a line decides it:

- an accept puts the commit back at the candidate's opening line, and the
  lines from there are read again, with the construct claiming them;
- a reject drops the candidate's state, and the main reading stays.

A decision that sees the end of the input waits for the next commit. The
lines of the main reading from a live candidate's opening line are
**undecided**. A snapshot decides each candidate as the text ends (5.5).

### 5.4 Completion

The blocks and fields of the completion queue complete in the order they
joined it: their inline roots are parsed, the element's finish steps run, and
the blocks in table cells are read.

A block completes at the next publish or at `finalize`:

- at a publish, a lookup sees only the definitions that start before the
  asking inline root (5.5), and every lookup that finds none is recorded with
  its root, its kind and its label;
- at `finalize`, a lookup sees every definition and records nothing.

Every definition is registered when its block closes or earlier (a footnote
or specimen definition at its opening line), and a block that starts after a
definition closes after it. So when a closed root completes, every definition
that starts before it is registered.

`Document.parse` publishes nothing before `finalize`, so each root is parsed
once and sees every definition, in the order `main` has today: blocks, then
definitions, then inline content, then anchors.

### 5.5 Snapshot

**What a snapshot is.** The document an append returns equals
`Document.parse` of the text so far, with one difference: a definition lookup
(reference link, footnote, specimen, heading label) sees only the definitions
that start before the inline root that asks. Headings take their anchors as
`Document.parse` assigns them (docs/specs/dialect/anchors.md): every explicit
anchor of the text so far reserves its name first, then automatic anchors are
numbered in source order.

The document's own tables (4.5) are those of the snapshot's nodes, as
`Document.parse` computes them. The difference depends only on positions, so
a snapshot does not depend on how the text was split into appends. A
definition written after its use leaves the `[x]` before it as literal text
until `finalize`, which is the jump in the text that the owner accepted for
streaming (D8).

**How a snapshot is made**, in this order:

- Complete the completion queue (5.4).
- Record a mark in the registry log.
- Decide each candidate as the text ends: one whose construct the text
  ends puts the reading back at its opening line, and the snapshot reads the
  lines from there with the construct claiming them; the others leave the
  main reading.
- Process the rest of the text on the committed builders, copying first each
  block whose kind or parse state that reading changes (5.2), then write the
  closed values of every open block into its object at the true end of the
  input.
- Complete the tail by the publish rule of 5.4, without recording the
  lookups that find none.
- Publish in place: give ids to the nodes that are not settled (5.9),
  compute the extents and runs of the tail and of the open blocks, and
  assemble the document.

The **tail** is the nodes a snapshot makes from the rest of the text, from
the lines of a candidate it accepts and from the inline content of an open
paragraph. At the start of the next call each
copied block takes back its state, the open blocks cut their buffers back to
their committed lengths, and the registry rolls back to the mark. The tail's
nodes stay until the next publish has given ids to its own tail (5.9).

### 5.6 Container folds

Some of a container's values come from its children, such as whether a list
is tight or loose. Each child's contribution is folded into the container's
builder when the child settles, so a snapshot looks at the last child only,
and one more item on a list of 10,000 costs O(1), not O(items).

### 5.7 Tables

- A pipe table opens at its delimiter row, and its header is the last line of
  the paragraph before it. From then on each line is a body row, read and
  settled on its own.
- A simple table opens when its header, its dashes and its first row are
  there, and is then read a row at a time too.
- Today a table reads its whole extent with one lookahead when it opens.
  That becomes a continuation, one row at a time.
- A grid table, a multiline table and a headerless simple table know their
  row structure only at their closing border. Until then each is a candidate
  (5.3), fed each line once, and its lines are undecided (7.2).

### 5.8 The settle point of an open paragraph

An open paragraph's inline content is parsed again for each snapshot,
because one later byte can change what the content before it means: a `*`
closes an emphasis, a backtick closes a code span, a `]` turns what came
before into a link. The inline level applies the same rule as the block
level:

- An inline decision that saw the end of the content is provisional. The
  provisional decisions are: an unmatched opening delimiter run, an unclosed
  bracket, a backtick run with no closer, an autolink, HTML, entity or link
  destination scanned to the end, and the last Text node, which autolink
  completion and Text consolidation can still change.
- The **settle point** is the latest start of a top-level token at or before
  the start of the earliest provisional decision where nothing read before
  it is still open (a bracket, a citation, a code or formula body) and a
  top-level node starts after a node that is not Text. The paragraph's
  top-level inline nodes before it are settled and kept by the session for
  the paragraph, and the next snapshot parses from the settle point.

When the paragraph closes, only the content after the settle point is parsed
too. So a paragraph's inline content is parsed in full once, plus, on each
append, the short stretch after the settle point.

The kept nodes stay the paragraph's while its content goes on from them. A
candidate that is still deciding can put a completed paragraph back open,
and the kept nodes return to the session with it. When the content stops
going on from them, as when a table takes the paragraph's last line or a
trailing identifier leaves it, the paragraph's content is parsed in full,
and the next publish matches it against the kept nodes as it matches
against the nodes a snapshot made (5.9).

### 5.9 Identity matching

- A settled node never changes its id.
- At each publish, every node that is not settled (the tail, the open
  blocks and the nodes that settled since the last publish) takes its id
  from the previous document's unsettled nodes by one rule: within the same
  owner relation, a new node continues the
  earliest old node of the same kind whose start lies in the new node's
  source range, and takes its id. A node that continues none takes the
  session's next unused id. The text only grows at the end, so an old node's
  start is at the same offset in the new text, and matching compares
  offsets.
- An inline node's start is an offset in its root's content (4.3). An open
  paragraph's content only grows at its end until the paragraph closes, so
  inline nodes match by the same rule.
- When a new node contains the starts of several old siblings, it takes the
  earliest not yet passed, and the ones after it remain for the next new
  sibling: the cells of a grid table that span rows start inside the ranges
  of the cells before them. Both sequences are in source order and the match
  is monotone, so it is linear in the tail.
- Matching visits only the tail, so it costs O(tail nodes).
- Consequences, each from the one rule:
  - Typing at the end of a paragraph keeps its id.
  - `==` typed after a paragraph makes a Setext heading in the snapshot,
    with a new id. One more `x` makes `==x`, and the paragraph is back with
    a new id again, because an id never changes kind and the earlier
    paragraph's id retired with it.
  - A row appended to a pipe table changes the table, the new row and the
    document; every earlier row is settled.
- `Document.parse` publishes once and numbers its nodes from 1 in canonical
  walk order (4.1).

### 5.10 `finalize` and `Document.parse`

`finalize`, in this order:

- Commits the rest of the text, decides each candidate at the true end of
  the input and closes every block.
- Completes the completion queue, with lookups that see every definition.
- Checks each lookup recorded by 5.4 that found no definition; if one is
  defined now, parses that root again in place.
- Assigns every heading anchor by `main`'s rule: explicit anchors reserve
  their spellings first, then automatic anchors are numbered in source
  order.
- Publishes.

An inline parse reads from a lookup only whether its label is defined (4.5),
so a lookup that found a definition before its root finds one with every
definition visible too, and only the recorded misses can change.

`Document.parse(text)` is a new session that commits every complete line of
the text and is finalized (R6).

### 5.11 Why the results are right

- **Committed decisions.** A committed decision did not see the end of the
  input, so it read only bytes that were already there and decided nothing
  from where the text ended. The line machine is a deterministic function of
  its state and the bytes it reads, so a step committed at an earlier append
  is processed exactly as a parse of any longer text processes it. A
  candidate is the one decision a later line still makes: the main reading
  is the parse in which it is rejected, and an accept puts the commit back
  at its opening line and reads on as the parse that claims it. By induction
  over the steps, the builder stack after a commit is the state a parse of
  the whole text has after the last committed step, once the live candidates
  are decided.
- **Snapshots.** A snapshot decides the live candidates and processes the
  rest of the text at the true end of the input, from that state, which it
  leaves as it found it (5.2). Every line of the text is then processed in
  order, by the same line machine, with the end where it really is, so the
  blocks are those of `Document.parse` of the text so far. Completion runs
  the same inline parser on the same content, against the definitions before
  each root, all of which are registered then (5.4). None of this depends on
  where earlier appends ended, so neither does the snapshot.
- **The final document.** `finalize` commits the rest of the text, decides
  the live candidates and completes the queue with every definition visible, as `Document.parse`
  does. A root completed earlier saw the definitions before it; the only
  answers that change with every definition visible are the recorded
  misses, and those roots are parsed again. Anchors are assigned with the
  full rule. So every field equals `Document.parse` of the text.
- **Settled nodes.** A settled block is finished from a builder no later line
  can change (5.2), and a settled inline node lies before every provisional
  decision of its paragraph (5.8), so neither is ever made again.

This argument is also the test oracle (section 8).

## 6. Bindings

### 6.1 Bindings build values from the C document

After an append, a binding converts the session's C document into platform
values node by node, exactly as it converts a fresh parse today, and returns
the result. The C document is its only input. The conversion's cost is the
platform's cost of building an immutable value, which grows with the whole
document; the design keeps it (D5) and reports its C side per append
(section 8).
Nodes the append left unchanged reach the consumer as equal values with
unchanged ids (4.2), which is what SwiftUI, Compose and React reconcile.

### 6.2 Wire format MCB3

Kotlin and ECMAScript receive a parse as one message. MCB3 extends MCB2
(`docs/architecture/wire-format.md`) and keeps its post-order stack model.
Every node record carries `u64 id`, the node's `Extent` in place of `Scope`,
and its runs (4.3). The message ends with the footnote, specimen and
`Reference` tables and the reference label table, each label with the id of
the node it resolves to (4.5). Every message is a whole document. The magic
is `MCB3` because the record layout changed.

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
  (retained by a view) is only released, which ends at a count decrement.
  Stack depth is constant in tree depth; the array holds at most the nodes
  being freed. Moving children out is the only mutation, and it happens only
  to a record that nothing else references, inside `deinit`. Records are
  therefore immutable to every observer and `Sendable` (`@unchecked`, with the
  invariant stated at its one use and an audit that no other code writes the
  storage).
- **Traversal.** Deep equality, the walker, conversion (6.1), scope
  queries and hit testing (4.3) and `description` use
  explicit work stacks. Hashing reads only the id. Kotlin (`equals`,
  `toString`) and ECMAScript (`markupEquals`) follow the same rule, because
  their stacks are finite too; their garbage collectors need no rule for
  release.
- **Gate.** The 30,000 and 65,536-level tests cover release, releasing a deep
  document while a view still holds one of its subtrees, equality of two deep
  documents that differ only at the deepest leaf, walking, `scope(of:in:)`,
  `node(at:in:)` and `description`, on every binding. They run on a thread
  with a small fixed stack, so a recursion regression fails deterministically
  instead of depending on the platform's default stack size.

Kotlin and ECMAScript already hold one object per node, so this brings Swift
to the same model instead of keeping a Swift-only layout.

### 6.4 Kotlin

Every kind has reference-first `equals` and id-based `hashCode`. Collections
stay read-only `kotlin.collections.List`. The package ships a Compose
stability configuration file that lists the AST types as stable, because the
Compose compiler treats classes from a module without the Compose compiler as
unstable. No Compose dependency is added.

### 6.5 ECMAScript

Values stay plain readonly objects. Lists key by id, and `React.memo` takes
`markupEquals` as its comparator (4.2). The session holds a WebAssembly
handle and must be disposed. A `Document` never holds a handle.

## 7. Complexity

### 7.1 Bounds

Let `c` be the appended bytes, `d` the depth of the builder stack, `s` the
content of the open paragraph after its settle point, `t` the tail's nodes
and `p` the total length of the undecided lines.

| Case | Work |
| --- | --- |
| An append inside a paragraph | O(c + d + s + t) |
| An append inside a code block or an HTML block | O(c + d + t) |
| A row appended to a pipe or simple table | O(c + d + the row) |
| An item appended to a list | O(c + d + t) |
| Completion | Each block completes once in its life, O(n) in all, as in a fresh parse |
| `finalize` | O(p + queued blocks + roots parsed again + headings) |
| `Document.parse` | O(n), as on `main` |

No bound depends on a size threshold. A binding builds each published
document whole (6.1); that O(nodes) is the platform's construction cost and
is outside these bounds (D5).

### 7.2 Costs that are the language's, not the algorithm's

In these cases the undecided region can be arbitrarily long, because the
language decides its meaning only at a later line or at the end:

- An unclosed grid table, multiline table, headerless simple table or comment
  block: each append processes its lines from its opening line again.
- A document that starts with `---`: any later `---` line can close the
  Properties envelope, so every line is undecided until one does or the stream
  ends.
- An unmatched `**`, backtick run or `[`: the settle point stays before it,
  and each append parses the rest of the paragraph again.
- A very long line: each append processes that line again.

## 8. Testing

The oracles below, the workloads they run on and the stream benchmarks are
specified in [Gates for incremental parsing](2026-09-29-incremental-gates.md),
which also says at which rollout step each one becomes a gate.

- **Final document.** Every stream script ends with `finalize`, after which
  the canonical dump, every scope and every definition query equal those of a
  fresh `Document.parse` of the text. This runs in C and in each binding, in
  both units.
- **Chunking independence.** After every append, the snapshot equals the
  snapshot a new session returns for one append of the whole text so far,
  ids aside. Corpus documents are fed in chunks of every size from one scalar
  up, and small documents are split at every scalar boundary.
- **Snapshot meaning.** After every append, the snapshot equals
  `Document.parse` of the text so far everywhere except at the inline roots
  that use a definition written after them and the headings whose text or
  anchor depends on such a root. The harness finds those places from the
  fresh parse's definition tables, anchors and scopes, not from the engine.
- **Identity and minimality.** After every append: ids are unique; no id
  changed kind; no retired id returns; every id follows the matching rule
  (5.9); in C and in every binding, a node equals the previous document's
  node with its id exactly when its value is unchanged. Scripted cases assert the exact sets of kept, new
  and retired ids: typing at the end of a paragraph, a paragraph that becomes
  a Setext heading and back, a row appended to a table, an item appended to a
  long list.
- **Definition queries.** `footnotes`, `specimens`, `references`,
  `footnote(for:)`, `specimen(for:)` and `reference(for:)` for every label in
  the text equal those of the fresh parse, and fixtures with nested,
  duplicate, anonymous and unreferenced definitions check each binding's
  answers against the winners of the C registries, since the canonical dump
  does not call the queries.
- **Errors.** An append or `finalize` after `finalize` fails with the
  finalized status. The allocator-seam OOM sweep fails every append and
  `finalize` at every allocation boundary and asserts that the call throws
  and that freeing the session leaks nothing.
- **Audits.** `scripts/audit/check-input-end.mjs` enforces the single
  lookahead entry of 5.1.

## 9. Rollout

Each step is one pull request, leaves `main` releasable and passes the gates
that [Gates for incremental parsing](2026-09-29-incremental-gates.md#7-activation-by-rollout-step)
activates for it.

- [x] **Step 0: Gates.** The edit and stream workloads, the correctness
   harness with the `reparse` subject and its faulty-subject self-tests, and
   the edit and stream benchmarks reporting the reparse baseline, in the
   existing benchmark workflow.
- [x] **Step 1: Model.** Ids for fresh parses, deep equality and hashing, raw
   extents in nodes with on-demand scope queries, MCB3, the Swift record
   storage, the coordinate unit (4.4), and definitions kept where written
   (4.5).
- [x] **Step 2: Sessions with a whole-document restart.** The session API on
   every platform, identity matching and value deduplication. Every call
   parses the whole text again.
- [x] **Step 3: Reference nodes and content runs.** Link reference
   definitions are `Reference` nodes where they were written, and reference
   links name their label (4.5). Inline extents are offsets in their root's
   content, inline roots carry content runs, scope queries answer `[Scope]`,
   and MCB3 carries runs and the reference tables (4.3, 6.2).
- [x] **Step 4: Node runs.** This plan replaces the general-edit engine.
   Every node's runs are its own source and scopes come from runs only;
   `Run` loses its length; a table's rows are one relation (4.3); the spec,
   C, the three bindings and MCB3 follow. A paragraph that holds only a
   formula stays a paragraph (D9).
- [x] **Step 5: No edits.** `edit` and `TextEdit` leave C and every binding,
   with the session's text tree and the harness's edit scripts and their
   oracles (D7).
- [x] **Step 6: Builders.** Open blocks are builders, finishing is a pure
   function and settled nodes are immutable (5.2); the completion queue
   replaces the finish walk (5.4); the registry is a log with rollback marks;
   `Document.parse` is a commit and a `finalize` (5.10).
- [x] **Step 7: Streaming session.** The append-only text, the commit rule and
   the single lookahead entry (5.1, 5.3), snapshots (5.5), tail ids (5.9),
   `finalize` in C and every binding (4.4, 5.10), container folds (5.6) and
   pipe and simple tables read a row at a time (5.7).
- [x] **Step 8: Settle point.** The settle point of an open paragraph (5.8).
- [ ] **Steps E0 to E3: Edits.** Gates, the C edit, the bindings and the LSP
   server's edit step (12.9).

## 10. Decisions for the owner

- **D1 Positions. Decided 2026-09-29: raw extents, scopes on request.** Nodes
  carry only the engine's own byte extent (signed lead from the previous
  sibling, span), copied verbatim to every binding. `document.scope(of:in:)`,
  `document.node(at:in:)` and the canonical dump `document.dump(in:)` compute
  today's editor line and column scope on request, because side-by-side
  editing is the only consumer and is not on the hot path. Rejected:
  absolute scopes in nodes, and line and column spans in nodes, which make
  every parse and publish count lines and convert units.
  Revised 2026-10-04: inline extents are offsets in their root's content, the
  bytes the parser reads. Rejected: inline extents in source bytes, which
  made the inline parser map every content offset back to the source.
  Revised 2026-10-05: each inline root records its content runs, from which
  every binding maps an inline node's range to the source with one walk.
  Revised 2026-10-06: runs are the one record of where a node's source lies,
  and pieces are removed.
  Revised 2026-10-14: every node's runs are its own source, and scopes come
  from runs only (4.3); the owner kept this model, "那个为修正表格的scope做的node
  runs的设计可能需要保留，我们需要正确的表达每个node的位置的能力", when the
  general-edit engine was dropped. Rejected 2026-10-14: an inline node without
  runs of its own, placed through its root's runs, which saves about 0.07 of
  a fresh parse's AST stage; the owner declined it.
- **D2 Definitions. Decided 2026-09-29: definitions stay where written.**
  Footnote and specimen definitions remain in the tree where they were
  written, an inline note's `Footnote` is owned at its call site, and the
  document carries the parser's footnote and specimen tables for label
  lookups instead of owning the definitions (4.5).
  Revised 2026-10-04: a link reference definition is a `Reference` node
  where it was written too, and a reference link names it by label as a
  `Citation` names a footnote, so every declaration is a node and no
  occurrence copies a definition's values. Rejected: keeping `inline-N`,
  where inserting one note changed every later note; a separate `InlineNote`
  kind, which would express footnote semantics with a second model; and
  lifted definitions named by `MarkupID`, which kept special identity and
  position rules for the lifted list.
- **D3 Swift storage. Decided 2026-09-29: per-node records,** on the
  condition that no operation recurses over tree edges (6.3). This restores
  Swift's original tree, which #240 had flattened only to bound ARC release
  depth; iterative release bounds it directly.
- **D4 Coordinate unit. Decided 2026-09-29: one unit per session.** Returned
  columns use the session's unit, UTF-16 by default in bindings and UTF-8 in
  C, and text is stored as UTF-8 (4.4). The rejected alternative was UTF-8
  everywhere, which leaves every editor integration to convert `NSRange` and
  JavaScript offsets itself. Revised 2026-10-14: sessions take no offsets
  (D7), so the unit applies to scope queries only, and the session's text
  tree is removed.
- **D5 Bindings. Decided 2026-09-29: every binding builds each document
  whole from C (6.1).** Unchanged nodes reach consumers as equal values with
  unchanged ids, and frameworks compare them with deep equality. Rejected:
  sharing objects between successive documents, which would give the
  binding state of its own (a table of previous objects, or the previous
  document as a second input) and the engine reports to maintain it.
  Confirmed 2026-10-14 for streaming ("保持D5"): from step 7 the benchmark
  reports what building the platform values costs per append, without a
  gate. Decided 2026-10-19 ("只计 C 侧 Ir"): the report counts the part of
  the build that reads the C document, in callgrind Ir on the gates'
  machine, and not building the platform objects (gates G14).
- **D6 Node storage. Decided 2026-10-01: shared subtrees.** Superseded
  2026-10-14 with the general-edit engine (D7): a session has one live
  document at a time, so nothing shares subtrees across documents, and
  settled nodes are owned by the session (5.2).
- **D7 Streaming only. Decided 2026-10-14.** The owner: "我们实际只有streaming的追加token更新一种需要高吞吐的实时编辑能力，因此，鉴于现在性能的现实，你可能需要彻底放弃现在的所有design". The general-edit
  engine of the previous revision (edit pass, cursor take and descend, E1–E5,
  reverse indexes and order labels, inline reuse, shared subtrees) is
  dropped, and "Edit删掉": `session.edit`, `TextEdit` and the edit workloads
  are removed (step 5).
  Revised 2026-10-27: a finalized session takes edits (section 12).
  Streaming stays the only workload that appends, and the parse and append
  paths do no work for edits. The general-edit engine stays rejected: an
  edit re-reads only the damaged block and splices the rest by the line
  machine's determinism (5.11), with no edit records on nodes.
- **D8 Lookups while streaming. Decided 2026-10-14.** The owner: "对于llm
  session streaming，你甚至可以在session finalize时再去做definition look up，因为token streaming过程中的文本跳变本来就是无法避免的。", and "你需要加一个session.finalize的API，由调用方显式结束当前session。" A snapshot's lookups
  see the definitions written before the asking root, and `finalize` applies
  the full rule (5.5, 5.10).
- **D9 Formula paragraphs. Decided 2026-10-12: no lift.** A paragraph whose
  only content is a standalone formula stays a paragraph holding a `Formula`;
  it does not become a `FormulaBlock`, so no block changes kind after its
  inline content is parsed.
- **D10 Anchors while streaming. Decided 2026-10-16.** The owner: "你在这里纠结的问题我们的语法spec里有答案，attributes里的锚点事attributes修饰的对象的，title的锚点指向title本身". A snapshot's headings take their
  anchors by docs/specs/dialect/anchors.md, as `Document.parse` does over the
  text so far (5.5).

## 11. Rejected alternatives

- **Returning a diff or patch.** Excluded by the requirement. Consumers
  already reconcile by identity and equality; a second protocol would be a
  parallel source of truth.
- **Re-parse everything on every append, then reconcile the trees.** It
  achieves R3 and R4 but not R2: it is O(n) per streamed token. It survives
  only as the test oracle and as rollout step 2.
- **The general-edit engine** (the previous revision of this plan, built in
  #432 and closed on 2026-10-14). Re-parsing any edit against the old tree
  required, on every node, an entry and a reach, persistent children trees,
  ordered registry facts and inline reuse records. Writing them cost every
  fresh parse about 1.7 times `main`'s AST stage, and the only workload that
  needs real-time updates is streaming (D7). Section 12 edits a finalized session without
  any of these records.
- **Re-parsing the last top-level block on every append.** A model often
  writes a whole answer as one list, and then the last top-level block is the
  whole text.
- **Persistent trees shared across documents** (reference-counted subtrees,
  balanced children trees). A session has one live document, so nothing would
  share them (D6).
- **Parsing a block's inline content when it closes, and again when a later
  definition answers its lookups** (#432). In a fresh parse every definition
  written after its use parses that root twice. Here a fresh parse completes
  at `finalize`, and a stream's lookups see what is written before them.
- **Copying every open block for each snapshot and matching the copies back
  to the previous document.** A block's opening decision is final, so its
  kind and id are already known; the copies and the matching cost O(depth)
  per append and decide nothing.
- **No lookups at all before `finalize`.** Definitions written before their
  use would not show either while the text streams, and nothing requires
  that.
- **Content-hash or path identifiers.** A hash changes on every change of the
  node, so a growing paragraph would lose its view state. A path changes when
  an earlier sibling is inserted. Neither is stable.
- **A separate streaming parser.** Two parsers diverge. `Document.parse` is
  the same session receiving the whole text at once (R6).
- **Content mapping rules in each binding.** Each binding could map inline
  offsets back to the source by repeating each element's rules (trimmed
  closing sequences, cell padding, escaped pipes, tab expansion). That
  copies the grammar into three languages. The runs the parser records carry
  the mapping as data (4.3).

## 12. Edits on a finalized session

### 12.1 Requirement and the constraint from #432

An editor replaces a range of a document's text with new text, anywhere in
the text. The editor is the notebook, and the LSP server through `didChange`.
After the edit:

- the document must equal `Document.parse` of the new text, ids aside;
- the nodes the edit did not touch keep their ids;
- **re-parsing is limited to the damaged block**: the block whose own bytes
  the edit touched, plus the sibling before it (12.3). Its ancestors are
  rebuilt from their children without parsing anything. All other blocks
  are kept as they are, at any depth.

The constraint comes from the previous general-edit engine (#432, section
11). It made every fresh parse cost about 1.7 times `main`'s AST stage,
because it wrote edit records on every node and kept persistent children
trees, ordered registry facts and inline reuse records, all on the parse
path. So the one rule of this section is:

**An edit uses only what a parse already keeps. The parse path and the
append path do no work for edits.** The parse and stream gates (1.02 times
the base, in Ir) prove this on every PR.

### 12.2 Why this can be cheap now

Three things the streaming engine already has turn an edit into a splice:

- **Relative positions.** Extents and runs are relative (4.3). A node's
  `lead` counts from the end of its previous sibling, so a block after the
  edit keeps all of its values when text before it grows or shrinks. Only
  the first kept sibling after the damage, at each level, gets a new `lead`.
  No offset shifts through the tree.
- **A deterministic line machine.** The line machine is a function of its
  state and the bytes it reads (5.11). Two readings that reach the same
  state with the same bytes left give the same blocks from there on.
- **Owned, immutable nodes.** Settled nodes are immutable and owned by the
  session (5.2), so the session can keep the ones the edit did not reach.

### 12.3 The edit

**A session's life.** A session streams by `append` until `finalize`, and
takes edits only after `finalize`. Streaming and editing are two phases of
one session, not two options: no workload edits text while it streams, and
no session is copied to do both (E-D1). An edit on a session that is still
streaming fails with `MARKDOWN_CORE_SESSION_STREAMING`, a new status. An
append after `finalize` still fails with the finalized status.

**The call.** `markdown_core_session_edit(session, start, end, text, length)`
replaces the bytes from `start` to `end` with `text` and publishes a final
document. Bindings take positions in the session's unit, as `locate` does.
Let `delta = length - (end - start)`.

**Rest points.** A rest point is the start of a child of a container:
- the document;
- a block quote, list, list item, callout, footnote or specimen definition,
  or block directive;
- a table, whose pipe and simple rows are its children (5.7).

At that point no candidate (5.3) is live, so the builder stack holds exactly
the open chain of ancestors. Each ancestor's builder state at a rest point
is a function of its node and of the children before the point. Its
continuation parameters are on the node (a list item's marker offset and
padding, a callout's marker, a fence). Its folded values come from those
children (5.6). Each container element gains one function, `reopen`. It
rebuilds that builder from the node and a child index. It runs only on the
edit path.

The edit runs in five steps:

- **Step 1: Damaged block.** Walk down from the document to the deepest child that
  holds the edit range inside its own runs (4.3). The runs tell a child's
  bytes from its container's own: the `>` prefixes, list markers and
  fences. The walk stops at a block whose container syntax the edit
  touches. Typing a `>` or a list marker therefore damages the container
  at that level, not the leaf inside it.
- **Step 2: Checkpoint.** The checkpoint is the rest point at the start of the
  damaged block's **previous sibling**. Every rule of the grammar that
  writes backwards reaches only the block before:
  - a Setext underline;
  - a definition list's `:` line;
  - a caption before a table;
  - a block identifier line;
  - a pipe table's header line;
  - a lazy continuation line.

  If a candidate's span covers that point, the checkpoint moves back to
  the candidate's opener. The session keeps a small log for this: an
  opening offset and a deciding offset per candidate (E-D3).
- **Step 3: Re-read.**
  - Reopen the ancestor chain at the checkpoint with `reopen`.
  - Truncate the line index, and roll the registry log back to the first
    record at or after the checkpoint. The log is in source order and
    already rolls back to marks.
  - Run the ordinary commit (5.3) over the new text from the checkpoint.

  The edit reuses the append path's line machine unchanged.
- **Step 4: Resync.** Once the commit has passed `start + length`, check three
  things at each new child start `b`, at any depth:
  - the old document has a child at `b - delta` under the same ancestor
    chain;
  - `reopen` of that old child gives the **continuation state** that the
    new reading has at `b`: each ancestor's kind, its continuation
    parameters, and the open state that decides how its later lines are
    read, such as whether its last line was blank. Folded values such as
    `List.tight` are not compared, because the splice recomputes them and
    no later line reads them. Making one early item loose must not stop
    the resync at every later item;
  - no candidate is live in the new reading at `b`. The main reading goes
    on making ordinary children while a candidate is undecided (5.3), so a
    child start alone does not rule one out: deleting `x` from
    `%%x\n\nbody\n\n%%\n` opens a comment candidate that a later line
    accepts, and the `body` paragraph is not the old one. The new reading
    knows its own live candidates, which include any opener the edit
    wrote.

  When all three hold, the new reading from `b` on is the old reading from
  `b - delta` on (12.6). The commit stops there, and every level is
  spliced:
  - the old children after the resync point are kept under each
    ancestor, up to the document;
  - each ancestor is rebuilt as a new value with its id, from its kept
    prefix, its re-read children and its kept suffix;
  - folds, such as `List.tight`, are recomputed from the children without
    parsing.

  The old document has a child at `b - delta` when a binary search over
  each level's children starts finds it, which is O(depth · log children).
  If no point qualifies, the commit goes on, at worst to the end of the
  text. That happens when the edit opens a fence, a comment or an HTML
  block that nothing closes. The language makes that cost, not the design
  (7.2).
- **Step 5: Finish.** As `finalize` does (5.10), but only over what changed:
  - **Inline roots.** Complete the inline roots of the re-read blocks, with
    every definition visible.
  - **Definitions.** If the edit added, removed or changed a definition (a
    `Reference`, a footnote or specimen definition, or a heading, which is
    a label), the winners of those labels may change. The roots that can
    name a changed label are parsed again. The edit path finds them with a
    byte filter over the kept roots' content that is conservative for every
    reference family: a root is parsed again when it holds the opener of
    the changed label's family and the label's first word, compared under
    full case folding. The opener is `[` for a reference, a footnote and a
    heading label, and `@` for a specimen, which is cited as `@label` and
    `(@label)`; a specimen definition changes whether `(@label)` is a
    specimen or a bibliography citation (E-D2). Typing inside a paragraph changes no
    definition and skips this step.
  - **Anchors.** If a heading or an explicit anchor changed, reassign the
    automatic anchors by `main`'s rule. This is O(headings) and parses
    nothing. A heading whose anchor changes keeps its id.
  - **Registry.** Re-index the records after the splice, and add `delta` to
    their `start`. This is O(records after): definitions, not nodes.
  - **Publish.**

### 12.4 Text and memory

- **Text.** No settled node views the session's text buffer. Step E0
  checked it: the buffer is a growing array that moves when an append
  outgrows it (`core/array.h`), so a node that kept a view would already
  break on append. A chunk with `alloc == 0` views a line only while the
  line is read. The edit therefore moves the suffix in place with one
  `memmove`.
- **Nodes.** The dropped nodes and the replaced ancestors go back to the
  node pool's slabs, as a released subtree does today.
- **Compaction.** When old buffers and slab holes exceed the live size, the
  next edit parses the whole text again (E-D4).

### 12.5 Identity

- Kept nodes keep their ids, at every depth.
- Rebuilt ancestors keep their ids, because only their children and folded
  values changed.
- The re-read blocks match the old ones by 5.9's rule. Old offsets after
  `end` are shifted by `delta`, so a paragraph the user types into keeps
  its id.
- Gone nodes retire their ids, and a kind change is a new id, as now.

### 12.6 Why the results are right

- **Before the checkpoint.** The bytes are unchanged. The checkpoint comes
  before any block that a rule writing backwards could reach, and no
  candidate spans it. So the reading up to the checkpoint is that of a
  fresh parse of the new text, and `reopen` gives the state that a fresh
  parse has there. Lookups and anchors are redone in step 5.
- **The re-read blocks.** The same commit reads them as a fresh parse does,
  from the same state.
- **After the resync point.** At `b`, the new reading's state equals the old
  reading's state at `b - delta` in everything a later line reads, and no
  candidate is live in the new reading. The folds differ at most, and the
  splice recomputes them. A child starts there, so no claimed line
  covers it: a candidate that was live there in the old reading was later
  rejected. Both readings then continue as the main reading from equal
  states, with equal bytes left. By 5.11 they give the same blocks,
  including how each ancestor ends.
- **Ancestors.** An ancestor's value is its parameters plus a fold over its
  children. Both are recomputed from the final children.
- **Inline content.** Completion is a function of a root's content and the
  visible definitions. Every definition is visible in this phase, so a kept
  root stays right unless a label it can name changed winner, and step 5
  parses those roots again. Anchors are reassigned whole.

So every field equals `Document.parse` of the new text. This argument is
also the test oracle.

### 12.7 Testing

The differential oracle, extending section 8:

- **Edit oracle.** After every edit, compare against a fresh
  `Document.parse` of the new text: the canonical dump, every scope, the
  definition queries, and the context at every slot cue. This runs in C and
  in each binding, in both units.
- **Inputs.** The corpus and the conformance fixtures, with:
  - every single-byte deletion and insertion at scalar boundaries on small
    documents;
  - generated random replacements on large ones;
  - scripted edits for each hard case:
    - opening and closing a fence, a comment and an HTML block;
    - typing and deleting `>`, list markers and indentation, which changes
      the container chain;
    - adding, removing and renaming a definition or a heading label,
      including a specimen definition cited as `@label` and `(@label)`;
    - an edit that opens a candidate before unchanged children, such as
      `%%` with its closer further down;
    - the text of a heading that has an automatic anchor;
    - splitting and joining paragraphs and list items;
    - a Setext underline, a definition list term, a caption, a block
      identifier line, a pipe table header and delimiter row, and a lazy
      continuation line;
    - the Properties envelope;
    - a loose list turning tight and back, including an early item made
      loose in a long list, whose later items must be spliced and not
      re-read.
- **Damage bound.** The harness counts the blocks each edit re-reads. For
  scripted edits it asserts the exact set: the damaged block and its
  previous sibling. A regression that re-reads a whole container fails.
- **Identity.** Kept nodes keep their ids and values, and rebuilt
  ancestors keep their ids. Every re-read node takes its id by 5.9's
  matching rule, with old offsets after `end` shifted by `delta`. A
  paragraph the user types into therefore keeps its id with a new value.
  Scripted cases assert the exact sets of kept, matched, new and retired
  ids.
- **Faulty-subject self-tests.** The harness must fail against subjects that
  skip:
  - the resync state check;
  - the live-candidate check at the resync point;
  - the specimen forms of the definition filter;
  - the previous-sibling rule;
  - the candidate log;
  - the definition step;
  - the anchor step;
  - the fold recomputation;
  - leaving folds out of the continuation state: a subject that compares
    them is still right, so the damage bound must be the test that fails.
- **Allocator sweep.** An edit that fails at any allocation either leaves the
  session as it was, or reports the failure and frees cleanly.
- **Benchmarks.** Each edit reports its Ir next to a fresh parse of the same
  text. The parse and stream gates stay at 1.02 times the base.

### 12.8 Complexity

Let `k` be the damaged block plus its previous sibling, `d` the depth, and
`c` the children of the ancestors.

| Case | Work |
| --- | --- |
| Typing inside a paragraph, at any depth | O(k + d · log c + c for the splice and folds + text move) |
| An edit that changes a definition | the above + the filtered roots |
| An edit that changes a heading or an anchor | the above + O(headings) |
| An edit that changes a container's syntax | the above, with that container as the damaged block |
| An edit that opens an unclosed fence or comment | up to O(n), as the language requires |

The `c` term copies child pointers and folds flags. It parses nothing. For a
list of 10,000 items, it is about 10,000 pointer copies.

### 12.9 Rollout

- **E0: Gates and model check.**
  - The edit oracle with the damage-bound counter.
  - The edit benchmark, restoring the edit workloads that step 5 removed.
  - The faulty-subject self-tests.
  - The check of which literals view the session text.
  - This section and the revisions of 12.11 land in the plans.
- **E1: C edit.** `markdown_core_session_edit`, `reopen` for every container
  element, nested resync, the candidate log, definitions, anchors and
  compaction.
- **E2: Bindings.** `session.edit` on Swift, Kotlin and ES, in one PR.
- **E3: LSP server.** One session per open document: `didOpen` parses and
  finalizes it, `didChange` edits it, and completion locates it.

### 12.10 Decisions for the owner

- **E-D1 Phases. Decided 2026-10-27.** A session streams until `finalize`
  and takes edits only after it. The owner: "我们显然streaming的时候不能编辑，编辑的时候不能streaming，session的复制本身就是个伪命题".
- **E-D2 Definition changes. Decided 2026-10-27: the byte filter on the edit
  path.** Rejected: recording every lookup on the parse path, which was part
  of what made #432 1.7 times slower.
- **E-D3 Candidate log. Decided 2026-10-27:** two integers per candidate on
  the parse path, gated as noise.
- **E-D4 Compaction. Decided 2026-10-27:** parse the whole text again when
  the garbage exceeds the live size.
- **E-D5 Damage bound. Decided 2026-10-27: re-parse only the damaged block,
  at any depth, in the first version.** The owner: "我希望能够实现将重解析限制在破坏的block内". Rejected: resync at top-level children only, which
  re-reads a whole list for an edit in one item.
