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
Every platform `Markup` also offers `dump()`, which delegates to
`TreeDumper.dump(markup)` and therefore supports focused subtree diagnostics.
Dump text is never used to construct production AST values.

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
`isHeader` preserve the complete public tree semantics without coupling the
generic tree formatter to schema-specific edge names.

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

A directive's label is a CHILD NODE, not a field: an absent label is a
directive with no `DirectiveLabel` child, an empty one is a `DirectiveLabel`
with `children=0`, and a populated one is a `DirectiveLabel` with children. It
was a scalar presence field until Step 7 made it a node.

## Field order by record kind

Fields appear after `scope` and before `children` in exactly this order:

This table is CHECKED against `canonical-ast.json` by
`scripts/audit-ast-projections.mjs`: every kind appears exactly once and its
fields are the contract's, in the contract's order, minus the fields that are
the child structure itself. Until Step 9b nothing read it, and it had drifted
in three ways at once -- a `mode` on four kinds that Q29 deleted at 15A.4, a
`label` on the two directive kinds that stopped being a scalar when Step 7 made
it a node, and no row for `DirectiveLabel` at all.

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
