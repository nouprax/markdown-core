# Pandoc extension oracle

The executable oracle is the official Pandoc 3.11 CLI. Pandoc is both the
producer of these extensions and the producer of its native JSON model, so a
third-party popularity proxy would add uncertainty rather than independence.
The pinned Pandoc User's Guide is the source of the feature definitions, the
dialect modules under `docs/specs/dialect/` are the rule, and the CLI supplies
reproducible evidence for recognition, fallback, grouping, ordering, and
semantic facts.

[`source.json`](source.json) is the immutable authority and runner contract. It
pins the 3.11 tag commit, the exact manual, Markdown reader, shared attribute
merge, and extension-registry blobs, and the official portable archives with
the SHA-256 digests published by GitHub's release service. A later version is
not accepted implicitly; moving any pin is a reviewed language-baseline change.

This is an extension-layer oracle, not a Pandoc-dialect oracle. Every case in
[`corpus.json`](corpus.json) declares `markdown_strict` plus only the extension
set required by that case. Invoking Pandoc's default `markdown` reader is
forbidden because it would enable unrelated rules. The base reader is merely
a controlled harness substrate: current cmark and cmark-gfm policies continue
to own Markdown Core's inherited CommonMark/GFM behavior.

Pandoc 3.11 is also the source and evidence for the one shared braced-attribute
grammar and its semantic `Attr` shape, which `docs/specs/dialect/attributes.md`
states. Markdown Core projects that tuple into its
consumer model: the identifier becomes `Markup.anchor`, classes become
`Attributes.classes`, and ordered key/value pairs become `Attributes.records`
of `Record` values. Pandoc's empty identifier projects to `anchor=null`; neither
array is otherwise flattened or deduplicated. The same grammar applies at
Remark directive attachment sites; Remark owns the directive envelope, not a
second attribute member language.

Pandoc JSON's `Link` and `Image` target URLs are projected into the shared
consumer value as `dest = Destination.url(url)` on their respective nodes. A
fragment-only target such as `#section` remains a URL-branch reference to the
declaration-side `Markup.anchor`; the projection does not copy that value into
the reference node's own anchor field.

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
source ranges. Attribute cases compare all three projected components and
their order; the records sequence may not collapse into a unique-key map.
