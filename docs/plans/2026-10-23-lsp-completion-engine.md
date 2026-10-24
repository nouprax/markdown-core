# A Markdown language engine on Markdown Core

Status: proposed. Nothing in this plan is built. On 2026-10-24 the owner
decided where the engine lives and its language (D1, D2): a native C engine
in this repository, reached in process through the platform bindings.
Section 10 lists the remaining decisions, which the owner makes before
rollout step 2.

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

Non-goals for this plan: rendering and preview, formatting, editing the AST,
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
}

public struct Slot: Sendable, Equatable {
    public let kind: Kind            // section 3.2
    public let owner: NodeKind       // the construct the slot belongs to
    public let before: Scope         // slot start ..< cursor
    public let after: Scope          // cursor ..< end of the slot's token
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

### 3.2 Slot kinds

One slot kind per role of a field of the dialect that holds a name, a key or a path
that an editor can complete. The list is generated from the node-kind
schemas, as `markdown_core_node_kind` is, so a new field with a completable
value appears in the context query by construction.

| Slot kind | Example, cursor at `‸` | Owner |
| --- | --- | --- |
| `dest` | `[text](‸`, `[[‸`, `![[‸` | `Link`, `Embedded`, `CrossLink`, `CrossEmbedded`, `Reference` |
| `anchor` | `[text](#‸`, `[text](file.md#‸`, `[[Note#‸`, `[[#‸` | `Link`, `CrossLink`, `CrossEmbedded` |
| `anchor` | `text #‸` at a paragraph's end | `Paragraph`, `ListItem` |
| `label` | `[text][‸` | `Link`, `Embedded` |
| `footnote` | `[^‸` | `Cite` |
| `bib` | `[@‸`, `[see @‸` | `Cite` |
| `specimen` | `(@‸` | `Cite` |
| `name` | `:‸`, `:::‸`, `#‸` | `Directive`, `DirectiveBlock`, `Topic` |
| `id` | `{#‸` | any attribute site |
| `class` | `{.‸` | any attribute site |
| `record` | `{‸`, `{.a ‸` | any attribute site |
| `variant` | `> [!‸` | `Callout` |
| `info` | ` ```‸ ` | `CodeBlock` |
| `field` | a line start in an open `---` envelope | `Metadata` |
| `value` | after `state: ` | `Metadata` |

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
  plain `TextEdit` over `before` and `after`, never an `InsertReplaceEdit`.
  A client may accept an `InsertReplaceEdit` in insert mode, which would
  leave `after` in place and break R2. Covering both means that accepting `getting-started` at `[text](#ge‸tting-started)` gives
  the anchor and not `#getting-startedtting-started`. `completionItem/resolve`
  fills `documentation` lazily: the heading's section, the footnote's text,
  the bibliography entry. Every item sets `filterText` to its own
  candidate's spelled value, without closers, so that a client that keeps
  filtering a cached list as the user types still matches `Field notes`
  after `[[Fi` turns into `[[Fie`.
- **Single-line edits.** LSP requires a completion's edit range to be one
  line, but some fields span line endings, such as a reference label
  `[text][getting\nsta‸rted]`. The primary edit then covers only the part of
  `before` and `after` on the cursor's line, and `additionalTextEdits` delete
  the parts on other lines. The ranges do not overlap, and applied together
  they give the same text as the parser's `Insertion`. R2's round-trip test
  applies them as one edit.
- **Workspace.** `workspace/didChangeWatchedFiles` and the workspace folders
  from `initialize`, which feed the workspace index of section 5.2.

Later versions add, each on the same context, path and field-scope queries:
`textDocument/hover`, `definition`, `references`, `rename` of labels and
anchors, `documentSymbol`, `foldingRange`, `semanticTokens` from node runs,
and diagnostics for references, footnotes, specimens and anchors that name
nothing. None needs a parser change beyond section 3.

## 5. Completion catalog

A provider answers one slot kind. It reads the context and returns
candidates; it never reads the source text.

### 5.1 From the document

| Slot kind | Candidates |
| --- | --- |
| `anchor` in a `Link` with no file | heading anchors, explicit anchors, block identifiers |
| `anchor` in a `CrossLink` with no note (`[[#`) | headings by text, then `^` block identifiers |
| `label` | `Document.references` labels |
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

| Slot kind | Candidates |
| --- | --- |
| `dest` in a `Link` or `Embedded` | workspace files by relative path |
| `anchor` in a `Link` with a file, or a `CrossLink` with a note | that file's anchors and headings |
| `dest` in a `CrossLink` or `CrossEmbedded` | notes by name, and for embeds every file |
| `name` in a `Topic` | topics used in the workspace |
| `name` in a `Directive` or `DirectiveBlock` | configured directive names, then names used in the workspace |
| `bib` | keys of the configured bibliography, then keys used in the workspace |

### 5.3 From configuration

`variant` (a default set of callout types, extensible), `info` (a default
set of code languages, extensible), directive names with their
documentation, and the bibliography file. Configuration arrives through `workspace/configuration`.

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
  the index.
- **Budget.** Measured in C, the engine's work for a completion request on
  the benchmark grammar corpus stays under 1 ms p95 for documents up to
  16 KiB, the size of a notebook's command input, and under 10 ms for
  documents up to 256 KiB, including the version's whole parse. The figures
  are a proposal for the owner (D5). The measurement lands in the existing
  benchmark workflow in step 4 as a report, in callgrind Ir as the parser's
  gates count, with wall time on an iPhone and a Mac as the device check.
  It becomes a gate only by the owner's decision.
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
  completable has a slot kind.
- **Protocol transcripts.** JSON-RPC request and response transcripts for the
  lifecycle, both position encodings, incremental sync with CRLF, astral
  characters and empty documents, cancellation, and malformed requests, run
  against the adapter of section 4.2.
- **Bindings.** The context, path and field-scope queries run the C tests'
  fixtures in Swift, Kotlin and ECMAScript, as the scope queries do today.
  Each binding of the engine replays the protocol transcripts' requests in
  process and must give the adapter's answers.

## 9. Rollout

Each step is one pull request and leaves `main` releasable.

- [ ] **Step 0: This plan.** The owner's decisions of section 10.
- [ ] **Step 1: Settle point.** Step 8 of the incremental parsing plan
   (section 3.5).
- [ ] **Step 2: Context in C.** The slot audit of `canonical-ast.md`, the
   generated slot kinds, `markdown_core_document_context` and `markdown_core_slot_spell`, the context agreement
   tests and the dialect spec's description of slots.
- [ ] **Step 3: Bindings.** Context, path and field scopes in Swift, Kotlin
   and ECMAScript, and the vocabulary exports (3.3, 3.4).
- [ ] **Step 4: Engine skeleton.** `markdown-core-lsp` in C with the
   lifecycle, position encoding, document store and sync, its Swift binding,
   the JSON-RPC adapter and the transcript harness, and the latency report
   (sections 4, 6, 7). Completion answers an empty list.
- [ ] **Step 5: In-document completion.** The providers of 5.1 and the oracle
   round-trip test, in C and Swift.
- [ ] **Step 6: Workspace completion.** The workspace index with
   host-supplied files, and the providers of 5.2 and 5.3.
- [ ] **Step 7: Kotlin and ECMAScript.** The engine's Kotlin binding for
   Android and its ECMAScript binding for the web, one binding per step
   (D7).
- [ ] **Step 8 onward: Navigation.** Hover, definition, references, rename,
   symbols, folding, semantic tokens and diagnostics (section 4), one feature
   per step.

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
- **D3 Edits. Proposed: whole-document parses per version, no general-edit
  engine** (section 6). This keeps D7 of the incremental plan. Rejected: a
  new edit API on sessions for the server.
- **D4 Context model. Proposed: the open state of a prefix parse** (sections
  2 and 3), which makes step 8 of the incremental plan a prerequisite.
- **D5 Latency budget.** 10 ms p95 for documents up to 256 KiB, reported and
  not gated until the owner decides (section 7).
- **D6 Version 1 scope. Proposed: in-document and workspace completion**
  (steps 5 and 6) before any navigation feature, and no bibliography format
  in step 6 beyond CSL JSON.
- **D7 Platform order. Decided 2026-10-24: C and Swift first, then Kotlin
  and ECMAScript** (R6, step 7). The owner: "我们这个repo里没有任何的Windows
  支持，所以我的建议是先做C和Swift binding，然后做Kotlin及TS的binding". This
  repository ships no Windows binding, so Windows is outside this plan. The
  parser's own new queries keep landing in every binding at once.
- **D8 JSON for the adapter. Proposed: a small JSON reader and writer in
  this repository, used only by `markdown-core-server`,** so that the engine and
  the platform libraries carry no JSON code and the repository keeps no
  third-party runtime dependency. The alternative is vendoring an existing C
  JSON library such as yyjson for the adapter alone.

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
