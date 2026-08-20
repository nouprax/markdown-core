# Phase 5 Canonical AST Contract

## Outcome

Phase 5 freezes one language-neutral AST contract for the native C facade and
the Swift, Kotlin, and ES bindings. The inventory contains 28 `Markup` kinds,
including the scoped `TableRow` and `TableCell`. Every kind has a
fixed name, field order, nullability, ownership rule, traversal rule, and at
least one reviewed Markdown case.

The contract is recorded in:

- `docs/specs/canonical-ast.md` for values, nodes, options, and traversal;
- `docs/specs/canonical-ast-dump.md` for canonical tree text;
- `specs/canonical-ast/` for the Phase 18 shared manifest and reviewed
  Markdown/`.ast` pairs.

## One shared tree representation

The `.ast` file-tree format is the only reviewed expected AST representation
and is also published as a diagnostic API. There is no parallel JSON tree
schema.

Phase 18 supersedes the Phase 15 ownership snapshot: the complete reviewed
golden corpus now lives only at root `specs/canonical-ast/`. All four native
conformance targets enumerate its manifest and compare every case byte for
byte. Swift, Kotlin, and ES still produce the grammar independently through
their public Visitor/Walker-based `TreeDumper` implementations and never call
the C dump. Every platform Markup delegates `dump()` to that dumper. Tree text
is not accepted as a production C-to-binding protocol or persistence format.

The frozen format uses `├──`, `└──`, and `│` connectors,
`Kind scope=<range> <ordered-fields> children=<count>` line order, JSON string
escaping, LF line endings, and one final newline.

## Source positions

`Scope` is non-optional and contains start and end `Position` values. Position
line and column integers, including their semantics, are inherited exactly from
the native C parser in the same Markdown Core release. The facade and bindings
copy them without rescanning, normalization, validation, or a separate
interpretation layer.

## Kotlin `List`

The concrete type remains `com.nouprax.markdown.core.List`. A compile-only
Kotlin contract target proves that consumers can use both a fully qualified
name and an import alias while collection properties explicitly use
`kotlin.collections.List<T>`.

## Reviewed fixture coverage

The C-owned fixtures cover all canonical `Markup` kinds across:

- blocks, lists, tasks, fenced code, HTML, and headings;
- embedded and standalone formulas;
- inline containers, links, images, directives, breaks, and footnotes;
- native source-position combinations;
- tables, typed rows/cells, and block directives.

`scripts/check-canonical-ast-fixtures.mjs` rejects missing Markdown/`.ast`
pairs, undeclared or unknown kinds, invalid tree lines,
non-canonical connectors, CRLF, missing final newline, and trailing whitespace.
Goldens are never rewritten automatically.

## Phase 6 implementation inputs

The review identified native details that the read-only facade and dump must
handle without changing the frozen public tree:

- footnote node type strings are not currently sufficient for canonical typed
  dispatch, and a reference id is reached through its definition;
- table alignment accessors return native alignment codes that the facade must
  map to canonical enum values;
- table rows/cells are native and canonical `Markup` nodes owned through typed
  table properties; directive labels remain typed collection edges;
- code block language is derived from the first token of the complete info
  string;
- source-position integers must be copied exactly, including zero values.

These are Phase 6 facade responsibilities, not reasons to add fields or another
test representation.

## Validation

Phase 5 is covered by:

```sh
pnpm run test:contracts
pnpm run verify
git diff --check
```

The contract check reports coverage for 28 `Markup` kinds. The Kotlin fully
qualified/import-alias compile target, formatting,
lint, C tests, Swift build, Gradle model import, and package-content audits are
part of the root verification chain.
