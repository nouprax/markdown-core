# A Markdown language engine on Markdown Core

Status: in progress. Step 1 has landed, and step 2 lands in five parts
(section 9). On 2026-10-24 the owner decided where the engine lives and its
language (D1, D2): a native C engine in this repository, reached in process
through the platform bindings, and made the other decisions of section 10.

The owner's requirements, 2026-10-23 and 2026-10-24:

> We want to use this parser as the source of truth and foundation grammar
> oracle to create a LSP conformed markdown intellisense completion engine.

> The LSP packages goal is to provide highly performance LSP support for
> various platform, our first target is an iOS/macOS native notebook that
> allow we use the markdown as command input for smooth rich editing. The
> product targets Android/Windows and web in the future, all are designed for
> best perf. Thus I do not think TS version is a correct choice.

This document specifies a language engine with Language Server Protocol
semantics whose completions come from Markdown Core: what the parser must
expose so that the engine never decides a question of Markdown syntax
itself, how the engine is built and reached on each platform, which LSP
surface it implements, and the order in which it lands. The word "server"
below means the engine together with whichever host drives it: a native
editor calling it in process, or the JSON-RPC adapter of section 4.2.

Sections 1–2 state requirements and the gap between what an editor asks and
what the parser answers today. Section 3 is the parser's new public model.
Sections 4–6 are the engine and its hosts, its completion catalog and its document model.
Sections 7–9 cover complexity, testing and rollout. Section 10 lists the
owner's decisions, and section 11 records rejected alternatives.

## 1. Requirements

- **R1 One grammar.** Every syntactic judgement the server makes comes from
  Markdown Core: which construct the cursor is in, which part of it (a
  destination, a label, an attribute key), where the token being completed
  starts and ends, and which characters can open a construct. The server
  holds no regular expression, TextMate grammar or hand-written scanner of
  Markdown. A dialect change in the parser changes the server's behavior
  with no change to the server.
- **R2 Completions are verified by the oracle.** Applying any completion the
  server offers, at the position it was offered, yields text that Markdown
  Core parses into the construct the completion names, with the value the
  completion names. Section 8 tests this as a property over the spec
  examples.
- **R3 LSP conformance.** The engine implements the semantics of LSP 3.17:
  the lifecycle, incremental text synchronization, position encoding
  negotiation, and `textDocument/completion` with `completionItem/resolve`
  (section 4.3). Its API takes and returns the protocol's types, so that a
  native editor calling it in process and an editor speaking JSON-RPC to the
  adapter of section 4.2 get the same answers. Every request answers within
  the protocol's error model; nothing a host sends crashes the engine.
- **R4 Positions.** Every position the server takes or returns is in the
  encoding negotiated at `initialize`: UTF-16 code units unless the client
  offers `utf-8`. Markdown Core's `TextUnit` already counts columns in either
  unit (incremental plan, 4.4), so the server converts only between LSP's
  coordinates and the parser's. A parser `Position`, such as the cursor or
  a scope's `start`, is the column of a character counted from 1, and maps
  to LSP `(line - 1, column - 1)`. A `Scope`'s `end` keeps the parser's
  existing convention: the count of columns from its line's start to the
  end of the range, which is already LSP's exclusive end, so it maps to
  `(line - 1, column)`. Under this one rule, `abc` at `1:1..1:3` becomes
  `0:0..0:3`, and an empty scope `L:C..L:C-1` such as an empty `before`
  becomes the empty range `L-1:C-1..L-1:C-1`. A range ending right after a
  line terminator, at `L:0` of the next line, becomes `L-1:0`. The slot
  scopes of section 3 follow the `Scope` contract unchanged, and the
  protocol transcripts (section 8) cover each of these cases.
- **R5 Native performance.** The engine is C, in the same library family as
  the parser, and every platform reaches it without a process boundary,
  JSON or a managed runtime on the request path: Swift on iOS and macOS
  first, then Kotlin on Android and WebAssembly on the web (section 4.1). A completion request answers in time proportional
  to the document's length with the parser's constant: one parse of the
  text before the cursor and, at most once per document version, one parse
  of the whole document (section 6). Section 7 sets the budget.
- **R6 Bindings stay in step.** Every new parser query lands in C and in the
  Swift, Kotlin and ECMAScript bindings in the same step, as every public
  query has so far. The engine's own API follows the platforms' order
  instead (D7): it ships in C and Swift first, then in Kotlin and
  ECMAScript.

Non-goals for this plan: rendering and preview, editing the AST,
a second dialect, and reviving the general-edit engine that D7 of the
[incremental parsing plan](2026-09-29-incremental-parsing.md) removed.

## 2. What an editor asks, and what the parser answers today

Completion happens on text that is not finished. When the user has typed
`See [[Field no` and asks for completion, the document holds no `CrossLink`:
`[[Field no` is `Text`. Today's queries answer about finished constructs only:

- `Document.nodeAt(position, source)` answers the last node in canonical walk
  order whose runs hold the position. On unfinished text it answers `Text`.
- `Document.scope(node, source)` answers a node's runs as editor ranges. It
  has no range for a node's fields: a `Link`'s destination, a heading's
  explicit anchor, an attribute record, a directive's name or a code block's
  info string are values, not nodes, and have no runs.
- `nodeAt` answers one node, not its ancestors; there is no parent link in
  any binding.
- `Document.footnotes`, `.specimens` and `.references` list the document's
  declarations, and headings carry their anchors. These are enough for the
  in-document candidates of section 5.

So a server built on today's API would have to recognize `[[` and decide that
the cursor is in a cross link's target itself, which is a second grammar
(R1). The missing piece is the parser's own view of the unfinished text.

