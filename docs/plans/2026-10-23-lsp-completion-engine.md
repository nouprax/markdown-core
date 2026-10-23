# A Markdown language server on Markdown Core

Status: proposed. Nothing in this plan is built. Section 10 lists the
decisions the owner makes before rollout step 2.

The owner's requirement, 2026-10-23:

> We want to use this parser as the source of truth and foundation grammar
> oracle to create a LSP conformed markdown intellisense completion engine.

This document specifies a Language Server Protocol server whose completions
come from Markdown Core: what the parser must expose so that the server never
decides a question of Markdown syntax itself, which LSP surface the server
implements, where it lives, and the order in which it lands.

Sections 1–2 state requirements and the gap between what an editor asks and
what the parser answers today. Section 3 is the parser's new public model.
Sections 4–6 are the server, its completion catalog and its document model.
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
- **R3 LSP conformance.** The server implements LSP 3.17 over JSON-RPC on
  stdio, and over a message port for browser hosts: the lifecycle,
  incremental text synchronization, position encoding negotiation, and
  `textDocument/completion` with `completionItem/resolve` (section 4). Every
  request answers within the protocol's error model; nothing the client sends
  crashes the server.
- **R4 Positions.** Every position the server takes or returns is in the
  encoding negotiated at `initialize`: UTF-16 code units unless the client
  offers `utf-8`. Markdown Core's `TextUnit` already counts columns in either
  unit (incremental plan, 4.4), so the server converts only between LSP's
  0-based lines and characters and the parser's 1-based lines and columns.
- **R5 Latency.** A completion request answers in time proportional to the
  document's length with the parser's constant, not the server's: one parse
  of the text before the cursor and, at most once per document version, one
  parse of the whole document (section 6). Section 7 sets the budget.
- **R6 Bindings stay in step.** Every new parser query lands in C and in the
  Swift, Kotlin and ECMAScript bindings in the same step, as every public
  query has so far, so that an on-device editor on any platform can use the
  same oracle without the server.

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

Left context decides what is being typed; the text after the cursor decides
only how far the completion's replacement reaches. Section 3 turns the open
state into a public value.

## 3. Public model

### 3.1 Context

