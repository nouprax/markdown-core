# Canonical AST contract

**`docs/specs/canonical-ast.json` is the contract.** This document is its prose
companion: it carries everything a table cannot say — the core rules, the
coordinate model, ownership, the attribute grammar — and its own kind/field
table below is a second copy of the JSON, **checked against it by
`scripts/audit-ast-projections.mjs` kind for kind, field for field, in order**,
so the two cannot drift. Edit the JSON; the audit will tell you if this table
disagrees.

The executable repository-level conformance data lives at
`specs/canonical-ast/manifest.json`. That manifest and its reviewed
Markdown/`.ast` pairs are the sole cross-platform oracle for this contract;
they do not change the production AST or define a serialization format.

The language the parser accepts is defined by [`dialect.md`](dialect.md) and
its modules; this document is the contract of the AST the implementation
produces today, and its option table is the registry of record for
`ParseOptions`. Where a dialect module describes a kind, field, value, or
option that this document lacks, the module names the landing item that adds
it, and this document stands until that item merges.

This document is the language-neutral public AST contract implemented by the
Swift, Kotlin, and ES bindings. Platform APIs may use idiomatic syntax, but
they must not change names, nullability, ownership, traversal order, defaults,
or semantics.

## Core rules

- `Markup` is the only abstract AST node type.
- Every `Markup` has a non-optional `scope: Scope`.
- AST values are immutable after construction and own their strings and
  collections. No value retains a C node, document, allocator, or WASM handle.
- Collections are ordered and read-only. Their order is source order unless a
  field below states otherwise.
- `TableRow` and `TableCell` are scoped `Markup` kinds reached through typed
  table properties. Being owned by `header`, `rows`, `cells`, and `content`
  does not make them non-node structural records.
- `DirectiveLabel` is `Markup` owned by a directive's typed `label` field. It
  is not an element of the directive's `content` and is not exposed through a
  generic child/content sequence.
- The AST contains parsing semantics only. Renderer state, security policy,
  layout, highlighting, and generated HTML are excluded.
- Adjacent `Text` nodes in one content array are merged into one node spanning
  from the first's start to the last's end, and a `Text` node is never empty.
- Besides `Markup`, exactly the scoped values `Citation`, `Footnote`,
  `Metadata`, and `MetadataRecord` carry a `scope`, because they are written;
  every other value is located by its owner's scope. Those values arrive with
  the landing items that add them.

## Coordinates

```text
Position(line: integer, column: integer)
Scope(start: Position, end: Position)
```

The C facade passes the supplied bytes to the native parser as UTF-8. Valid
UTF-8 is a caller precondition; Markdown Core has no validation or repair mode
for malformed input. Swift, Kotlin, and ECMAScript strings are encoded as UTF-8
before entering that same parse path.

`line` is 1-based and increments once per line ending, whether LF, CR, or
CRLF. `column` is the 1-based byte index within the line; a tab is one byte.
`start` is the first byte of the node's first code point and `end` the last
byte of its last code point, inclusive. A node's scope never includes the line
ending that terminates its last line. `SoftBreak` covers the line-ending bytes
of its break, and `LineBreak` covers the line-ending bytes together with the
backslash or trailing spaces that produced it. A multiline or grid table cell
under the dialect's table options is the one construct whose scope may include
bytes of sibling cells, because its segments are written on shared lines.

Scopes inherit the native C parser's source-position values and semantics
exactly. The C facade and platform bindings copy `line` and `column` without
rescanning, normalizing, expanding, rejecting, or otherwise reinterpreting
particular coordinate combinations. Consumers that need to interpret a source
position use the native parser contract from the same Markdown Core release.

A `Markup.scope` is the source-faithful, contiguous editor cursor range of that
node's own lexical occurrence. It never becomes an expanded or composite range
of every source location that contributed semantic values to the node.
Reference resolution, metadata inheritance, normalization, synthesis, and
other finalization operations may populate fields on an occurrence, but they
must not copy, union, substitute, or otherwise change its scope. In particular,
a resolved reference occurrence does not acquire the separate definition's
range, and a generated value has no fictional source position.

