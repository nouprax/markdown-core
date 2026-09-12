# Markdown Core documentation

Start with the [Markdown syntax guide](dialect.md) to write documents and
understand how they parse. Every syntax page includes copyable Markdown,
a description of the result, and the rules that affect ambiguous or incomplete
input. All syntax is enabled on every platform.

## For authors

- [Syntax guide](dialect.md): the complete language, organized by what you want to write.
- [Basics](dialect/base.md): paragraphs, indentation, escaping, and where to start.
- [Compatibility and precedence](dialect/conflicts.md): differences from other Markdown dialects and how overlapping syntax is interpreted.

## For application developers

- [Canonical AST contract](canonical-ast.md): node fields, ownership, source coordinates, and traversal.
- [Machine-readable AST contract](canonical-ast.json): the kind and field inventory used by repository audits.
- [Debug dump format](canonical-ast-dump.md): how to read diagnostic tree output.
- [Package usage](../../README.md#usage): installation and the parsing APIs.

The syntax guide defines the accepted language. The AST contract defines the
values a parse returns. Examples explain those values in words; they are not
HTML output or a promise about a particular renderer. Styling, navigation,
resource loading, and presentation belong to the application.

## For contributors

- [Testing architecture](../architecture/testing.md): platform runners and correctness/conformance boundaries.
- [Syntax conformance](../architecture/syntax-conformance.md): fixtures, pinned external parsers, and reviewing language changes.
- [Parser architecture](../architecture/syntax-elements.md): ownership of syntax implementations.
- [Toolchains and environment](../toolchains.md): toolchain setup.

`docs/specs/` contains the readable language and API specifications. The root
[`specs/`](../../specs) directory contains executable fixtures and external
comparison policies. Historical implementation plans live in
[`docs/plans/`](../plans); they do not describe pending syntax work.
