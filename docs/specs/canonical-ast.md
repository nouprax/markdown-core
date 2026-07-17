# Canonical AST contract

Status: frozen for Phase 5 on 2026-07-11; footnote contract revised for v2
milestone M3 on 2026-07-16 (source-order definitions, label-carrying
references, query-based numbering — see the footnote semantics section and
`sessions-and-deltas.md`); identity/equality contract added and scope moved
off node values for v2 milestone M4 on 2026-07-17 (`MarkupSession` becomes a
canonical entry point; the footnote label field is renamed `label` because
`id` now names node identity).

Phase 18 adds the executable repository-level conformance data at
`specs/canonical-ast/manifest.json`. That manifest and its reviewed
Markdown/`.ast` pairs are the sole cross-platform oracle for this contract;
they do not change the production AST or define a serialization format.

This document is the language-neutral public AST contract implemented by the
Swift, Kotlin, and ES bindings. Platform APIs may use idiomatic syntax, but
they must not change names, nullability, ownership, traversal order, defaults,
or semantics.

## Core rules

- `Markup` is the only abstract AST node type.
- Every `Markup` has a non-optional identity `id: MarkupID` and a
  `revision`; equality and hashing are `(id, revision)` — O(1) and
  allocation-free — and equal nodes are guaranteed to have identical AST
  content. See the identity and equality section.
- Nodes do not store absolute source positions. Scopes are resolved through
  the owning snapshot (`document.scope(of:)`), supplied with every `Walker`
  event, and printed by the dump; see `sessions-and-deltas.md` for the
  resolution rules.
- AST values are immutable after construction and own their strings and
  collections. No value retains a C node, document, allocator, or WASM handle.
- Collections are ordered and read-only. Their order is source order unless a
  field below states otherwise.
- `TableRow` and `TableCell` are scoped `Markup` kinds reached through typed
  table properties. Being owned by `header`, `rows`, `cells`, and `content`
  does not make them non-node structural records.
- A directive label is not a synthetic `Markup`. It is the typed `label`
  property of its directive and contains inline `Markup` values.
- The AST contains parsing semantics only. Renderer state, security policy,
  layout, highlighting, generated HTML, and MS-private syntax are excluded.

## Coordinates

```text
Position(line: integer, column: integer)
Scope(start: Position, end: Position)
```

Scopes inherit the native C parser's source-position values and semantics
exactly. The C facade and platform bindings copy `line` and `column` without
rescanning, normalizing, expanding, rejecting, or otherwise reinterpreting
particular coordinate combinations. Consumers that need to interpret a source
position use the native parser contract from the same Markdown Core release.

`TableRow` and `TableCell` resolve scopes like every other `Markup`, so
typed table boundaries do not discard source information.

## Shared value types

### PlacementMode

`PlacementMode` has exactly two values:

- `embedded`: content participates in surrounding inline flow.
- `standalone`: content is presented independently from surrounding inline
  flow.

Placement and AST containment are related but not interchangeable. In
particular, `Formula` may be `standalone` while remaining inside a
paragraph. The invariants for other nodes are:

| Type | Allowed mode |
| --- | --- |
| `Directive` | `embedded` |
| `DirectiveBlock` | `standalone` |
| `Code` | `embedded` |
| `CodeBlock` | `standalone` |
| `Formula` | `embedded` or `standalone` |
| `FormulaBlock` | `standalone` |

### Directive attributes JSON

Directive `attributes` is an optional `String` containing the normalized JSON
representation of a generic directive attribute list. Every member name and
value is a JSON string. Non-string values and nested objects or arrays are
invalid. `null` means no attributes container was present; `"{}"` means an
explicit empty map.

Markdown source uses `{key=value}` attribute-list syntax, not JSON syntax.
Bare attributes and unquoted, single-quoted, or double-quoted values are
supported. HTML-style `#id` and `.class` shortcuts are not supported; `id` and
`class` written as ordinary keys have no special behavior. Values that look
like booleans or numbers remain strings. Every repeated key uses its last
value while retaining its first source position. JSON serialization is
deterministic and is the value passed to consumers for decoding.

Attribute names have no HTML semantics and are never projected to HTML
attributes. For example:

```markdown
:video[My video]{id=123 muted=true title="My Video"}
```

is exposed as `{"id":"123","muted":"true","title":"My Video"}`.

### Other enums

