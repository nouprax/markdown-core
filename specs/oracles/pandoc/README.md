# Pandoc extension oracle

The executable oracle is the official Pandoc 3.11 CLI. Pandoc is both the
producer of these extensions and the producer of its native JSON model, so a
third-party popularity proxy would add uncertainty rather than independence.
The normative source language remains the pinned Pandoc User's Guide; the CLI
supplies reproducible evidence for recognition, fallback, grouping, ordering,
and semantic facts.

[`source.json`](source.json) is the immutable authority and runner contract. It
pins the 3.11 tag commit, the exact manual/reader/extension-registry blobs, and
the official portable archives with the SHA-256 digests published by GitHub's
release service. A later version is not accepted implicitly; moving any pin is
a reviewed language-baseline change.

This is an extension-layer oracle, not a Pandoc-dialect oracle. Every case in
[`corpus.json`](corpus.json) declares `markdown_strict` plus only the extension
set required by that case. Invoking Pandoc's default `markdown` reader is
forbidden because it would enable unrelated rules. The base reader is merely
a controlled harness substrate: current cmark and cmark-gfm policies continue
to own Markdown Core's inherited CommonMark/GFM behavior.

Braced attribute tokenization is one explicit exception to Pandoc authority.
Markdown Core uses the pinned Remark/micromark attribute grammar at every
attachment site. It therefore treats `.` and `#` as adjacent shorthand
boundaries, retains `:` inside a shorthand, treats `{-}` as the empty-valued
name `-`, and accepts other bare names. Upstream Pandoc's different results are
unsupported. The future parity gate must preserve these as declared semantic
deltas rather than normalize them away or treat them as product gaps.

The corpus contains inputs and runner options only. It contains no Markdown
Core expected AST and no stored Pandoc output. The planned parity gate will run
both parsers, project their semantic trees, and register every current product
gap fail-closed as described by the
[implementation plan](../../../docs/plans/2026-09-03-pandoc-markdown-extensions.md).
Until that gate lands, this directory freezes the oracle source and initial
corpus but does not claim executable product parity.

The comparison must use `--to=json` without citeproc, filters, defaults files,
templates, metadata files, bibliography lookup, or network access. A semantic
projection may translate representation-only Pandoc constructors into the
canonical consumer model; it may not erase a syntax, ordering, content,
attribute, citation, list, heading, or table difference. Product scopes remain
owned by Markdown Core fixtures because Pandoc JSON does not expose compatible
source ranges. Attribute cases are compared too; their declared authority
exception must reproduce fail-closed.
