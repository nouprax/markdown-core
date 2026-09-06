# Canonical AST file-tree dump

The dump is a deterministic public debug representation of the canonical
AST and the reviewed expected representation used by parser tests. It is not
JSON, XML, a renderer, or a serialization/transport API.

The complete reviewed `.ast` golden corpus and its v1 coverage manifest live
only at `specs/canonical-ast/`. C, Swift, Kotlin, and ES conformance targets
enumerate that same non-empty manifest. Swift, Kotlin, and ES each export
`TreeDumper` and implement this tree format independently over their public
immutable AST; they never call the native C dump or another binding output.
Every platform `Markup` also offers `dump()`, which delegates to
`TreeDumper.dump(markup)` and therefore supports focused subtree inspection.
Dump text is never used to construct production AST values.

The API is public, but the text remains a human-readable debug contract,
not a persistence or interchange format. Consumers that need structured data
must traverse the typed immutable AST.

## Line grammar

The root line is:

```text
Kind scope=L:C..L:C <fields> children=N
```

Every descendant line is:

```text
<ancestor-prefix><connector>Kind scope=L:C..L:C <fields> children=N
```

Connectors and prefixes are exact UTF-8:

- non-final sibling connector: `├── `
- final sibling connector: `└── `
- ancestor with later siblings: `│   `
- ancestor without later siblings: four spaces

Output uses LF line endings and ends with exactly one LF. There is no trailing
whitespace and no color or terminal-dependent output. Tokens on a line are
separated by exactly one space; a kind with no fields prints
`Kind scope=L:C..L:C children=N`.

`children` counts the node's structural children: `content.count` for every
content-bearing kind, `items.count` for `List`, `cells.count` for `TableRow`,
one for `header` plus `rows.count` for `Table`, and zero for every leaf and
for `Directive`. A directive's optional `label` is a separate Markup-valued
field and is not included in that number.

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
  arrays are unquoted, as in `alignments=[none,left]`.
- Directive attributes are printed as their ordered name/value pairs. Each name
  keeps its first-occurrence source position; values use normal JSON string
  escaping.
- Every optional and default-bearing field is printed; fields are never
  omitted because they are null, empty, false, or default.
- Scope is always printed immediately after the kind. Kind-specific fields
  follow it, and `children` is always last.