```text
ListFlavor = bullet | ordered
TableAlignment = none | left | center | right
```

## Node inventory

`content` and other collection fields below own their values. `inline content`
means only inline `Markup` kinds are valid; `block content` means only block
kinds are valid. Bindings treat a category violation from the C facade as an
error rather than silently dropping a value.

| Kind | Fields in canonical order | Nullability and invariants |
| --- | --- | --- |
| `Document` | `content: [Markup]` | block content |
| `BlockQuote` | `content: [Markup]` | block content |
| `Paragraph` | `content: [Markup]` | inline content |
| `Heading` | `level: Int`, `content: [Markup]` | `level` is 1 through 6; inline content |
| `ThematicBreak` | none | leaf |
| `List` | `flavor: ListFlavor`, `start: Int?`, `tight: Bool`, `items: [ListItem]` | `start` is non-null only for ordered lists |
| `ListItem` | `checked: Bool?`, `content: [Markup]` | `checked == null` means not a task item; block content |
| `CodeBlock` | `mode`, `info: String?`, `language: String?`, `literal: String`, `fenced: Bool`, `closed: Bool` | mode is `standalone`; `info` is the complete raw info string; `language` is its first non-whitespace token; indented blocks have `fenced=false, closed=true` |
| `HTMLBlock` | `literal: String` | raw HTML is preserved |
| `FormulaBlock` | `mode`, `literal: String` | mode is `standalone` |
| `Table` | `alignments: [TableAlignment]`, `header: TableRow`, `rows: [TableRow]` | one alignment per column; header is non-optional |
| `TableRow` | `isHeader: Bool`, `cells: [TableCell]` | `isHeader` is true only for `Table.header` and false for entries in `Table.rows` |
| `TableCell` | `content: [Markup]` | inline content |
| `DirectiveBlock` | `mode`, `name: String`, `attributes: String?`, `label: [Markup]?`, `content: [Markup]` | attributes is normalized string-map JSON object text; mode is `standalone`; label is inline; content is block; null label and explicit empty label remain distinct |
| `FootnoteDefinition` | `label: String`, `content: [Markup]` | label is written between `[^` and `]`; non-empty; block content; stays at its source position whether referenced or not |
| `Text` | `literal: String` | leaf |
| `SoftBreak` | none | leaf |
| `LineBreak` | none | leaf |
| `Code` | `mode`, `literal: String` | mode is `embedded`; leaf |
| `HTML` | `literal: String` | raw HTML is preserved; leaf |
| `Formula` | `mode`, `literal: String` | either mode; leaf |
| `Emphasis` | `content: [Markup]` | inline content |
| `Strong` | `content: [Markup]` | inline content |
| `Strikethrough` | `content: [Markup]` | inline content |
| `Link` | `destination: String?`, `title: String?`, `content: [Markup]` | absent and empty title remain distinct; inline content |
| `Image` | `source: String?`, `title: String?`, `content: [Markup]` | content is parsed alt-text inline content |
| `Directive` | `mode`, `name: String`, `attributes: String?`, `label: [Markup]?` | attributes is normalized string-map JSON object text; mode is `embedded`; null label and explicit empty label remain distinct |
| `FootnoteReference` | `label: String` | label is written as in source; non-empty; leaf; never degrades to text when unresolved |

Every row above also has the inherited identity fields `id: MarkupID` and
`revision`; they are not repeated in the table. No row has a stored scope.

### Identity and equality

`MarkupID` packs the owning session's random `lineage` salt with the node's
raw 64-bit id: ids are unique within a session, never reused, and stable
across incremental commits while the node remains the same kind of thing at
the same place; nodes from different sessions (one-shot parses included —
`Document.parse` runs an internal single-commit session) never compare
equal. `revision` is the commit revision at which the node's own fields,
child list, or any descendant last changed; a pure positional shift never
changes it. Equality and hashing on every kind are `(id, revision)`,
identifiable-style APIs use `MarkupID` alone, and two equal nodes are
guaranteed to have identical AST content. Absolute source position is not
content. The full identity contract lives in `sessions-and-deltas.md`.

### Footnote semantics (revised 2026-07-16)

