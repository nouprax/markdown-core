# Canonical AST file-tree dump

Status: frozen for Phase 5 on 2026-07-11.

The dump is a deterministic public diagnostic representation of the canonical
AST and the reviewed expected representation used by parser tests. It is not
JSON, XML, a renderer, or a serialization/transport API.

The complete reviewed `.ast` golden corpus and its v1 coverage manifest live
only at `specs/canonical-ast/`. C, Swift, Kotlin, and ES conformance targets
enumerate that same non-empty manifest. Swift, Kotlin, and ES each export
`TreeDumper` and implement this tree format independently over their public
immutable AST; they never call the native C dump or another binding output.
Every platform `Document` also offers `dump()` and a focused subtree form
`dump(of: node)`, both delegating to `TreeDumper`; dumping is
document-mediated in v2 because node values carry no positions. A subtree
dump prints scopes with the subtree as origin: the root's start line becomes
line 1, later lines shift by the same amount, columns are line-local and
unchanged, and position-free markers (`0:0..0:0`) print unchanged. Dump text
is never used to construct production AST values.

The API is public, but the text remains a human-readable diagnostic contract,
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
whitespace and no color or terminal-dependent output.

`children` counts direct typed descendants. `TableRow` and `TableCell` are
`Markup` kinds, produce Visitor/Walker callbacks, and own their descendants
through `cells` and `content` respectively.

The dump deliberately carries no property or array-index edge labels. Parent
kind, sibling order, `children`, and behavior-bearing fields such as
`isHeader` and directive `label` preserve the complete public tree semantics
without coupling the generic tree formatter to schema-specific edge names.

## Scalar encoding

- Strings use JSON string escaping and are always quoted.
- `null`, `true`, and `false` are unquoted lowercase tokens.
- Integers use base-10 ASCII with no leading zero except zero itself.
- Enums use their lowercase contract spelling without quotes.
- Arrays use compact JSON punctuation with no spaces.
- Directive attributes are normalized string-map JSON strings produced by the
  parser from source attribute-list syntax. The dump applies normal JSON string
  escaping around that already-normalized value and does not decode it again.
- Every optional and default-bearing field is printed; fields are never
  omitted because they are null, empty, false, or default.
- Scope is always printed immediately after the kind. Kind-specific fields
  follow it, and `children` is always last.

The dump prints the native C parser's public scope coordinates exactly, without
normalizing or interpreting particular line/column combinations.

Directive `label` is a scalar presence field in the dump: `label=null` for no
label, otherwise `label=<count>`, including `label=0` for explicit `[]`.

## Field order by record kind

Fields appear after `scope` and before `children` in exactly this order:

| Kind | Ordered fields between `scope` and `children` |
| --- | --- |
| `Document`, `BlockQuote`, `Paragraph`, `ThematicBreak`, `TableCell`, `SoftBreak`, `LineBreak` | none |
| `Heading` | `level` |
| `List` | `flavor`, `start`, `tight` |
| `ListItem` | `checked` |
| `CodeBlock` | `mode`, `info`, `language`, `literal`, `fenced`, `closed` |
| `HTMLBlock` | `literal` |
| `FormulaBlock` | `mode`, `literal` |
| `Table` | `alignments` |
| `TableRow` | `isHeader` |
| `DirectiveBlock` | `mode`, `name`, `attributes`, `label` |
| `FootnoteDefinition` | `id` |
| `Text` | `literal` |
| `Code` | `mode`, `literal` |
| `HTML` | `literal` |
| `Formula` | `mode`, `literal` |
| `Emphasis`, `Strong`, `Strikethrough` | none |
| `Link` | `destination`, `title` |
| `Image` | `source`, `title` |
| `Directive` | `mode`, `name`, `attributes`, `label` |
| `FootnoteReference` | `id` |

Example:

```text
Document scope=1:1..1:10 children=1
└── Paragraph scope=1:1..1:10 children=1
    └── Directive scope=1:1..1:10 mode=embedded name="badge" attributes=null label=1 children=1
        └── Text scope=1:8..1:9 literal="ok" children=0
```

Any public behavior-bearing field added later must be added to this table, the
manifest coverage vocabulary, affected shared goldens, and all four dump
implementations in the same reviewed change. The maintenance command writes C
dump candidates below `build/` for human review; tests never accept them.
