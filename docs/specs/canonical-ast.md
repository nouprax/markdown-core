# Canonical AST contract

**`docs/specs/canonical-ast.json` is the contract.** This document is its prose
companion: it carries everything a table cannot say — the core rules, the
coordinate model, ownership, the attribute grammar — and its own kind/field
table below is a second copy of the JSON, **checked against it by
`scripts/audit-ast-projections.mjs` kind for kind, field for field, in order**,
so the two cannot drift. Edit the JSON; the audit will tell you if this table
disagrees.

Until Step 15A this table WAS the contract and it lived under
`docs/deprecated/`, which `docs/RECONSTRUCTION.md` says is archive and not
normative, while four executable policy files read it from there.

Status: frozen for Phase 5 on 2026-07-11.

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
- Every `Markup` has a non-optional `scope: Scope`.
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
one whose value is a fact about the source rather than about the kind. The
other five carried one until Step 15A.4 and every one of them was a constant:

| Type | Its one value, now implied by the kind |
| --- | --- |
| `Directive` | `embedded` |
| `DirectiveBlock` | `standalone` |
| `Code` | `embedded` |
| `CodeBlock` | `standalone` |
| `FormulaBlock` | `standalone` — `markdown_core_extensions_set_formula_mode` refuses any other value for this kind |

### Directive attributes

Directive `attributes` is an optional ordered sequence of `DirectiveAttribute`
pairs, each a `name: String` and a `value: String`. It is **sorted by name**:
after class-accumulation and last-value-wins the sequence is a map, and a map
has no source order to keep. `null` means the source wrote no attribute
container; an empty sequence means it wrote `{}`.

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
| `CodeBlock` | `info: String?`, `language: String?`, `literal: String`, `fenced: Bool`, `closed: Bool` | `info` is the complete raw info string; `language` is its first non-whitespace token; indented blocks have `fenced=false, closed=true` |
| `HTMLBlock` | `literal: String` | raw HTML is preserved |
| `FormulaBlock` | `literal: String` | a formula block is always standalone; see the note below |
| `Table` | `alignments: [TableAlignment]`, `header: TableRow`, `rows: [TableRow]` | one alignment per column; header is non-optional |
| `TableRow` | `isHeader: Bool`, `cells: [TableCell]` | `isHeader` is true only for `Table.header` and false for entries in `Table.rows` |
| `TableCell` | `content: [Markup]` | inline content |
| `DirectiveBlock` | `name: String`, `attributes: [DirectiveAttribute]?`, `label: DirectiveLabel?`, `content: [Markup]` | attributes is an ordered sequence of name/value pairs sorted by name; label is a node whose scope spans its brackets; content is block; an absent attribute container and an empty one remain distinct, as do an absent label and an empty one |
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
| `Link` | `destination: String?`, `title: String?`, `content: [Markup]` | absent and empty title remain distinct; inline content |
| `Image` | `source: String?`, `title: String?`, `content: [Markup]` | content is parsed alt-text inline content |
| `LinkReference` | `label: String`, `identifier: String`, `form: ReferenceForm`, `content: [Markup]` | `label` and `identifier` are exactly as on `ReferenceDefinition`; the node carries NO destination — the destination is stated once, at the definition; `form` records which of the three spellings the source used, and all three resolve identically; inline content |
| `ImageReference` | `label: String`, `identifier: String`, `form: ReferenceForm`, `content: [Markup]` | as `LinkReference`; content is parsed alt-text inline content |
| `Directive` | `name: String`, `attributes: [DirectiveAttribute]?`, `label: DirectiveLabel?` | attributes is an ordered sequence of name/value pairs sorted by name; label is a node whose scope spans its brackets; an absent attribute container and an empty one remain distinct, as do an absent label and an empty one |
| `FootnoteReference` | `label: String`, `identifier: String` | `label` is non-empty and as written; `identifier` KEEPS the leading `^`; no form — there is one footnote call syntax; leaf |

Every row above also has the final inherited field `scope: Scope`; it is not
repeated in the table.

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
entry point, and it returns TWO TOTAL VIEWS: `semantic`, the root this table
calls `Document`, and `concrete`, the normalized source with every byte of it in
exactly one region. This table is the semantic view's contract; the concrete
view has no kinds and no fields of its own.

**One kind is spelled differently in the bindings.** `Document` here is the
markup ROOT, and the three value models declare it as `DocumentRoot`, because
the name `Document` belongs to the parse result — the pair — the way
`markdown_core_document` always has in C. The kind name, this table, the dump
string, the wire enums and every `visitDocument` are unchanged. `ParseOptions` is immutable and contains exactly these booleans:

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
| `directives` | `true` |

Disabling an extension disables recognition of its syntax and produces the
same fallback core AST on every platform. `formulas` is the whole gate for
every formula delimiter: `$`, `$$`, `` $`...`$ ``, `\(...\)` and `\[...\]`
are one extension's syntax and turn on together. Scope tracking is mandatory
and is not an option.
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

The standard read-only `Walker` performs depth-first traversal and emits
`entering` then `exiting` events for every reachable `Markup`. Applying an
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

Swift, Kotlin, and TypeScript publish `TreeDumper.dump(markup)` and a
convenience `Markup.dump()` method. Both traverse that platform's immutable
typed tree through its exhaustive Visitor and read-only Walker; they do not
call the C diagnostic dump. Dumping a non-Document Markup treats that value as
the root and emits only its subtree. The canonical text grammar is defined in
`canonical-ast-dump.md` and is diagnostic rather than a serialization API.

## Kotlin `List` naming contract

The concrete AST type remains `com.nouprax.markdown.core.List`. Kotlin source
inside the library spells collection types as `kotlin.collections.List<T>`.
Consumers resolve ambiguity with either the fully qualified AST name or an
import alias such as:

```kotlin
import com.nouprax.markdown.core.List as MarkdownList
```
