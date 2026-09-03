# Pandoc Markdown extensions

Status: normative target index. Authority: Pandoc 3.11, released 2026-08-29,
at immutable tag commit `b913622e1ff87c69ab8b1a606577122e220925cd`.
Implementation sequencing and proof obligations are defined by the
[Pandoc Markdown extensions plan](../plans/2026-09-03-pandoc-markdown-extensions.md).
The [Pandoc User's Guide](https://pandoc.org/MANUAL.html#pandocs-markdown)
owns the source language; the tagged
[Markdown reader](https://github.com/jgm/pandoc/tree/b913622e1ff87c69ab8b1a606577122e220925cd/src/Text/Pandoc/Readers)
and official Pandoc 3.11 native/JSON output are grammar and executable AST
evidence. Their immutable blobs, release artifacts, checksums, extension map,
and runner restrictions are fixed by the
[Pandoc oracle policy](../../specs/oracles/pandoc/README.md).

Pandoc 3.11 also owns the one shared braced-attribute grammar. Its `Attr` tuple
is projected into the universal anchor plus the shared classes-and-records
attribute model. Remark contributes directive envelopes and attachment
positions but does not introduce a second attribute grammar.

This directory groups independently composable syntax extensions. It does not
define a monolithic Pandoc dialect or change inherited CommonMark/GFM behavior.
Enabling one option enables only its named recognition rule and semantic
projection. Unless a future preset explicitly says otherwise, every option in
this index is opt-in.

The shared value contract lives outside this directory:

- [Anchors](anchors.md) owns the universal `Markup.anchor` field and its
  source-independent consumer meaning.
- [Destinations](destinations.md) owns the shared tagged target value;
  ordinary Markdown links, including implicit heading references, use its
  `url` branch.
- [Attributes](attributes.md) owns the universal `Markup.attributes` field, the
  shared classes-and-records shape, sole grammar, value invariants,
  normalization, and merge operation.
- [Citation model](citation-model.md) owns `Cite`, `Citation`, and
  `CitationReferent` independently of Pandoc bibliography syntax.

## Modules

| Requested extension                                                                                                            | Normative module                                                 |
| ------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------- |
| `inline_code_attributes`, `header_attributes`, `fenced_code_attributes`, `link_attributes`, and the Pandoc attachment registry | [Pandoc attributes](pandoc/attributes.md)                        |
| `citations` (bibliography branch)                                                                                              | [Citations](pandoc/citations.md)                                 |
| `bracketed_spans`                                                                                                              | [Bracketed spans](pandoc/bracketed-spans.md)                     |
| `superscript`, `subscript`                                                                                                     | [Superscript and subscript](pandoc/superscript-and-subscript.md) |
| `auto_anchors`, `implicit_header_references`                                                                                   | [Heading anchors](pandoc/headings-and-anchors.md)                |
| `simple_tables`, `multiline_tables`, `grid_tables`, `table_captions`                                                           | [Tables](pandoc/tables.md)                                       |
| `fancy_lists`, `startnum`, `example_lists`                                                                                     | [Ordered and example lists](pandoc/lists.md)                     |
| `definition_lists`, requested compact form                                                                                     | [Definition lists](pandoc/definition-lists.md)                   |
| `fenced_divs`                                                                                                                  | [Fenced divs](pandoc/fenced-divs.md)                             |

`auto_anchors` is Markdown Core's public option name for Pandoc's
`auto_identifiers` with `gfm_auto_identifiers`; it deliberately selects the
GitHub algorithm requested by this contract. `compact_definition_lists` is not
a Pandoc 3.11 extension. Compact and loose definitions are two source forms of
`definition_lists` and are modeled without inventing a second parser gate.

## Common implementation rules

All extensions operate inside the shared block/inline parsers. They may add a
token class, delimiter, or block start, but may not rescan a completed AST with
regular expressions. Source owned by code, comments, or an HTML token/block is
opaque; paired inline HTML tags do not make intervening Markdown opaque.

Recognition failure is local and transactional. It must release source to the
inherited parser without consuming a bracket, fence, delimiter, or attribute
container required by a later construct. All scanners remain linear under
unmatched delimiters and size-doubling adversarial input, obey the shared
nesting limit, and propagate allocation failure without partial nodes.