The streaming engine already holds that view. While a session streams, the
open blocks are builders (incremental plan 5.2), and step 8 of that plan
records the provisional inline decisions of the open paragraph: an unmatched
opening delimiter run, an unclosed bracket, a backtick run with no closer, an
autolink, HTML, entity or link destination scanned to the end (5.8). At the
end of the text, that state says exactly which constructs are open and what
each is still reading. This plan's central observation follows:

> **The syntactic context at the cursor is the open state of a parse of the
> text before the cursor.**

The text after the cursor still matters in two ways. It decides how far the
completion's replacement reaches, and it can also decide whether a provisional
opener before the cursor opens anything at all. In `` `[[No‸]]` `` the closing
backtick makes the whole run a code span, which is opaque, while in
`` `[[No‸]] `` the backtick has no closer and the `[[` opens a cross link. The
two prefixes are identical. So the prefix's open state proposes what the
cursor may be typing, and the parse of the whole text confirms it
(section 3.1). Both are the parser's own decisions. Section 3 turns them into
a public value.

## 3. Public model

### 3.1 Context

```swift
public struct Context: Sendable, Equatable {
    public let blocks: [Opener]      // open blocks, outermost first
    public let inlines: [Opener]     // provisional openers, earliest first
    public let slots: [Slot]         // what the cursor may be typing
    public func enter() -> [Edit]    // what Enter writes at the cursor (4.4)
}

public struct Edit: Sendable, Equatable {
    public let range: Scope          // source replaced, empty for an insertion
    public let text: String
}

public struct Slot: Sendable, Equatable {
    public let kind: Kind            // section 3.2
    public let owner: NodeKind       // the construct the slot belongs to
    public let before: Scope         // the field from its start to the cursor
    public let after: Scope          // the field from the cursor to its end
    public let prefix: [String]      // earlier fields the slot depends on
    public let closed: Bool          // the whole document finishes the construct
    public func spell(_ value: String) -> Insertion?   // section 3.2
}

public struct Insertion: Sendable, Equatable {
    public let text: String          // replaces before and after
    public let cursor: Int           // offset in text where the caret lands
}

extension Document {
    /// `self` is the parse of `source`, the whole text of this version.
    public func context(at position: Position, in source: String) throws -> Context
}
```

- **`blocks`** are the session's open builders when its text is the source
  before the cursor: their kinds and the opening facts already decided (a
  fence's character and length, a list item's marker, a callout's type, an
  open properties envelope, a directive fence's colons).
- **`inlines`** are the open paragraph's provisional decisions from step 8,
  each with its kind (`[`, `![`, `[[`, `![[`, `[^`, `[@`, `(@`, `:name`,
  `{`, a backtick run, `$`, `<`, `#`) and its source offset.
- **`slots`** are the fields the innermost open construct may be reading at
  the cursor, or the field of a finished construct whose runs hold the
  cursor. There is more than one only where the left context is ambiguous in
  the grammar itself: `#` at the end of a paragraph may start a topic or a
  block identifier, and the right context decides. The server offers both.
  `before` is the partial token the user has written, so the server can filter
  and replace it; `after` is the same field's continuation after the cursor,
  read by the same scanner in the whole source, so that the
  completion's edit replaces the whole token when the user completes inside
  one. `prefix` carries the fields before the slot that scope its candidates,
  such as `Note` in `[[Note#`.
- The query is a method of the whole document, which the server parses once
  per version (section 6). It runs a session over the source up to the
  cursor's byte and reads its open state. It then confirms each provisional
  opener against the document it is called on, so that no request parses
  the text after the cursor. An opener at offset `o` survives only if the
  whole document reads the source at `o` as one of these: part of a field of
  the slot's kind, on a finished node of the slot's owner kind whose runs
  hold the cursor, or `Text` in an inline root whose runs hold the cursor.
  Ownership is checked with the field's own scope (3.3), not with the
  node's opener. In `` `x`{.co‸de} ``, the `{` is not where the `Code` node
  opens, but it does open that node's attribute field, so the
  `class` slot survives. An opener that the
  whole document absorbs into another node is dropped, and so are its slots.
  That node may be a code span, HTML, a formula, an autolink, a comment, or
  a construct that closes before the cursor. Last, the query scans forward
  in the whole source for `after`. It is one algorithm with the parse: the
  open state and the whole document are the engine's own results, read and
  not recomputed (incremental plan R6).

- **`closed`** is true when the whole document finishes the slot's
  construct after the cursor, as in `[[Fi‸eld notes]]`, and false when the
  construct is still open, as in `See [[Field no‸`.
- **`spell(_:)`** is the parser's spelling of a value in the slot. It returns
  the text that replaces `before` and `after`. That text writes the value in
  the field's own syntax and escapes or brackets what the field cannot hold
  as written, such as a space in a link destination (written `<…>`) or a `|`
  or `]]` in a cross target. When the construct is not `closed`, the text
  also adds the shortest closing syntax that finishes the construct, such as
  `]]`, `)` or `}`. `cursor` places the caret after the value, before any
  closer that a later field could follow, such as the `|label` of a cross
  link. `spell` returns nil for a value the field cannot hold in any
  spelling, and the server drops that candidate. The server never writes
  Markdown syntax itself (R1). R2's round-trip test checks every spelling.

C exposes `markdown_core_document_context(document, source, length, position,
&context)`, `markdown_core_slot_spell(context, slot, value, length, &insertion)`
and `markdown_core_context_free`, with accessors for the arrays.
Kotlin and ECMAScript mirror the Swift shape (R6).

Step 2b built the query this way:

- `before` and `after` are one scope each, not a list of runs. A slot is
  reported only on the cursor's line, so its field before and after the
  cursor is one run of the source.
- The parse of the source before the cursor carries a context sink. The
  block phase lists the blocks open at its end. An inline run whose content
  reaches the end lists the openers its scanners leave open and the fields
  they read when the content ends, each with the scanner's own function for
  where the field goes on past the cursor. That function reads the field
  from its start, so the part before the cursor counts: a topic's segment
  lets a `/` after the cursor go on, and a destination's open `(` lets a
  `)` after it go on. A line of the block phase that
  ends the text reports the same way, for a callout's variant, a fence's
  info string, a directive block's name and an attribute tail. A reference
  definition reports its destination when its paragraph closes.
- The confirmation checks nodes, not field scopes, until step 2d adds them:
  a slot is closed when the document holds a node of its owner kind over
  the field's start and the cursor's last byte, and an `id` slot's node must
  also carry an anchor. Otherwise the opener must be `Text` in an inline
  root that holds the cursor's last byte. A slot that declares a name, such
  as a definition's label, stands only when closed. The inline openers are
  confirmed the same way. A block owner, such as a `CodeBlock` or a
  `Callout`, holds the whole opening line, so its slots read as closed even
  where the attribute block or the variant is still open; step 2d's field
  scopes make that exact.
- A `#` at the end of a paragraph reports both a topic's name and the
  paragraph's block identifier, and the whole document keeps the one it
  reads.

### 3.2 Slot kinds

One slot kind per role of a field of the dialect that holds a name, a key or a path
that an editor can complete. The list is generated from the node-kind
schemas, as `markdown_core_node_kind` is, so a new field with a completable
value appears in the context query by construction.

| Slot kind | Example, cursor at `‸` | Owner |
| --- | --- | --- |
| `dest` | `[text](‸`, `[[‸`, `![[‸` | `Link`, `Embedded`, `CrossLink`, `CrossEmbedded`, `Reference` |
| `anchor` | `[text](#‸`, `[text](file.md#‸`, `[[Note#‸`, `[[#‸` | `Link`, `CrossLink`, `CrossEmbedded` |
| `label` | `[text][‸`, `[‸]:` | `Link`, `Embedded`, `Reference` |
| `footnote` | `[^‸`, `[^‸]:` | `Citation`, `Footnote` |
| `bib` | `[@‸`, `[see @‸`, `@‸` and `-@‸` in text | `Citation` |
| `specimen` | `(@‸`, `(@‸)` opening a specimen | `Citation`, `Specimen` |
| `name` | `:‸`, `:::‸`, `#‸` | `Directive`, `DirectiveBlock`, `Topic` |
| `id` | `{#‸`, `text #‸` at a paragraph's end | any attribute site, any block that takes a block identifier |
| `class` | `{.‸` | any attribute site |
| `record` | `{‸`, `{.a ‸` | any attribute site |
| `variant` | `> [!‸` | `Callout` |
| `info` | ` ```‸ ` | `CodeBlock` |
| `field` | a line start in an open `---` envelope | `Metadata` |
| `value` | after `state: `, after `bib: `, after `src=` | `Metadata`, any attribute site |

A slot kind is one word without the letters `g`, `j` or `y` (the owner's
naming preference, 2026-10-24). Where the canonical AST already names the
field in such a word, the slot kind is that name: `dest`, `anchor`,
`label`, `name`, `id`, `variant` and `info`. The three citation families
take the names of `CitationReferent`'s branches, `footnote`, `bib` and
`specimen`, and an attribute record's name is `record`. A kind that several
owners share, such as `name` or `dest`, means the same role in each, and
`owner` tells the providers which construct it is.

The list is the starting point for review against the dialect spec, not a
closed set: section 9 step 2 audits every field of `docs/specs/canonical-ast.md`.

The audit (step 2a) put the slot kinds in the contract: `slots` in
`docs/specs/canonical-ast.json` numbers them, and each field names the slot
kinds its source writes, from which the generator derives their owners. It
changed the list above in three ways. A slot that declares a name is a slot
too, so a reference definition's, a footnote definition's and a specimen
definition's label are slots, which complete from the labels used and not
declared. A block identifier declares an anchor, so it is an `id`, like
`{#…}`, and `anchor` refers to one. And a citation's slots belong to the
`Citation`, which holds the referent, not to the `Cite`.

### 3.3 Ancestors and field scopes

- **`Document.path(at:in:)`** answers the nodes that hold a position, root
  first, so that the server can tell a link in a table cell from a link in a
  heading without walking the tree itself. `nodeAt` is its last element.
- **`Document.scope(of:field:in:)`** answers a field's source ranges, computed
  on request from the node's runs by the scanner that read the field, as
  `scope(of:in:)` computes a node's (incremental plan D1). Hover, go to
  definition and rename need it; completion uses the slot's `before` and
  `after` instead.

### 3.4 Vocabularies

Where the dialect fixes a set of names, the parser exports it so that the
server does not repeat it: the ten `Metadata` field names, the scalar values
a field accepts where it accepts a fixed set, and the opener characters of
section 3.1, which become the server's completion trigger characters. Open
sets (callout types, code languages, directive names) are not the grammar's
and come from configuration and from the workspace (section 5).

### 3.5 Prerequisite: the settle point

The context query reads the open paragraph's provisional decisions, which
step 8 of the incremental plan introduces. Step 8 therefore lands before
step 2 of this plan. Until it does, an open paragraph's provisional decisions
are not recorded, and the query has no `inlines`.

## 4. The engine, its hosts and its LSP surface

### 4.1 One C engine, reached in process

The engine is a C library, `markdown-core-lsp`, beside the parser in
`packages/markdown-core/lsp` and built by the same CMake project. It
links the parser directly, so the context query, the cached documents and
the providers share one address space and one allocator, and no request
copies a document across a boundary. It owns:

- the **document store**: each open document's text, version and cached
  parse (section 6);
- the **workspace index** of section 5.2;
- the **providers** of section 5, which turn slots into candidates;
- the **LSP types** of the surface below, as C structs whose strings and
  arrays borrow from a result the caller frees.

The engine does no I/O. A host hands it document text, workspace files and
configuration, so that an iOS app inside its sandbox, an Android app with
its storage access framework and a browser tab each decide how files are
read. It has one writer per workspace; the results it returns are immutable
and may be read on any thread. A request names the document version it was
computed for, and the host drops a result whose version is stale, as LSP's
`ContentModified` error does.

Each platform reaches the engine through the binding it already uses for the
parser:

| Platform | Host | Binding |
| --- | --- | --- |
| iOS, macOS (first) | the notebook's native editor | `MarkdownCoreLSP.Server`, a second library product of `swift-markdown-core`, with values of the LSP types |
| Android | the Android editor | Kotlin `Server` in `kotlin-markdown-core`, package `lsp` |
| Web | the web editor | ECMAScript `Server` in `es-markdown-core`, from WebAssembly |

Every binding names the engine's entry type `Server`, in a module or
package named for the C library, as sourcekit-lsp names its own server
module `SourceKitLSP`. The protocol's types live beside it in the same
module, not in a separate `LanguageServerProtocol` module, whose name breaks
the naming rule of 3.2.

On every platform the editor calls the engine as a library, in process. For
the notebook's use of Markdown as a command input, a completion therefore
costs the engine's work and one call into Swift, not a message round trip.

### 4.2 The JSON-RPC adapter

`markdown-core-server` is a small C executable that maps LSP 3.17 JSON-RPC
messages on stdio onto the engine's API and back, for editors that speak the
protocol on the wire, such as VS Code, Neovim and Zed. It holds no state the
engine does not hold, and it is where the protocol transcripts of section 8
run, so it is also the conformance harness for the in-process bindings: a
binding test replays a transcript's requests through the binding and
compares the answers. It needs a JSON reader and writer in C (D8).

### 4.3 The LSP surface

Version 1 implements:

- **Lifecycle.** `initialize`, `initialized`, `shutdown`, `exit`.
  `initialize` negotiates `positionEncoding`: `utf-8` when the client lists
  it, otherwise `utf-16`, mapped to `TextUnit` (R4).
- **Synchronization.** `textDocument/didOpen`, `didChange` with
  `TextDocumentSyncKind.Incremental`, and `didClose`. The server applies each
  change to its copy of the text (section 6).
- **Completion.** `textDocument/completion` with the trigger characters of
  section 3.4, answering a `CompletionList` whose items carry a `textEdit`:
  the slot's `spell` of the candidate, as a snippet whose `$0` is the
  insertion's `cursor` when the client supports snippets. The edit is a
  plain `TextEdit` over the runs of `before` and `after` (below), never an `InsertReplaceEdit`.
  A client may accept an `InsertReplaceEdit` in insert mode, which would
  leave `after` in place and break R2. Covering both means that accepting `getting-started` at `[text](#ge‸tting-started)` gives
  the anchor and not `#getting-startedtting-started`. `completionItem/resolve`
  fills `documentation` lazily: the heading's section, the footnote's text,
  the bibliography entry. Every item sets `filterText` to its own
  candidate's spelled value, without closers, so that a client that keeps
  filtering a cached list as the user types still matches `Field notes`
  after `[[Fi` turns into `[[Fie`.
- **Runs, not spans.** A field's source can be discontiguous, as every
  node's can (incremental plan 4.3): a reference label written over two
  lines of a quote has the `> ` marker between its lines, and one in a grid
  table cell has the cell's border and its neighbours' content there. So
  `before` and `after` are the field's runs, as `Scope` arrays like the
  ones scope queries answer, and an edit touches only those runs, never
  the source between them. The primary edit replaces the cursor's run,
  which is on the cursor's line as LSP requires, with the whole of the
  `Insertion`; `additionalTextEdits` delete the field's other runs. Gaps
  such as quote markers and table borders survive, the ranges do not
  overlap, and applied together they give the parser's `Insertion`. R2's
  round-trip test applies them as one edit, over spec examples that put a
  multi-line field in a quote, a list item and a grid table cell.
- **Workspace.** `workspace/didChangeWatchedFiles` and the workspace folders
  from `initialize`, which feed the workspace index of section 5.2.

Later versions add the rest of the protocol's language features in two
phases, after completion (D6).

- **Navigation**, on the same context, path and field-scope queries:
  `textDocument/hover`, `definition`, `references`, `documentLink`,
  `rename` of labels and anchors, `documentSymbol`, `foldingRange`,
  `semanticTokens` from node runs, and diagnostics for references,
  footnotes, specimens and anchors that name nothing. None needs a parser
  change beyond section 3.
- **Formatting**: `textDocument/onTypeFormatting`, then `formatting` and
  `rangeFormatting` (section 4.4).

### 4.4 Formatting

Formatting writes Markdown, so it needs the grammar's spelling as much as
completion does, and it takes it from the parser for the same reason (R1).

- **On-type formatting** answers Enter with the parser's `Context.enter()`,
  C's `markdown_core_context_enter`. The parser reads the open blocks at the
  cursor and writes the edits: the line break and the prefix that keeps
  every open quote, callout and list item open on the new line, with the
  indentation each block's content needs; a sibling item's marker, with the
  next ordinal for an ordered list and an unchecked box for a task list; or,
  when the innermost item is empty, the edits that remove its marker and
  close it. The parser already decides each of these when it reads a
  continuation line, so writing them is its knowledge, not the engine's
  (R1). `enter()` is a pre-Enter operation: its edits apply to the document
  as it was before the key, and the in-process notebook, which intercepts
  the key, applies them instead of inserting a newline. An LSP client sends
  `textDocument/onTypeFormatting` after it has already inserted the newline
  and perhaps its own indentation, so the engine adapts the same edits to
  the current document: it takes the context at the end of the previous
  line, whose prefix the client's insertion did not change, and answers
  `enter()`'s edits with its line break and prefix replacing the range from
  that line end to the cursor, the client's newline and indentation. Edits
  before the line end, such as removing an empty item's marker, keep their
  ranges. Both paths produce the same document. The query needs no
  writer, so on-type formatting is the first formatting feature, and for the
  notebook's command input it is the one that matters most while typing.
  Its test is the round trip of R2 for structure: for every spec example
  and every line end in it, applying `enter()`'s edits and parsing gives
  the open blocks of the context plus a new sibling item where one was
  written, or the innermost item closed where it was empty. A protocol
  transcript sends `didChange` with the newline, with and without client
  indentation, then `onTypeFormatting`, and checks that the result equals
  applying `enter()` before the key.
- **Document and range formatting** need a Markdown writer: a function in
  the parser library that writes a document, or a range of its blocks, in
  one canonical spelling. The repository has none today. It gets a plan of
  its own before it is built, because it is the grammar written backwards
  and has to hold two invariants for every input: parsing its output gives
  the same semantic tree as parsing its input (no change of meaning), and
  writing its own output changes nothing (idempotence). The semantic tree is
  the canonical AST without source coordinates: no scopes, extents or runs,
  which a canonical spelling changes by design (`#   Title` written as
  `# Title` moves every scope in the heading). The writer plan defines that
  projection; the canonical dump of `docs/specs/canonical-ast-dump.md`
  carries scopes on every node, so it is not the comparison. The engine diffs the
  written text against the document and answers only the changed blocks as
  `TextEdit`s, so a format keeps the editor's undo history and cursor
  usable.

## 5. Completion catalog

A provider answers one slot kind. It reads the context and returns
candidates; it never reads the source text.

### 5.1 From the document

| Slot kind | Candidates |
| --- | --- |
| `anchor` in a `Link` with no file | heading anchors, explicit anchors, block identifiers |
| `anchor` in a `CrossLink` with no note (`[[#`) | headings by text, as `Parent#Child` paths where a heading repeats, then `^` block identifiers |
| `label` | `Document.references` labels, then heading labels (implicit heading references) |
| `footnote` | `Document.footnotes` labels |
| `specimen` | `Document.specimens` labels |
| `id`, `class`, `record` | ids, classes and keys used elsewhere in the document |
| `field`, `value` | the vocabulary of section 3.4, minus keys already present |

### 5.2 From the workspace

The engine keeps a workspace index: for each Markdown file, its path, its
headings and anchors, its block identifiers, its topics, its directive names
and its citation keys, built by parsing the file with `Document.parse` and
refreshed when the host reports a change, as `didChangeWatchedFiles` does.
The host supplies the files (4.1). An open document overlays its index
entry: when a provider asks for the declarations of a file that is open, the
engine reads them from that document's current version, through its lazy
cached parse (section 6), and not from the index. So `[[ThatNote#` offers a
heading renamed in an unsaved editor, and no keystroke updates the index.

A completion request parses at most one document, its own. Providers that
aggregate across the workspace (topics, directive names, citation keys) and
a lookup of another open file read each open document's overlay as of its
last parsed version. An open document that is not the requester and has
unparsed edits is parsed off the request path: when the host calls the
engine's idle entry point (`markdown_core_lsp_idle`), which the notebook calls
when typing pauses and the adapter calls between messages, or when that
document itself next serves a request. Until then its overlay may lag its
newest version by the edits made since the last idle call; the requesting
document's own text is always current.

| Slot kind | Candidates |
| --- | --- |
| `dest` in a `Link` or `Embedded` | workspace files by relative path |
| `anchor` in a `Link` with a file, or a `CrossLink` with a note | that file's anchors and headings |
| `dest` in a `CrossLink` or `CrossEmbedded` | notes by name, and for embeds every file |
| `name` in a `Topic` | topics used in the workspace |
| `name` in a `Directive` or `DirectiveBlock` | configured directive names, then names used in the workspace |
| `bib` | keys of the document's bibliography (5.4), then keys used in the workspace |
| `value` of the `bib` metadata field | workspace bibliography files of 5.4 by relative path |
| `value` of a path attribute (`src` by default, configurable) | workspace files by relative path |

### 5.3 From configuration

`variant` (a default set of callout types, extensible), `info` (a default
set of code languages, extensible), directive names with their
documentation. Configuration arrives through `workspace/configuration`.

### 5.4 The bibliography

A document names its bibliography in its front matter, with a `bib` field
whose value is one relative path or a list of them:

```markdown
---
title: Notes
bib: refs.json
---
```

A rich editor does not render the front matter and changes it only through
its own interface, so the field is metadata, not text the writer types. The
properties grammar recognizes ten names today and drops unknown ones, so
`bib` becomes an eleventh field of `Metadata` (docs/specs/dialect/properties.md),
a parser change that lands in every binding at once. The parser already
decodes the envelope (`elements/properties.c`): a bounded properties
language of scalars and flat lists, not general YAML, so it is the reader
for `bib` and needs no second one. What it lacks is positions: a `Metadata`
node has one scope for the whole envelope and its fields have none
(canonical AST), so the `field` and `value` slots of 3.2 cannot say which
member holds the cursor. Each decoded member gains the scopes of its name
and its value, in step 2. CSL YAML's entries are nested maps, which the
properties language rejects today; under D9 one YAML parser, reworked from
libyaml, reads both. The host supplies the
named file among the workspace files (4.1, 5.2), and the engine reads it in
C, so that no platform parses it on its own. A document without the field
offers only the keys used in the workspace.

The engine reads bibliographies and never edits them: it takes from each
entry its key, and its title, authors and year for the completion item's
detail and for hover. Editing a bibliography file is the job of the tool
that made it, or of another language server. The formats are those common in
academic writing, chosen by the file's extension as Pandoc chooses them:

| Format | Extension | Source | Reader |
| --- | --- | --- | --- |
| CSL JSON | `.json` | Zotero, Pandoc, Mendeley | the JSON reader of D8 |
| CSL YAML | `.yaml`, `.yml` | Pandoc | the YAML parser reworked from libyaml (D9) |
| BibTeX | `.bib`, `.bibtex` | LaTeX, Google Scholar, publishers' exports | a BibTeX reader: entries, `@string` macros, `#` concatenation, braces and LaTeX escapes in values |
| BibLaTeX | `.bib` | BibLaTeX | the BibTeX reader, with BibLaTeX's field names (`date`, `journaltitle`) |
| RIS | `.ris` | EndNote, publishers' exports | a line reader of `TAG  - value` records; an entry's key is its `ID`, and an entry without one offers no key |

Each reader is small, lives in the engine, and is tested against Pandoc's
reading of the same files: the keys must agree, and a malformed entry is
skipped without losing the entries after it. EndNote XML, which Pandoc also
reads, needs an XML reader and stays out of this plan.

## 6. Documents and edits

The incremental plan made sessions append-only (D7), and an editor changes
text anywhere. This plan does not revive general edits. Instead:

- The server holds each open document's text as a string and applies
  `didChange` edits to it. Text editing is the server's work; parsing is the
  parser's.
- A document version is parsed with `Document.parse` at most once, lazily,
  on the first request that needs the whole document, and the result is
  cached by version.
- A completion request runs `context(at:in:)` on the version's cached
  document. Its cost is one session over the text before the cursor, the
  confirmation lookups in the cached document, and the forward scan for
  `after`. A completion request is therefore one of the requests that need
  the whole document, and the first one after a change pays for its parse.
- The workspace index parses a file when it changes on disk or closes in the
  editor, never per keystroke. While a file is open, its current version's
  cached parse answers for it instead (5.2), and is parsed only when a
  request needs it.

Section 7 bounds what this costs. If the budget fails on documents the owner
cares about, the remedy is a measured change to the shared algorithm, not a
second parse path (AGENTS.md).

## 7. Complexity

- A completion request costs O(p) for the parse of the text before the
  cursor, where p is its length. Confirmation is a path lookup in the cached
  document, O(depth + log n) with the runs indexed per version, or O(n)
  with one walk as `nodeAt` does today. On top of that comes O(n) once per
  document version for the whole parse, which the first request after a
  change pays, plus the providers' lookups, which are hash-table reads in
  the index and in the overlays' last parsed versions. A request never
  parses another document (5.2): the overlays' parses, O(total length of
  the open documents with unparsed edits), run in the idle entry point, and
  the latency report measures that call separately.
- **Budget.** Measured in C, the engine's work for a completion request on
  the benchmark grammar corpus stays under 1 ms p95 for documents up to
  16 KiB, the size of a notebook's command input, and under 10 ms for
  documents up to 256 KiB, including the version's whole parse. The figures
  are a proposal for the owner (D5). The measurement lands in the existing
  benchmark workflow in step 4 as a report, in callgrind Ir as the parser's
  gates count, with wall time on an iPhone and a Mac as the device check.
  It becomes a gate in step 10, after every feature has landed (D5).
- Adversarial shapes are the ones the incremental plan's 7.2 lists: a context
  query inside them costs what a session costs over the same prefix.

## 8. Testing

- **Context agreement.** For every spec example and every byte offset in it,
  `Document.context(at:)` equals the open state of a session whose text is the
  example up to that offset, after the confirmation against the whole
  example that section 3.1 defines. This checks that the query reads the
  engine's state and recomputes nothing.
- **Right-context ownership.** Pairs of texts with the same prefix whose
  suffixes decide ownership differently each get their own context. Examples
  are `` `[[No‸]]` `` against `` `[[No‸]] ``, `<a href="[[No‸">` against
  `<a [[No‸`, and `$[[No‸$` against `$[[No‸`. No slot is offered inside an
  opaque node.
- **Oracle round trip (R2).** For every spec example, every offset, and every
  candidate a provider offers there from a fixed fixture workspace, applying
  the item's edit and parsing yields, at the edited range, a node of the
  slot's owner kind whose field holds the candidate. A provider that offers
  a candidate the grammar would not read as intended fails the test. The
  offsets include unfinished constructs such as `See [[Field no‸`, where
  only `spell`'s closer makes the edit parse as a `CrossLink`, and values
  that need escaping, such as destinations with spaces and cross targets
  with `|`.
- **Slot coverage.** Every slot kind of section 3.2 appears in at least one
  spec example's context, and every field the audit of step 2 marks as
  completable has a slot kind. In C, `tests/context/context.txt` holds the
  expected contexts, `scripts/audit/check-context-coverage.mjs` requires an
  example of every slot kind and owner the contract derives, and the context
  runner's sweep asks for the context at every position of every spec
  example in both units.
- **Protocol transcripts.** JSON-RPC request and response transcripts for the
  lifecycle, both position encodings, incremental sync with CRLF, astral
  characters and empty documents, cancellation, and malformed requests, run
  against the adapter of section 4.2.
- **Bindings.** The context, path and field-scope queries run the C tests'
  fixtures in Swift, Kotlin and ECMAScript, as the scope queries do today.
  Each binding of the engine replays the protocol transcripts' requests in
  process and must give the adapter's answers.

## 9. Rollout

Each step is one pull request and leaves `main` releasable. Track W runs
beside steps 1 to 7: it does not depend on the engine, and step 8 depends on
it (D6).

- [ ] **Step 0: This plan.** The owner's decisions of section 10.
- [ ] **Step 1: Settle point.** Step 8 of the incremental parsing plan
   (section 3.5).
- [ ] **Step 2: Context in C.** In five pull requests: 2a the slot audit
   and the generated slot kinds (#448), 2b the context query and the path
   query, 2c `markdown_core_slot_spell`, 2d field scopes and marker ranges,
   and 2e the YAML parser and `Metadata`'s member scopes. The slot audit of `canonical-ast.md`, the
   generated slot kinds, `markdown_core_document_context` and
   `markdown_core_slot_spell`, the context agreement tests and the dialect
   spec's description of slots, and per-member name and value scopes on
   `Metadata` (5.4), on the YAML parser reworked from libyaml (D9). The audit also checks that every marker
   (`**`, `##`, `]]`, list markers, fences) has a source range, from a node's
   scope minus its fields' runs; where one does not, the parser gains a
   marker query, used by semantic tokens and formatting, instead of a CST.
- [ ] **Step 3: Bindings.** Context, path and field scopes in Swift, Kotlin
   and ECMAScript, and the vocabulary exports (3.3, 3.4).
- [ ] **Step 4: Engine skeleton.** `markdown-core-lsp` in C with the
   lifecycle, position encoding, document store and sync, its Swift binding,
   the JSON-RPC adapter and the transcript harness, and the latency report
   (sections 4, 6, 7). Completion answers an empty list.
- [ ] **Step 5: In-document completion.** The providers of 5.1 and the oracle
   round-trip test, in C and Swift.
- [ ] **Step 6: Workspace completion.** The workspace index with
   host-supplied files, the providers of 5.2 and 5.3, and the bibliography
   of 5.4 with the `bib` metadata field in the parser and every binding:
   CSL JSON first, then BibTeX and BibLaTeX, CSL YAML and RIS, one reader
   per change.
- [ ] **Step 7 onward: Navigation.** Hover, definition, references, links,
   rename, symbols, folding, semantic tokens and diagnostics (section 4.3),
   one feature per step, in C and Swift.
- [ ] **Step 8: Formatting.** `markdown_core_context_enter` with its
   round-trip test and on-type formatting on it, then document and range
   formatting on track W's writer (4.4), in C and Swift.
- [ ] **Step 9: Kotlin and ECMAScript.** The engine's Kotlin binding for
   Android and its ECMAScript binding for the web, one binding per step
   (D7).
- [ ] **Step 10: Latency.** With every feature in place, tune the engine's
   latency as a whole against the reports of section 7, then set the
   budget's numbers as a CI gate (D5).
- [ ] **Track W: Markdown writer.** Its own plan, then the writer in the
   parser library and every binding: `markdown_core_document_write`, Swift
   `Document.write()`, with the meaning and idempotence invariants of 4.4.
   It is a parser feature, not an engine one, and needs none of steps 1
   to 7.

## 10. Decisions for the owner

- **D1 Where the engine lives. Decided 2026-10-24: this repository,** as
  the C library `markdown-core-lsp` in `packages/markdown-core/lsp`
  with its bindings in the existing platform packages (4.1). A parser change
  and the engine change that uses it land in one pull request, and the
  engine's tests run against the parser at the same commit.
- **D2 Language. Decided 2026-10-24: C, reached in process on every
  platform.** The owner: "our first target is an iOS/macOS native notebook
  ... The product targets Android/Windows and web in the future, all are
  designed for best perf. Thus I do not think TS version is a correct
  choice." Rejected: a TypeScript server on the WebAssembly binding.
- **D3 Edits. Decided 2026-10-24: whole-document parses per version, no
  general-edit engine** (section 6). This keeps D7 of the incremental plan:
  sessions stay append-only, because Markdown's context-dependent grammar
  and backtracking make random-edit reuse irreconcilable with one-shot and
  streaming performance. The owner: "仓库现在的增量语意是为streaming优化的，因为markdown的highly contextual语法和backtracking问题导致random optimize与one shot perf+streaming perf之间是不可调和的矛盾。" Rejected: a
  new edit API on sessions for the server, a concrete syntax tree, and
  tree-sitter, which would be a second Markdown grammar (R1).
- **D4 Context model. Decided 2026-10-24: the open state of a prefix parse**
  (sections 2 and 3), a streaming append from the start to the cursor, which
  makes step 8 of the incremental plan a prerequisite. It stands in for a
  CST's error recovery: Markdown has no syntax errors, and the parser knows
  exactly which constructs are open at the cursor.
- **D5 Latency budget. Decided 2026-10-24: reported, not gated, until every
  feature has landed;** then step 10 tunes latency across the engine and
  sets the gate (section 7). 10 ms p95 for documents up to 256 KiB stays the
  reported target. The owner: "先报告，等我们实现完所有的需求功能后再反过来统一压延迟最后定一个gate".
- **D6 Scope and order. Decided 2026-10-24: completion, then navigation,
  then formatting, all on C and Swift before the other bindings,** with the
  Markdown writer as track W in parallel (4.3, 4.4, section 9). The owner,
  on the order: "我们需要在Step 7前实现Step 8而Then里的Markdown Generator，因为AST->Markdown是LSP独立的前置依赖它应该在前序step时可以并行处理". The owner: "navigation是LSP的一部分么？format呢？如果是的话可能也得支持，只不过是在completion之后".
  Decided 2026-10-24: the engine reads, and never edits, the bibliography
  formats common in academic writing (5.4). The owner: "需要你把集中学术界常见格式都写进计划，我们针对这些格式都应该是不需要支持编辑，只需要支持理解的对吧". Decided
  2026-10-24: a document names its bibliography in its front matter (5.4).
  The owner: "我觉得这个东西可以靠写进front matter？对于rich editor而言，front matter是一个纯纯的metadata block不会被渲染，也没有编辑接口，只能通过UI重新指定".
- **D7 Platform order. Decided 2026-10-24: C and Swift first, then Kotlin
  and ECMAScript** (R6, step 9). The owner: "我们这个repo里没有任何的Windows
  支持，所以我的建议是先做C和Swift binding，然后做Kotlin及TS的binding". This
  repository ships no Windows binding, so Windows is outside this plan. The
  parser's own new queries keep landing in every binding at once.
- **D8 JSON. Decided 2026-10-24: JSON is parsed in C,** by
  `markdown-core-server` for the protocol and by the engine for a CSL JSON
  bibliography (5.4), so that no platform library parses JSON on its own.
  The owner: "我认为D8这项应该放在C侧进行解析". Proposed: the reader and
  writer are a small one in this repository, which keeps no third-party
  runtime dependency, rather than a vendored C library such as yyjson.

- **D9 One YAML parser. Decided 2026-10-24: take libyaml's scanner and
  parser source into the parser library and rework it,** as this
  repository did with cmark and houdini: the code is merged into
  `elements/` under the repository's naming, allocator and text units, not
  kept as a separate vendored library behind a wrapper. libyaml's MIT
  licence and its upstream (repository and the commit taken) are kept in
  the source and the notices, so upstream fixes can be ported by hand. The
  owner: "有意做窄是因为当时春自己写做起来太多case了，如果引入libyaml然后我们再魔改，我是没意见的", and: "我们是不是应该把代码从libyaml里copy出来，然后结合我们properties对markdown的语意和streaming适配做一个魔改？我们留着libyaml的license和upstream信息，但是代码完全融合进来就和我们懂cmark、hodini等库的操作一样". The rework builds the properties semantics in, rather than around it:
  - **Streaming.** The scanner reads the session's appended input as it
    arrives and resolves the envelope at its closing fence, as
    `markdown_core_properties_closed` does today, with no second pass over
    the source.
  - **Member recovery.** An error ends the current top-level member, not
    the parse: the scanner resynchronizes at the next member and keeps the
    valid neighbours, the rule of `docs/specs/dialect/properties.md`.
  - **Values.** Scalars resolve to null, booleans and exact number
    spellings by that spec. Aliases, anchors and tags make a member invalid.
  - **Scopes.** Each member's name and value carry scopes in the parser's
    text units (5.4).
  - **AST.** `Metadata` keeps its fixed fields and value model; nesting is
    read, for CSL YAML, and not exposed in the canonical AST.
  The decoder of `properties.c` retires; its spec, fixtures and the
  `yaml@2.9.0` oracle stay and must pass unchanged, and CSL YAML entries
  read through the same code. This revises O6 of the Obsidian plan
  (2026-09-02), which ruled out a general YAML parser or dependency. It
  lands in step 2 with the member scopes.

## 11. Rejected alternatives

- **A TypeScript server on the WebAssembly binding.** Rejected by the owner
  on 2026-10-24 (D2): the products are native and performance-first, and a
  Node or browser runtime would sit on every native platform's request path.
- **A language server process per platform, reached over JSON-RPC.** An
  in-process editor such as the notebook would pay a message round trip and
  JSON encoding on every keystroke for nothing; the adapter exists for
  editors that only speak the wire protocol (4.2).
- **An engine per platform in Swift, Kotlin and TypeScript.** Three
  implementations of the providers and the protocol mapping would drift, as
  three grammars would (R1).

- **A Markdown grammar in the server.** Regular expressions, a TextMate
  grammar, or the scanners of an existing Markdown language server. Each is a
  second grammar that drifts from the parser (R1).
- **A tree-sitter grammar for editing, and Markdown Core for the rest.** Two
  grammars that disagree at exactly the unfinished text completion runs on.
- **Probing with speculative closers.** Appending guesses such as `]]` or
  `)` at the cursor and reparsing until a node appears. The guesses are
  grammar knowledge in the server, each request parses several times, and a
  wrong guess yields a wrong context silently.
- **Reading the context from the whole document's tree only.** Unfinished
  constructs are `Text` there (section 2).
- **Field nodes.** Making destinations, labels and attribute records nodes
  with runs, so that `nodeAt` reaches them. It changes the canonical AST and
  every binding's value model for a query that is not on any parse's hot
  path; field scopes are computed on request instead (3.3).
- **Reviving the general-edit engine for keystrokes.** The owner removed it
  for its cost to every fresh parse (incremental plan D7), and a completion
  request needs one prefix parse, not an incremental one.