`TableRow` and `TableCell` have non-optional scopes like every other `Markup`,
so typed table boundaries do not discard source information.

## Shared value types

### PlacementMode

`PlacementMode` has exactly two values:

- `embedded`: content participates in surrounding inline flow.
- `standalone`: content is presented independently from surrounding inline
  flow.

Placement and AST containment are related but not interchangeable. In
particular, `Formula` may be `standalone` while remaining inside a paragraph.

**`Formula` is the only kind that carries a `mode`**, because it is the only
one whose value is a fact about the source rather than about the kind. For the
other five kinds the placement is constant and therefore implied by the kind:

| Type | Its one value, now implied by the kind |
| --- | --- |
| `Directive` | `embedded` |
| `DirectiveBlock` | `standalone` |
| `Code` | `embedded` |
| `CodeBlock` | `standalone` |
| `FormulaBlock` | `standalone` — `markdown_core_extensions_set_formula_mode` returns `false` and leaves the node unchanged for any other value |

### Directive attributes

This section describes the directive attribute model as implemented today.
Landing item `M7` replaces it with the universal `anchor` and `attributes`
fields of [`dialect/attributes.md`](dialect/attributes.md), which also states
the one attribute grammar; until then this section stands.

Directive `attributes` is an optional ordered sequence of `DirectiveAttribute`
pairs, each a `name: String` and a `value: String`. It preserves the source
order of each name's first occurrence. A later occurrence updates that same
slot instead of moving it; `class` accumulates there in source order. `null`
means the source wrote no attribute container; an empty sequence means it
wrote `{}`.

Markdown source uses `{key=value}` attribute-list syntax. Bare attributes and
unquoted, single-quoted or double-quoted values are supported. `#name` and
`.name` are shorthand for `id` and `class`. `class` is the one name whose
repeats accumulate, space-separated in source order, whether they were written
as shorthand or as `class=`; every other name keeps its last value. Values that
look like booleans or numbers remain strings.

Attribute names have no HTML semantics and are never projected to HTML
attributes. For example:

```markdown
:video[My video]{id=123 muted=true title="My Video"}
```

is exposed as `id="123"`, `muted="true"`, `title="My Video"`, in that order.

### Other enums

```text
ListFlavor = bullet | ordered
ReferenceForm = full | collapsed | shortcut
TableAlignment = none | left | center | right
```

## Node inventory

`content` and other collection fields below own their values. `inline content`
means only inline `Markup` kinds are valid; `block content` means only block
kinds are valid. A category violation reported by the C facade fails
`Document.parse` on that binding with the platform contract-violation error
and returns no document.