```swift
public struct Context: Sendable, Equatable {
    public let blocks: [OpenBlock]   // open blocks, outermost first
    public let inlines: [Opener]     // provisional openers, earliest first
    public let slots: [Slot]         // what the cursor may be typing
}

public struct Slot: Sendable, Equatable {
    public let kind: SlotKind        // section 3.2
    public let owner: NodeKind       // the construct the slot belongs to
    public let typed: Scope          // slot start ..< cursor
    public let rest: Scope           // cursor ..< end of the slot's token
    public let prefix: [String]      // earlier fields the slot depends on
}

extension Document {
    public static func context(at position: Position, in source: String,
                               unit: TextUnit = .utf16) throws -> Context
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
  `typed` is the partial token the user has written, so the server can filter
  and replace it; `rest` is the same field's continuation after the cursor,
  read by the same scanner in the whole source, so that an LSP
  `InsertReplaceEdit` replaces a whole token when the user completes inside
  one. `prefix` carries the fields before the slot that scope its candidates,
  such as `Note` in `[[Note#`.
- The query runs a session over the source up to the cursor's byte, reads its
  open state, and scans forward in the whole source for `rest`. It is one
  algorithm with the parse: the open state is the engine's own state, read and
  not recomputed (incremental plan R6).

C exposes `markdown_core_context_at(source, length, unit, position,
&context)` and `markdown_core_context_free`, with accessors for the arrays.
Kotlin and ECMAScript mirror the Swift shape (R6).

### 3.2 Slot kinds

One slot kind per field of the dialect that holds a name, a key or a path
that an editor can complete. The list is generated from the node-kind
schemas, as `markdown_core_node_kind` is, so a new field with a completable
value appears in the context query by construction.

| Slot kind | Example, cursor at `‸` | Owner |
| --- | --- | --- |
| `linkDestination` | `[text](‸` | `Link`, `Embedded` |
| `linkAnchor` | `[text](#‸`, `[text](file.md#‸` | `Link` |
| `referenceLabel` | `[text][‸` | `Link`, `Embedded` |
| `crossTarget` | `[[‸`, `![[‸` | `CrossLink`, `CrossEmbedded` |
| `crossAnchor` | `[[Note#‸`, `[[#‸` | `CrossLink`, `CrossEmbedded` |
| `footnoteLabel` | `[^‸` | `Citation` |
| `citationKey` | `[@‸`, `[see @‸` | `Citation` |
| `specimenLabel` | `(@‸` | `Cite` |
| `directiveName` | `:‸`, `:::‸` | `Directive`, `DirectiveBlock` |
| `attributeId` | `{#‸` | any attribute site |
| `attributeClass` | `{.‸` | any attribute site |
| `attributeKey` | `{‸`, `{.a ‸` | any attribute site |
| `calloutType` | `> [!‸` | `Callout` |
| `codeLanguage` | ` ```‸ ` | `CodeBlock` |
| `topicName` | `#‸` | `Topic` |
| `blockIdentifier` | `text #‸` at a paragraph's end | `Paragraph`, `ListItem` |
| `metadataKey` | a line start in an open `---` envelope | `Metadata` |
| `metadataValue` | after `state: ` | `Metadata` |

The list is the starting point for review against the dialect spec, not a
closed set: section 9 step 2 audits every field of `docs/specs/canonical-ast.md`.

### 3.3 Ancestors and field scopes

- **`Document.path(at:in:)`** answers the nodes that hold a position, root
  first, so that the server can tell a link in a table cell from a link in a
  heading without walking the tree itself. `nodeAt` is its last element.
- **`Document.scope(of:field:in:)`** answers a field's source ranges, computed
  on request from the node's runs by the scanner that read the field, as
  `scope(of:in:)` computes a node's (incremental plan D1). Hover, go to
  definition and rename need it; completion uses the slot's `typed` and
  `rest` instead.

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

## 4. The LSP surface

Version 1 implements:

- **Lifecycle.** `initialize`, `initialized`, `shutdown`, `exit`.
  `initialize` negotiates `positionEncoding`: `utf-8` when the client lists
  it, otherwise `utf-16`, mapped to `TextUnit` (R4).
- **Synchronization.** `textDocument/didOpen`, `didChange` with
  `TextDocumentSyncKind.Incremental`, and `didClose`. The server applies each
  change to its copy of the text (section 6).
- **Completion.** `textDocument/completion` with the trigger characters of
  section 3.4, answering a `CompletionList` whose items carry a `textEdit`:
  an `InsertReplaceEdit` from the slot's `typed` and `rest` when the client
  supports it, a `TextEdit` over `typed` otherwise. `completionItem/resolve`
  fills `documentation` lazily: the heading's section, the footnote's text,
  the bibliography entry. Every item sets `filterText` to the slot's token so
  that the client's own filtering stays consistent with `typed`.
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
| `linkAnchor` (no file) | heading anchors, explicit anchors, block identifiers |
| `crossAnchor` (`[[#`) | headings by text, then `^` block identifiers |
| `referenceLabel` | `Document.references` labels |
| `footnoteLabel` | `Document.footnotes` labels |
| `specimenLabel` | `Document.specimens` labels |
| `attributeId`, `attributeClass`, `attributeKey` | ids, classes and keys used elsewhere in the document |
| `metadataKey`, `metadataValue` | the vocabulary of section 3.4, minus keys already present |

### 5.2 From the workspace

The server keeps a workspace index: for each Markdown file, its path, its
headings and anchors, its block identifiers, its topics, its directive names
and its citation keys, built by parsing the file with `Document.parse` and
refreshed on `didChangeWatchedFiles`.

| Slot kind | Candidates |
| --- | --- |
| `linkDestination` | workspace files by relative path |
| `linkAnchor` (with file), `crossAnchor` (with note) | that file's anchors and headings |
| `crossTarget` | notes by name, and for embeds every file |
| `topicName` | topics used in the workspace |
| `directiveName` | configured directive names, then names used in the workspace |
| `citationKey` | keys of the configured bibliography, then keys used in the workspace |

### 5.3 From configuration

`calloutType` (a default set, extensible), `codeLanguage` (a default set,
extensible), directive names with their documentation, and the bibliography
file. Configuration arrives through `workspace/configuration`.

## 6. Documents and edits

The incremental plan made sessions append-only (D7), and an editor changes
text anywhere. This plan does not revive general edits. Instead:

- The server holds each open document's text as a string and applies
  `didChange` edits to it. Text editing is the server's work; parsing is the
  parser's.
- A document version is parsed with `Document.parse` at most once, lazily,
  on the first request that needs the whole document, and the result is
  cached by version.
- A completion request runs `Document.context(at:in:)`, whose cost is one
  session over the text before the cursor plus the forward scan for `rest`.
- The workspace index parses a file when it changes on disk or closes in the
  editor, never per keystroke.

Section 7 bounds what this costs. If the budget fails on documents the owner
cares about, the remedy is a measured change to the shared algorithm, not a
second parse path (AGENTS.md).

## 7. Complexity

- A completion request costs O(p) for the parse of the text before the
  cursor, p its length, plus O(n) once per document version for the whole
  parse when a provider needs declarations, plus the providers' lookups,
  which are hash-table reads in the index.
- **Budget.** The p95 latency of a completion request on the benchmark
  grammar corpus, measured in the ECMAScript binding on the gates' machine,
  stays under 10 ms for documents up to 256 KiB. The figure is a proposal for
  the owner (D5); the measurement lands in the existing benchmark workflow in
  step 4 as a report, and becomes a gate only by the owner's decision.
- Adversarial shapes are the ones the incremental plan's 7.2 lists: a context
  query inside them costs what a session costs over the same prefix.

## 8. Testing

- **Context agreement.** For every spec example and every byte offset in it,
  `Document.context(at:)` equals the open state of a session whose text is the
  example up to that offset. This checks that the query reads the engine's
  state and recomputes nothing.
- **Oracle round trip (R2).** For every spec example, every offset, and every
  candidate a provider offers there from a fixed fixture workspace, applying
  the item's edit and parsing yields, at the edited range, a node of the
  slot's owner kind whose field holds the candidate. A provider that offers
  a candidate the grammar would not read as intended fails the test.
- **Slot coverage.** Every slot kind of section 3.2 appears in at least one
  spec example's context, and every field the audit of step 2 marks as
  completable has a slot kind.
- **Protocol transcripts.** JSON-RPC request and response transcripts for the
  lifecycle, both position encodings, incremental sync with CRLF, astral
  characters and empty documents, cancellation, and malformed requests. The
  same transcripts run against the stdio and browser transports.
- **Bindings.** The context, path and field-scope queries run the C tests'
  fixtures in Swift, Kotlin and ECMAScript, as the scope queries do today.

## 9. Rollout

Each step is one pull request and leaves `main` releasable.

- [ ] **Step 0: This plan.** The owner's decisions of section 10.
- [ ] **Step 1: Settle point.** Step 8 of the incremental parsing plan
   (section 3.5).
- [ ] **Step 2: Context in C.** The slot audit of `canonical-ast.md`, the
   generated slot kinds, `markdown_core_context_at`, the context agreement
   tests and the dialect spec's description of slots.
- [ ] **Step 3: Bindings.** Context, path and field scopes in Swift, Kotlin
   and ECMAScript, and the vocabulary exports (3.3, 3.4).
- [ ] **Step 4: Server skeleton.** The server package, lifecycle, position
   encoding, document store and sync, the transcript harness, both
   transports, and the latency report (sections 4, 6, 7). Completion answers
   an empty list.
- [ ] **Step 5: In-document completion.** The providers of 5.1 and the oracle
   round-trip test.
- [ ] **Step 6: Workspace completion.** The workspace index and the
   providers of 5.2 and 5.3.
- [ ] **Step 7: Clients.** A VS Code extension and a web-worker build for
   Monaco and CodeMirror hosts.
- [ ] **Step 8 onward: Navigation.** Hover, definition, references, rename,
   symbols, folding, semantic tokens and diagnostics (section 4), one feature
   per step.

## 10. Decisions for the owner

- **D1 Where the server lives. Proposed: a package in this repository,**
  `packages/markdown-language-server`, in the pnpm workspace beside
  `es-markdown-core`. A parser change and the server change that uses it then
  land in one pull request, and the server tests run against the parser at
  the same commit, which is what "source of truth" asks. The alternative is a
  new repository that depends on a published `@nouprax/es-markdown-core`;
  it keeps this repository a parser, at the cost of a release between every
  parser change and the server change that uses it. A new repository has to
  be created on GitHub by the owner.
- **D2 Language. Proposed: TypeScript on the WebAssembly binding.** The
  ECMAScript binding already runs the C parser in Node and in browsers, and
  already counts UTF-16 columns by default. `vscode-languageserver` is the
  protocol's reference implementation, and one build serves VS Code, Node
  hosts and browser editors. The alternatives are a native server in C, which
  would have to write its own JSON-RPC and protocol types, or a Swift or
  Kotlin server, which would serve one platform's editors.
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

## 11. Rejected alternatives

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