The AST is source-faithful. A footnote definition is an ordinary block at its
source position: it is never moved to the document tail, never dropped when
unreferenced, and never reordered by use. A footnote reference is always a
`FootnoteReference` node carrying the label exactly as written between `[^`
and `]`; an unresolved reference stays a reference (it does not degrade to
literal text), and a bracket whose label has no non-whitespace character
never forms a reference. Labels match case-folded with collapsed whitespace,
and the earliest definition of a label in document order wins.

Numbering, first-use order, resolution state, and back-reference ordinals are
not AST content. They are queries over a session-maintained index defined in
`sessions-and-deltas.md`; renderers that need the GFM presentation
(definitions gathered at the tail in first-use order, numbered markers)
derive it from those queries. This aligns the tree with the mdast model and
keeps edits from rewriting unrelated parts of the document.

### Typed table ownership

```text
Table(alignments, header: TableRow, rows: readonly TableRow[])
TableRow(isHeader, cells: readonly TableCell[])
TableCell(content: readonly Markup[])
```

These are all immutable `Markup` values. The typed edges preserve legal table
shape without a generic public `children` property. `isHeader` mirrors and
validates the owning edge: the value in `Table.header` is true and values in
`Table.rows` are false.

## ParseOptions

`Document.parse(source, options = ParseOptions.default)` and
`MarkupSession(options)` (`sessions-and-deltas.md`) are the two canonical
parsing entry points. `ParseOptions` is immutable and contains exactly these
booleans:

| Field | Default |
| --- | --- |
| `smartPunctuation` | `true` |
| `footnotes` | `true` |
| `stripHTMLComments` | `true` |
| `tables` | `true` |
| `strikethrough` | `true` |
| `autolinks` | `true` |
| `taskLists` | `true` |
| `formulas` | `true` |
| `dollarFormulaDelimiters` | `true` |
| `latexFormulaDelimiters` | `true` |
| `directives` | `true` |

Disabling an extension disables recognition of its syntax and produces the
same fallback core AST on every platform. Delimiter options have no effect
when `formulas` is false. Scope tracking is mandatory and is not an option.
Renderer-only `unsafe`, `github-pre-lang`, and `full-info-string` options do
not exist. Raw HTML, URLs, and full code info strings are always retained.

## Visitor and Walker

The typed `Visitor<Result>` has one dispatch method for every `Markup` kind in
the node inventory, including `TableRow` and `TableCell`. A directive label has
no dispatch method because it is a typed collection edge, not synthetic
`Markup`. The interface is exhaustive: every typed method is required, there is
no `defaultVisit`, optional handler, catch-all adapter, or protocol-extension
fallback. Adding a `Markup` kind must therefore produce compile errors in every
visitor until the new case is handled. Visiting one node does not implicitly
recurse.

The standard read-only `Walker` walks a `Document` snapshot (whole or from
a subtree root) depth-first and emits `entering` then `exiting` events for
every reachable `Markup`, each carrying the node's resolved absolute scope. Applying an
exhaustive Visitor on `entering` invokes it exactly once per node. Walker owns
the typed-property rules, so consumers never inspect kinds to discover
structure:

- ordinary containers traverse `content` in index order;
- `List` traverses `items` in order;
- `Table` traverses `header`, then `rows`; each row traverses cells and each
  cell traverses inline content;
- directives traverse `label` first when present, then block `content`;
- `Link` and `Image` traverse their inline `content`.

Rows and cells produce normal visitor callbacks before their descendants.
Visitor and Walker expose no replace, remove, setter, parent mutation, or
native-handle callback.

## Diagnostic dump

Swift, Kotlin, and TypeScript publish `TreeDumper.dump(document)` with a
convenience `document.dump()`, plus a subtree form
`TreeDumper.dump(document, of: node)` / `document.dump(of: node)`. All
traverse that platform's immutable typed tree through its exhaustive Visitor
and read-only Walker; they do not call the C diagnostic dump. Dumping is
document-mediated because scopes are (subtree dumps print scopes with the
subtree as origin — see `canonical-ast-dump.md`). The canonical text grammar
is defined in `canonical-ast-dump.md` and is diagnostic rather than a
serialization API; its frozen `id=` key on footnote nodes prints the
`label` field.

## Kotlin `List` naming contract

The concrete AST type remains `com.nouprax.markdown.core.List`. Kotlin source
inside the library spells collection types as `kotlin.collections.List<T>`.
Consumers resolve ambiguity with either the fully qualified AST name or an
import alias such as:

```kotlin
import com.nouprax.markdown.core.List as MarkdownList
```
