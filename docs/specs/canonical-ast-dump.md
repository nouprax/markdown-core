# Canonical AST file-tree dump

[Documentation](README.md) · [AST contract](canonical-ast.md) · [Syntax guide](dialect.md)

The dump is a deterministic public debug representation of the canonical
AST and the reviewed expected representation used by parser tests. It is not
JSON, XML, a renderer, or a serialization/transport API.

The complete reviewed `.ast` golden corpus and its v1 coverage manifest live
only at `specs/canonical-ast/`. C, Swift, Kotlin, and ES conformance targets
enumerate that same non-empty manifest. Swift, Kotlin, and ES each implement
this tree format independently over their public immutable AST; they never
call the native C dump or another binding output. Scopes are computed from the
nodes' extents and the source the document was parsed from, so every dump
takes the document and that source, and a node dump names a node of that
document for focused subtree inspection:

- Swift: `try document.dump(in: source)` and
  `try document.dump(node, in: source)`.
- Kotlin: `document.dump(source)` and `document.dump(node, source)`, or
  `MarkupDumper.dump(document, source)` and
  `MarkupDumper.dump(document, node, source)`.
- ES: `MarkupDumper.dump(document, source)` and
  `MarkupDumper.dump(document, node, source)`.
- C: `markdown_core_document_dump(document, node, source, length, ...)`; pass
  `markdown_core_document_root(document)` to dump the whole document.

Dump text is never used to construct production AST values.

The API is public, but the text remains a human-readable debug contract,
not a persistence or interchange format. Consumers that need structured data
must traverse the typed immutable AST.

## Line grammar

The root line is:

```text
Kind scope=L:C..L:C anchor=null attributes={} <fields> children=N
```

Every descendant line is:

```text
<ancestor-prefix><connector>Kind scope=L:C..L:C anchor=null attributes={} <fields> children=N
```

Connectors and prefixes are exact UTF-8:

- non-final sibling connector: `├── `
- final sibling connector: `└── `
- ancestor with later siblings: `│   `
- ancestor without later siblings: four spaces

Output uses LF line endings and ends with exactly one LF. There is no trailing
whitespace and no color or terminal-dependent output. Tokens on a line are
separated by exactly one space; a kind with no fields prints
`Kind scope=L:C..L:C anchor=null attributes={} children=N`.

`children` counts the node's structural children: `content.count` for every
content-bearing kind, `items.count` for `List`, `cells.count` for `TableRow`,
`head.count + content.count + foot.count` for `Table`, `citations.count` for `Cite`,
`definitions.count` for `DefinitionList`, the number of bodies for `Definition`,
and zero for every leaf and for `Directive`. A directive's optional `label`
is a separate Markup-valued field and is not included in that number, and
neither is `Document.metadata`.

The dump deliberately carries no property or array-index edge labels. Each
node kind's dump function decides which structural children and Markup-valued
fields to emit, in their canonical order. The file-tree connectors visualize
that owned output; they do not redefine every nested record as a child.

## Scalar encoding

- Strings are always quoted and escaped in one canonical JSON form: `"` and
  `\` are escaped, U+0008, U+0009, U+000A, U+000C, and U+000D use their short
  escapes, every other code point below U+0020 uses `\u00XX` with lowercase
  hex, and everything else is raw UTF-8.
- `null`, `true`, and `false` are unquoted lowercase tokens.
- Integers use base-10 ASCII with no leading zero except zero itself.
- Enums use their lowercase contract spelling without quotes.
- Arrays use compact JSON punctuation with no spaces; enum elements inside
  arrays are unquoted, as in `columns=[none:null,left:null]`.
- Universal attributes print as `{.class name="value"}`. Classes lead in
  stored order, then records in stored order; duplicates are retained.
  Empty attributes print `{}`. Record values use JSON string escaping.

Class dump tokens use `.name` for non-empty printable ASCII strings excluding
`"`, `\`, `{`, `}`, `[`, `]`, `(`, `)`, and `=`. Every other class uses `.`
followed by a JSON string, for example `."a}b"` or `."中文"`. This escaping is
only dump syntax; it never changes the stored class or the attribute grammar.

- A tagged value prints its branch and its named fields with no spaces: a
  `Destination` prints as `dest=url("...")`, as
  `dest=cross(path="...",anchor=null)` with `anchor` a string or `null`, or
  as `dest=reference("...")` with the normalized label.
- Every optional and default-bearing field is printed; fields are never
  omitted because they are null, empty, false, or default.
- The inherited fields lead: `scope`, the node's scope computed from its
  `extent` and the source, then `anchor` and `attributes`. `id` is not
  printed.
  Kind-specific scalar fields follow; `children` is last.