| Kind | Fields in canonical order | Nullability and invariants |
| --- | --- | --- |
| `Document` | `content: [Markup]` | block content |
| `BlockQuote` | `content: [Markup]` | block content |
| `Paragraph` | `content: [Markup]` | inline content |
| `Heading` | `level: Int`, `content: [Markup]` | `level` is 1 through 6; inline content |
| `ThematicBreak` | none | leaf |
| `List` | `flavor: ListFlavor`, `start: Int?`, `tight: Bool`, `items: [ListItem]` | `start` is non-null only for ordered lists |
| `ListItem` | `checked: Bool?`, `content: [Markup]` | `checked == null` means not a task item; block content |
| `CodeBlock` | `info: String?`, `language: String?`, `literal: String`, `fenced: Bool`, `closed: Bool` | `info` is the info string after escape and character-reference processing, stripped of leading and trailing spaces and tabs, and `null` when that is empty or the block is indented; `language` is the prefix of `info` before the first space or tab; `fenced` is true for a fenced block; `closed` is true if and only if a closing fence was found, and always for an indented block |
| `HTMLBlock` | `literal: String` | raw HTML is preserved |
| `FormulaBlock` | `literal: String` | a formula block is always standalone; see the note below |
| `Table` | `alignments: [TableAlignment]`, `header: TableRow`, `rows: [TableRow]` | one alignment per column; header is non-optional; a row shorter than the delimiter row is completed with empty cells scoped at the row's end and a longer row is truncated, so every row has one cell per column |
| `TableRow` | `isHeader: Bool`, `cells: [TableCell]` | `isHeader` is true only for `Table.header` and false for entries in `Table.rows` |
| `TableCell` | `content: [Markup]` | inline content |
| `DirectiveBlock` | `name: String`, `attributes: [DirectiveAttribute]?`, `label: DirectiveLabel?`, `content: [Markup]` | attributes preserves first-occurrence source order with unique names; label is a node-valued field whose scope spans its brackets and is never part of content; content is block; an absent attribute container and an empty one remain distinct, as do an absent label and an empty one |
| `DirectiveLabel` | `content: [Markup]` | inline content; the scope spans the brackets, so an empty label is still a place |
| `FootnoteDefinition` | `label: String`, `identifier: String`, `content: [Markup]` | `label` is non-empty and as written; `identifier` KEEPS the leading `^`, so a footnote and a link definition of one name cannot collide; block content |
| `ReferenceDefinition` | `label: String`, `identifier: String`, `destination: String`, `title: String?` | `label` is the bytes between the brackets as written, delimiters excluded, escapes and character references unresolved, whitespace uncollapsed, case unfolded; `identifier` is the match key — full Unicode case fold, trimmed, internal whitespace collapsed — and is compared with memcmp over its bytes; neither derives the other; `destination` is never absent, because a definition that could not build one is not produced at all; absent and empty title remain distinct; leaf |
| `Text` | `literal: String` | leaf |
| `SoftBreak` | none | leaf |
| `LineBreak` | none | leaf |
| `Code` | `literal: String` | leaf |
| `HTML` | `literal: String` | raw HTML is preserved; leaf |
| `Formula` | `mode`, `literal: String` | either mode; leaf |
| `Emphasis` | `content: [Markup]` | inline content |
| `Strong` | `content: [Markup]` | inline content |
| `Strikethrough` | `content: [Markup]` | inline content |
| `Link` | `destination: String`, `title: String?`, `content: [Markup]` | `destination` is never absent: `[a]()` and `[a](<>)` wrote one and wrote nothing in it, and a link with no destination at all is a `LinkReference`; absent and empty title remain distinct; inline content |
| `Image` | `source: String`, `title: String?`, `content: [Markup]` | `source` is never absent, for the reason `Link.destination` is not; absent and empty title remain distinct; content is parsed alt-text inline content |
| `LinkReference` | `label: String`, `identifier: String`, `form: ReferenceForm`, `content: [Markup]` | `label` and `identifier` are exactly as on `ReferenceDefinition`; the node carries NO destination — the destination is stated once, at the definition; `form` records which of the three spellings the source used, and all three resolve identically; inline content |
| `ImageReference` | `label: String`, `identifier: String`, `form: ReferenceForm`, `content: [Markup]` | as `LinkReference`; content is parsed alt-text inline content |
| `Directive` | `name: String`, `attributes: [DirectiveAttribute]?`, `label: DirectiveLabel?` | attributes preserves first-occurrence source order with unique names; label is a node-valued field whose scope spans its brackets and is never a child/content element; an absent attribute container and an empty one remain distinct, as do an absent label and an empty one |
| `FootnoteReference` | `label: String`, `identifier: String` | `label` is non-empty and as written; `identifier` KEEPS the leading `^`; no form — there is one footnote call syntax; leaf |

Every row above also has the final inherited field `scope: Scope`; it is not
repeated in the table. `Link.destination`, `Image.source`, and every `title`
are the CommonMark-unescaped values with angle-bracket wrappers removed and no
percent-encoding or normalization; an unresolved reference is the inherited
literal text with its brackets.

### Typed table ownership

```text
Table(alignments, header: TableRow, rows: readonly TableRow[], scope)
TableRow(isHeader, cells: readonly TableCell[], scope)
TableCell(content: readonly Markup[], scope)
```

These are all immutable `Markup` values. The typed edges preserve legal table
shape without a generic public `children` property. `isHeader` mirrors and
validates the owning edge: the value in `Table.header` is true and values in
`Table.rows` are false.

## ParseOptions

`Document.parse(source, options = ParseOptions.default)` is the only parsing
entry point. A parse returns exactly the `Document` this table describes. The
document does not retain source text, a normalized source copy, a line index,
tokens, trivia, or recovery records. `ParseOptions` is immutable and today
contains exactly these booleans:

