# Shared canonical AST conformance spec

This directory is the repository's only canonical Markdown/AST corpus. Each
Markdown input has a reviewed `.ast` companion containing its expected
canonical file-tree representation. `manifest.json` is the sole case list and
freezes case order, paths, parse options, UTF-8/LF rules, and coverage labels.
C, Swift, Kotlin, and ES conformance targets must enumerate it directly or use
build-generated resources derived from it. A runner or platform-owned copy is
not allowed in this directory.

These files are test-only product contract data. They are not a production
serialization format, a C-to-binding transport, or a public API. Every binding
parses the Markdown through its public `Document.parse`, traverses its own
immutable public AST through its public Visitor/Walker/TreeDumper path, and
compares the result byte for byte. No production path may consume dump text,
and no release artifact may contain this directory.

Run `node scripts/check-canonical-ast-fixtures.mjs` to audit the schema,
discovery, grammar, declared coverage, and completeness. Intentional parser or
AST contract changes may run
`scripts/generate-canonical-ast-candidates.sh`; it writes candidates below
`build/` and prints diffs without changing accepted goldens. Tests and CI never
rewrite or accept them. A grammar or schema change must update the contract,
manifest, goldens, all four implementations, and conformance evidence in the
same change.

Scopes copy the native C parser's line and column values exactly, including
zero-valued combinations. The goldens do not reinterpret or normalize them.