The dump is a scope query: it takes the source the document was parsed from
and prints each node's scope in UTF-8 columns, whatever the document's text
unit. The coordinate contract is
[`canonical-ast.md`](canonical-ast.md#coordinates); these coordinates are not
string ranges. A zero-byte document and a document containing one newline
both dump as `1:1..1:0`.

A directive's label is a node-valued FIELD, not a member of directive content.
The directive-specific dump function nests that field before content to
visualize ownership: an absent label emits no `DirectiveLabel`, an empty one
emits `DirectiveLabel children=0`, and a populated one emits the label followed
by its inline children. This visual nesting does not redefine the typed AST,
the `children` count, or the C child traversal contract.

A callout's `title` is an optional inline-node list, never callout content: the callout-specific dump function nests
it before the content as a GROUP line, `Title children=N`, with no
scope and no fields, at the callout's nesting depth, and the title's inline
nodes one level below it. A null title prints no line. `N` is the number of
title nodes, never zero because a present title holds at least one node, and
it is never counted by the callout's own `children`.

A `Definition` prints its inline term in a `DefinitionTerm children=N` group,
then each ordered block body in a `DefinitionBody children=N` group. The groups
have no scope and are not Markup. An empty body still prints its group with
zero children. The definition's `children` counts bodies only, while its
`compact` flag records the authored term gap.

A table prints its columns as compact `flow:relative` values, for example
`columns=[left:0.25,none:null]`. A double uses the shortest decimal that
round-trips, using ordinary decimal notation for values in `[1e-6, 1e21)`
and scientific notation otherwise (lowercase `e`, explicit `+` for a positive
exponent, no exponent zero padding). Its rows always nest under three group lines, `TableHead`,
`TableBody`, then `TableFoot`, including empty groups. The table's `children`
counts their rows, not the group lines. A row prints no scalar fields; each
cell prints `rowspan` then `colspan`, followed by its unchanged content.

A `Dimensions` value prints `(width=W,height=H)` with no internal spaces;
`H` is `null` when only width was authored. `Embedded.dimensions` and `CrossEmbedded.dimensions` print `null`
when absent, so `dimensions=null` and `dimensions=(width=100,height=null)`
remain distinct. A present value always has positive width; height without
width is invalid. The value introduces no node or child line.

## Field order by record kind

Fields appear after `scope` and before `children` in exactly this order:

This table is CHECKED against `canonical-ast.json` by
`scripts/audit/check-ast-projections.mjs`: every kind appears exactly once and its
fields are the contract's, in the contract's order, minus node-valued fields
that the dump represents as nested descendants.

| Kind | Ordered fields between `scope` and `children` |
| --- | --- |
| `Document` | `anchor`, `attributes` |
| `Callout` | `anchor`, `attributes`, `variant`, `collapsed` |
| `Paragraph` | `anchor`, `attributes` |
| `Heading` | `anchor`, `attributes`, `level` |
| `ThematicBreak` | `anchor`, `attributes` |
| `List` | `anchor`, `attributes`, `flavor`, `start`, `variant`, `delimiter`, `tight` |
| `ListItem` | `anchor`, `attributes`, `marker` |
| `CodeBlock` | `anchor`, `attributes`, `info`, `language`, `literal`, `fenced`, `closed` |
| `HTMLBlock` | `anchor`, `attributes`, `literal` |
| `FormulaBlock` | `anchor`, `attributes`, `literal` |
| `Table` | `anchor`, `attributes`, `columns` |
| `TableCaption` | `anchor`, `attributes` |
| `TableRow` | `anchor`, `attributes` |
| `TableCell` | `anchor`, `attributes`, `rowspan`, `colspan` |
| `DirectiveBlock` | `anchor`, `attributes`, `name` |
| `DirectiveLabel` | `anchor`, `attributes` |
| `Text` | `anchor`, `attributes`, `literal` |
| `SoftBreak` | `anchor`, `attributes` |
| `LineBreak` | `anchor`, `attributes` |
| `Code` | `anchor`, `attributes`, `literal` |
| `HTML` | `anchor`, `attributes`, `literal` |
| `CrossLink` | `anchor`, `attributes`, `dest`, `label` |
| `CrossEmbedded` | `anchor`, `attributes`, `dest`, `label`, `dimensions` |
| `Comment` | `anchor`, `attributes`, `literal` |
| `Formula` | `anchor`, `attributes`, `mode`, `literal` |
| `Emphasis` | `anchor`, `attributes` |
| `Strong` | `anchor`, `attributes` |
| `Strikethrough` | `anchor`, `attributes` |
| `Mark` | `anchor`, `attributes` |
| `Insertion` | `anchor`, `attributes` |
| `Span` | `anchor`, `attributes` |
| `Superscript` | `anchor`, `attributes` |
| `Subscript` | `anchor`, `attributes` |
| `Link` | `anchor`, `attributes`, `dest`, `title` |
| `Embedded` | `anchor`, `attributes`, `dest`, `title`, `dimensions` |
| `Directive` | `anchor`, `attributes`, `name` |
| `Cite` | `anchor`, `attributes` |
| `DefinitionList` | `anchor`, `attributes` |
| `Definition` | `anchor`, `attributes`, `compact` |
| `Citation` | `anchor`, `attributes`, `referent` |
| `Footnote` | `anchor`, `attributes`, `label` |
| `Specimen` | `anchor`, `attributes`, `label`, `start` |
| `Metadata` | `anchor`, `attributes`, `name`, `title`, `subtitle`, `time`, `date`, `authors`, `keywords`, `abstract`, `state`, `comment` |
| `Reference` | `anchor`, `attributes`, `label`, `dest`, `title` |

Example:

```text
Document scope=1:1..1:10 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:10 anchor=null attributes={} children=1
    └── Directive scope=1:1..1:10 anchor=null attributes={} name="badge" children=0
        └── DirectiveLabel scope=1:7..1:10 anchor=null attributes={} children=1
            └── Text scope=1:8..1:9 anchor=null attributes={} literal="ok" children=0
```

Any public behavior-bearing field added later must be added to this table, the
manifest coverage vocabulary, affected shared goldens, and all four dump
implementations in the same reviewed change.
`scripts/conformance/generate-canonical-ast-candidates.sh` writes C dump candidates below
`build/canonical-ast-candidates/` for human review; tests never accept them.

## Owned nodes and groups

Every scoped node uses the common node line, including `anchor` and
`attributes`. Named ownership fields determine nesting without becoming
ordinary content. A group line, `Kind children=N`, organizes a node-valued
list and has no scope or common node fields. Groups are not Markup.

- A tagged value prints its branch and named fields with no spaces, as `dest`
  does: `referent=bib(key="...",mode=normal)`,
  `referent=footnote(label="...")`, `referent=footnote(note)` and
  `referent=specimen(label="...")`.
- `Cite` prints one `Citation` node line per item, in source order, with
  `referent` as its one field and a `children` of zero; each item nests the
  `Footnote` of a `footnote(note)` referent, then a `CitationPrefix` group and
  then a `CitationSuffix` group holding the affix nodes, both printed even
  when empty. The cite's own `children` counts the items.
- A `Footnote` prints `label` as its one field and a `children` counting its
  content, which nests one level below it. A definition prints where it was
  written, as content. An inline note has `label=null` and nests its inline
  body directly under the node line; no `Paragraph` is synthesized.

Example, for the source `[^a]` followed by a blank line and `[^a]: note`:

```text
Document scope=1:1..3:10 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
│   └── Cite scope=1:1..1:4 anchor=null attributes={} children=1
│       └── Citation scope=1:2..1:3 anchor=null attributes={} referent=footnote(label="a") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
└── Footnote scope=3:1..3:10 anchor=null attributes={} label="a" children=1
    └── Paragraph scope=3:7..3:10 anchor=null attributes={} children=1
        └── Text scope=3:7..3:10 anchor=null attributes={} literal="note" children=0
```

## Metadata and specimen nodes

A present `Document.metadata` prints one `Metadata` line before document
content. It prints `scope`, `anchor`, `attributes`, then `name`, `title`, `subtitle`, `time`, `date`,
`authors`, `keywords`, `abstract`, `state`, and `comment`, then `children=0`.
A missing field prints `null`. A present field prints `scalar(null)`,
`scalar(bool(true|false))`, `scalar(number("lexeme"))`, `scalar(text("..."))`,
or `list([number("lexeme"),text("...")])`; lists may be empty. Metadata is a leaf node with no nested record lines or separate field scopes.
Absent metadata emits no line.

A specimen definition prints where it was written, as
`Specimen scope=L:C..L:C anchor=null attributes={} label=<string or null> start=<integer or null> children=N`,
followed by its block content. A specimen reference prints a `Cite`
containing a `Citation` with `referent=specimen(label="...")` and empty affix
groups. No derived display number is printed.

## Maintaining dump examples

Exact machine expectations belong in the shared conformance fixtures and the
C package's correctness fixtures. The [syntax wiki](dialect.md) uses Markdown
examples with prose explanations rather than duplicating these full dumps.
A dump change must update its grammar, reviewed expectations, and every binding
in the same change; see [syntax conformance](../architecture/syntax-conformance.md).
