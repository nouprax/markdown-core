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
whitespace and no color or terminal-dependent output.

For historical compatibility, the dump token is named `children`, but it
counts direct node-valued fields in Walker order, not a generic AST child
property. Thus `Table` contributes `header` and `rows`, while a directive
contributes its optional `label` field before its independent `content`.
`TableRow`, `TableCell`, and `DirectiveLabel` are `Markup` kinds and produce
normal Visitor/Walker callbacks.

The dump deliberately carries no property or array-index edge labels. Parent
kind, sibling order, `children`, and behavior-bearing fields such as
`isHeader` preserve the complete public tree semantics without coupling the
generic tree formatter to schema-specific edge names.

## Scalar encoding

- Strings use JSON string escaping and are always quoted.
- `null`, `true`, and `false` are unquoted lowercase tokens.
- Integers use base-10 ASCII with no leading zero except zero itself.
- Enums use their lowercase contract spelling without quotes.
- Arrays use compact JSON punctuation with no spaces.
- Directive attributes are printed as their ordered name/value pairs. Each name
  keeps its first-occurrence source position; values use normal JSON string
  escaping.
- Every optional and default-bearing field is printed; fields are never
  omitted because they are null, empty, false, or default.
- Scope is always printed immediately after the kind. Kind-specific fields
  follow it, and `children` is always last.

The dump prints the native C parser's public scope coordinates exactly, without
normalizing or interpreting particular line/column combinations.

A directive's label is a node-valued FIELD, not a member of directive content.
The dump nests that field to visualize ownership: an absent label emits no
`DirectiveLabel`, an empty one emits `DirectiveLabel children=0`, and a
populated one emits the label followed by its inline descendants. This visual
nesting does not redefine the typed AST or the C child traversal contract.

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
    └── Directive scope=1:1..1:10 name="badge" attributes=null children=1
        └── DirectiveLabel scope=1:7..1:10 children=1
            └── Text scope=1:8..1:9 literal="ok" children=0
```

Any public behavior-bearing field added later must be added to this table, the
manifest coverage vocabulary, affected shared goldens, and all four dump
implementations in the same reviewed change. The maintenance command writes C
dump candidates below `build/` for human review; tests never accept them.
