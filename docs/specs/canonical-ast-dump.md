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
one for `header` plus `rows.count` for `Table`, `citations.count` for `Cite`,
and zero for every leaf and for `Directive`. A directive's optional `label`
is a separate Markup-valued field and is not included in that number, and
neither are `Document.footnotes` and `Document.specimens`.

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
- A tagged value prints its branch and its named fields with no spaces: a
  `Destination` prints as `dest=url("...")`, or as
  `dest=cross(path="...",anchor=null)` with `anchor` a string or `null`.
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

A callout's `title` is likewise a node-valued field, never callout content,
and it is a list rather than a node: the callout-specific dump function nests
it before the content as a GROUP line, `Title children=N`, with no
scope and no fields, at the callout's nesting depth, and the title's inline
nodes one level below it. A null title prints no line. `N` is the number of
title nodes, never zero because a present title holds at least one node, and
it is never counted by the callout's own `children`.

## Field order by record kind

Fields appear after `scope` and before `children` in exactly this order:

This table is CHECKED against `canonical-ast.json` by
`scripts/audit-ast-projections.mjs`: every kind appears exactly once and its
fields are the contract's, in the contract's order, minus node-valued fields
that the dump represents as nested descendants.

| Kind | Ordered fields between `scope` and `children` |
| --- | --- |
| `Document`, `Paragraph`, `ThematicBreak`, `TableCell`, `DirectiveLabel`, `SoftBreak`, `LineBreak`, `Emphasis`, `Strong`, `Strikethrough`, `Cite` | none |
| `Callout` | `variant`, `collapsed` |
| `Heading` | `level` |
| `List` | `flavor`, `start`, `variant`, `delimiter`, `tight` |
| `ListItem` | `marker` |
| `CodeBlock` | `info`, `language`, `literal`, `fenced`, `closed` |
| `HTMLBlock` | `literal` |
| `FormulaBlock` | `literal` |
| `Table` | `alignments` |
| `TableRow` | `isHeader` |
| `DirectiveBlock` | `name`, `attributes` |
| `Text` | `literal` |
| `Code` | `literal` |
| `HTML` | `literal` |
| `Comment` | `literal` |
| `Formula` | `mode`, `literal` |
| `Link` | `dest`, `title` |
| `Image` | `dest`, `title` |
| `Directive` | `name`, `attributes` |

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

## Scoped values and groups

A scoped value is written, so it has a scope, but it is not a `Markup` kind
and never a child: the dump nests it under its owner with the same connectors
as a child line, and it prints as a VALUE line,
`Kind scope=L:C..L:C <fields> children=N`, without the universal fields. A
GROUP line, `Kind children=N`, nests a node-valued list under its owner with
no scope and no fields; its own `children` is the number of lines nested
under it. Nested value and group lines are never counted by their owner.

- A tagged value prints its branch and named fields with no spaces, as `dest`
  does: `referent=bib(key="...",mode=normal)` and
  `referent=footnote(id="...")`.
- `Cite` prints one `Citation` value line per item, in source order, with
  `referent` as its one field and a `children` of zero; each item nests a
  `CitationPrefix` group and then a `CitationSuffix` group holding the affix
  nodes, both printed even when empty. The cite's own `children` counts the
  items.
- `Document` prints its content, then one `Footnote` value line per element
  of `footnotes`, in that order, each with `id` as its one field and a
  `children` counting its content, which nests one level below it. The
  document's own `children` counts the content alone.

Example, for the source `[^a]` followed by a blank line and `[^a]: note`:

```text
Document scope=1:1..3:10 children=1
├── Paragraph scope=1:1..1:4 children=1
│   └── Cite scope=1:1..1:4 children=1
│       └── Citation scope=1:2..1:3 referent=footnote(id="a") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
└── Footnote scope=3:1..3:10 id="a" children=1
    └── Paragraph scope=3:7..3:10 children=1
        └── Text scope=3:7..3:10 literal="note" children=0
```

## Encodings reserved for the target model

The dialect modules add fields and values that the table above does not print
yet, and their examples already use the encodings below. Each lands with its
item, so that the grammar has one answer before the first of them arrives:

- The universal fields print immediately after `scope` on every `Markup`
  line, before the kind-specific fields: `anchor=<string or null>` and
  `attributes={...}`, where the braces hold the classes as `.name` and the
  records as `name="value"` in source order, separated by single spaces, and
  `Attributes.empty` prints as `attributes={}`.
- Further tagged values print as `referent` does: `value=scalar(text("..."))`,
  `value=scalar(null)`, `value=scalar(bool(true))`,
  `value=scalar(number("1.50"))`, and `value=list([text("a"),number("1")])`.
- A double prints as the shortest decimal that round-trips, and a table
  column prints as `columns=[left:0.25,none:null]`.
- Besides its structural children, a node prints these nested lines with the
  same connectors, in this order: `Document` prints its `Metadata` value when
  non-null, then the content, then its footnotes and specimens as today; `Table` prints its
  `TableCaption` when non-null, then the `TableHead`, `TableBody`, and
  `TableFoot` groups holding the rows; `Definition` prints a `DefinitionTerm`
  group, then one `DefinitionBody` group per body.
- Further value lines print as `Citation` and `Footnote` do:
  `Metadata scope=... children=N` and
  `MetadataRecord scope=... name="..." value=... children=0`.
- `children` keeps counting structural children: `head.count + content.count
  + foot.count` for `Table`, `definitions.count` for `DefinitionList`, the
  number of bodies for `Definition`, and `records.count` for `Metadata`;
  nested caption, metadata, term, and row-group lines are never counted by
  their owner.
- Every scalar and enum keeps the encodings above; nothing is omitted because
  it is null, empty, or default, and an absent optional nested value prints
  no line.

A document prints its specimen definitions after its footnotes, each as
`Specimen scope=L:C..L:C id=<string or null> start=<integer or null> children=N`,
followed by its block content. Definitions are scoped values; they never
increase the document's `children` count. A specimen reference uses a `Cite`
with a `Citation` whose referent prints `specimen(id="...")` and whose affix
groups are empty. No resolved display number is printed.