The dump prints the native C parser's public scope coordinates exactly, without
normalizing or interpreting particular line/column combinations. The
coordinate contract is [`canonical-ast.md`](canonical-ast.md#coordinates):
one-based lines and one-based, end-inclusive byte columns, with that
contract's one sentinel: an end column of `0` names the boundary before the
first byte of its line, so a block closed by a line ending it consumed ends
at `L:0` and the empty document dumps as `1:1..1:0`. A dumper prints the
sentinel as the parser reports it, and a validator accepts it.

A directive's label is a node-valued FIELD, not a member of directive content.
The directive-specific dump function nests that field before content to
visualize ownership: an absent label emits no `DirectiveLabel`, an empty one
emits `DirectiveLabel children=0`, and a populated one emits the label followed
by its inline children. This visual nesting does not redefine the typed AST,
the `children` count, or the C child traversal contract.

## Field order by record kind

Fields appear after `scope` and before `children` in exactly this order:

This table is CHECKED against `canonical-ast.json` by
`scripts/audit-ast-projections.mjs`: every kind appears exactly once and its
fields are the contract's, in the contract's order, minus node-valued fields
that the dump represents as nested descendants.

| Kind | Ordered fields between `scope` and `children` |
| --- | --- |
| `Document`, `BlockQuote`, `Paragraph`, `ThematicBreak`, `TableCell`, `DirectiveLabel`, `SoftBreak`, `LineBreak`, `Emphasis`, `Strong`, `Strikethrough` | none |
| `Heading` | `level` |
| `List` | `flavor`, `start`, `tight` |
| `ListItem` | `checked` |
| `CodeBlock` | `info`, `language`, `literal`, `fenced`, `closed` |
| `HTMLBlock` | `literal` |
| `FormulaBlock` | `literal` |
| `Table` | `alignments` |
| `TableRow` | `isHeader` |
| `DirectiveBlock` | `name`, `attributes` |
| `FootnoteDefinition` | `label`, `identifier` |
| `ReferenceDefinition` | `label`, `identifier`, `destination`, `title` |
| `Text` | `literal` |
| `Code` | `literal` |
| `HTML` | `literal` |
| `Formula` | `mode`, `literal` |
| `Link` | `destination`, `title` |
| `Image` | `source`, `title` |
| `LinkReference`, `ImageReference` | `label`, `identifier`, `form` |
| `Directive` | `name`, `attributes` |
| `FootnoteReference` | `label`, `identifier` |

Example:

```text
Document scope=1:1..1:10 children=1
└── Paragraph scope=1:1..1:10 children=1
    └── Directive scope=1:1..1:10 name="badge" attributes=null children=0
        └── DirectiveLabel scope=1:7..1:10 children=1
            └── Text scope=1:8..1:9 literal="ok" children=0
```

Any public behavior-bearing field added later must be added to this table, the
manifest coverage vocabulary, affected shared goldens, and all four dump
implementations in the same reviewed change.
`scripts/generate-canonical-ast-candidates.sh` writes C dump candidates below
`build/canonical-ast-candidates/` for human review; tests never accept them.

## Encodings reserved for the target model

The dialect modules add fields and values that the table above does not print
yet, and their examples already use the encodings below. Each lands with its
item, so that the grammar has one answer before the first of them arrives:

- The universal fields print immediately after `scope` on every `Markup`
  line, before the kind-specific fields: `anchor=<string or null>` and
  `attributes={...}`, where the braces hold the classes as `.name` and the
  records as `name="value"` in source order, separated by single spaces, and
  `Attributes.empty` prints as `attributes={}`.
- A tagged value prints its branch and named fields with no spaces:
  `dest=url("...")`, `dest=cross(path="...",anchor=null)`,
  `referent=bib(key="...",mode=normal)`, `referent=footnote(id="...")`,
  `value=scalar(text("..."))`, `value=scalar(null)`,
  `value=scalar(bool(true))`, `value=scalar(number("1.50"))`, and
  `value=list([text("a"),number("1")])`.
- A double prints as the shortest decimal that round-trips, and a table
  column prints as `columns=[left:0.25,none:null]`.
- Besides its structural children, a node prints these nested lines with the
  same connectors, in this order: `Document` prints its `Metadata` value when
  non-null, then the content, then one `Footnote` value per element of
  `footnotes`; `Callout` prints a `CalloutTitle` group when `title` is
  non-null, then the content; `Table` prints its `TableCaption` when non-null,
  then the `TableHead`, `TableBody`, and `TableFoot` groups holding the rows;
  `Definition` prints a `DefinitionTerm` group, then one `DefinitionBody`
  group per body; `Cite` prints one `Citation` value per item, each holding a
  `CitationPrefix` and a `CitationSuffix` group; `Directive` and
  `DirectiveBlock` print the `DirectiveLabel` as today.
- A value line prints `Kind scope=L:C..L:C <fields> children=N` without the
  universal fields: `Citation scope=... referent=... children=0`,
  `Footnote scope=... id="..." children=N`, `Metadata scope=... children=N`,
  and `MetadataRecord scope=... name="..." value=... children=0`. A group
  line prints `Kind children=N` with no scope and no fields.
- `children` keeps counting structural children: `content.count` for every
  content-bearing kind, `items.count` for `List`, `cells.count` for
  `TableRow`, `head.count + content.count + foot.count` for `Table`,
  `definitions.count` for `DefinitionList`, the number of bodies for
  `Definition`, `citations.count` for `Cite`, `records.count` for `Metadata`,
  `content.count` for `Footnote`, and zero for every leaf, for `Directive`,
  and for `Citation`. A group line's own `children` is the number of lines
  nested under it. Nested title, caption, label, metadata, footnote, term,
  prefix, suffix, and row-group lines are never counted by their owner.
- Every scalar and enum keeps the encodings above; nothing is omitted because
  it is null, empty, or default, and an absent optional nested value prints
  no line.

Example, for the source `[^a]` followed by a blank line and `[^a]: note`:

```text
Document scope=1:1..3:10 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
│   └── Cite scope=1:1..1:4 anchor=null attributes={} children=1
│       └── Citation scope=1:2..1:3 referent=footnote(id="a") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
└── Footnote scope=3:1..3:10 id="a" children=1
    └── Paragraph scope=3:7..3:10 anchor=null attributes={} children=1
        └── Text scope=3:7..3:10 anchor=null attributes={} literal="note" children=0
```