| Field | Default | Status |
| --- | --- | --- |
| `smartPunctuation` | `true` | active |
| `footnotes` | `true` | active |
| `stripHTMLComments` | `true` | active until `M0` removes it |
| `tables` | `true` | active |
| `strikethrough` | `true` | active |
| `autolinks` | `true` | active |
| `taskLists` | `true` | active |
| `formulas` | `true` | active |
| `directives` | `true` | active |

This table is the registry of record. `Status` is `active` for an option the
implementation recognizes; every other option of the dialect is allocated by
name in the feature table of [`dialect.md`](dialect.md), enters this table as
`active` in the item that lands it, and defaults to `false`. Binding, C, CLI,
and fixture spellings follow the dialect index. The effect of each option is
stated by its dialect module: an option that is off leaves the inherited
grammar byte for byte, `smartPunctuation` is defined by
[`dialect/base.md`](dialect/base.md), and `stripHTMLComments` is removed by
`M0` because a comment is a `Comment` node that nothing strips.

Disabling an extension disables recognition of its syntax and produces the
same fallback core AST on every platform. `formulas` is the whole gate for
every formula delimiter: `$`, `$$`, `` $`...`$ ``, `\(...\)` and `\[...\]`
are one extension's syntax and turn on together. Scope tracking is mandatory
and is not an option.
Renderer-only `unsafe`, `github-pre-lang`, and `full-info-string` options do
not exist. Raw HTML, URLs, and code info strings are always retained.

## Visitor and walking

The typed `Visitor<Result>` has one dispatch method for every `Markup` kind in
the node inventory, including `TableRow`, `TableCell`, and `DirectiveLabel`.
The interface is exhaustive: every typed method is required, there is
no `defaultVisit`, optional handler, catch-all adapter, or protocol-extension
fallback. Adding a `Markup` kind must therefore produce compile errors in every
visitor until the new case is handled. Visiting one node does not implicitly
recurse.

The bindings also expose a read-only, depth-first `walk` operation driven by an
exhaustive node-kind-dispatched walking visitor. Every typed callback receives
an `entering` phase before the node's owned markup relations and an `exiting`
phase after them. The walk is implemented with an explicit action stack, so
language call-stack depth does not grow with AST depth.

Walking does not expose an iterator or a generic child projection. Each
node-kind traversal branch selects its own typed, owned relations. Relations
are visited in canonical field order and arrays retain their stored order:
`Table.header` precedes `Table.rows`, while `DirectiveBlock.label` precedes
`DirectiveBlock.content`. A directive label therefore participates in a
complete AST walk as the named `label` field without becoming directive
content or contributing to a `children` collection.

Once `M4` adds them, the scoped values `Citation` and `Footnote` receive value
callbacks and the walk descends into their markup arrays in declared field
order; unscoped values receive no callback and are not descended into.

The walking visitor is exhaustive under the same rule as `Visitor`: every
node-kind callback is required and there is no default, optional handler,
untyped callback, or catch-all adapter. The walk is observation only; it has no
prune, replace, remove, setter, parent mutation, or native-handle callback.

Operations that need relation-specific policy rather than the canonical full
walk continue to implement recursion in their own exhaustive per-node Visitor.

## Debug dump

Swift, Kotlin, and TypeScript publish `TreeDumper.dump(markup)` and a
convenience `Markup.dump()` method. Each TreeDumper uses exhaustive per-node
Visitor dispatch, like cmark's per-node render callback: that node's dump
function emits its fields and decides which content or field nodes to visit.
No binding calls the C debug dump. Dumping a non-Document Markup treats that
value as the root and emits only its operation-defined dump projection. The
canonical text grammar is defined in `canonical-ast-dump.md` and is for
debugging rather than serialization.

## Kotlin `List` naming contract

The Kotlin AST type remains `com.nouprax.markdown.core.List`. Kotlin source
inside the library spells collection types as `kotlin.collections.List<T>`.
Consumers resolve ambiguity with either the fully qualified AST name or an
import alias such as:

```kotlin
import com.nouprax.markdown.core.List as MarkdownList
```
