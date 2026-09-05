# OFM inherited syntax and integration

Status: normative target module. Authority:
[Obsidian Flavored Markdown](https://obsidian.md/help/obsidian-flavored-markdown),
[Basic formatting syntax](https://obsidian.md/help/syntax),
[Advanced formatting syntax](https://obsidian.md/help/advanced-syntax),
[Embed files](https://obsidian.md/help/embeds), and
[Search](https://obsidian.md/help/plugins/search) at the snapshot pinned by the
[OFM contract index](../obsidian-flavored-markdown.md).

This module owns cross-feature composition and the documented inherited forms
that need no new standalone syntax node.

## Reference links and images

Reference links are a source-level indirection for constructing `Link`, not a
consumer-visible reference relationship. After the inherited reference-label
algorithm resolves a definition, all equivalent authoring forms produce the
same semantic node:

```text
[text](url "title")          \
[text][label]                  |
[text][]                       |--> Link(dest=Destination.url(url), title, content, scope)
[label]                        |
<autolink>                    /
```

The resolved `Link.content` is the inline content authored at that occurrence.
The `Destination.url` value and `title` come from the inline destination or the
winning reference definition. Its `scope` covers the link occurrence, not a
separate definition elsewhere in the document. Resolution and inheritance
never create an expanded or composite scope. This module uses the shared
[destination model](../destinations.md); reference resolution cannot expose a
bare string in parallel with `Link.dest`.

Direct and reference images normalize in the same way to `Image`. Their alt
content belongs to each `Image` occurrence; their `Destination.url` and title
come from the direct destination or winning reference definition, and
resolution does not change the occurrence's source-faithful scope. No
`Image.source` string survives beside `Image.dest`.

Reference labels, normalized lookup keys, and full/collapsed/shortcut form are
parser-resolution data. A successfully parsed reference definition populates
the parser's internal map but produces no public AST value. An unreferenced
definition likewise produces no semantic output. A call that does not resolve,
or source that does not satisfy the inherited definition grammar, retains the
inherited literal/content fallback.

The canonical vNext AST therefore has no `LinkReference`, `ImageReference`,
`ReferenceDefinition`, or `ReferenceForm`. It does not model reference links as
`Citation`, does not add a `CitationReferent.link` branch, and does not add
`Document.links`.
Two links resolved through the same source definition have no shared consumer
identity after resolution; each is an ordinary `Link`.

The parser may intern or share a resolved destination internally, but storage
optimization cannot change the public model. Resolution performs one lookup in
the shared normalized-reference map per candidate and remains linear in source
bytes plus emitted AST size; it may not repeatedly scan definitions or expose
the lookup table through a binding.

## Tables

OFM tables use the existing typed `Table`, `TableRow`, and `TableCell` model.
Outer pipes are optional. A delimiter cell contains at least two hyphens;
leading/trailing colons select alignment. Basic inline and OFM inline syntax is
valid in cells.

A pipe belonging to a wikilink label or an embed size must be authored as `\|`
inside a table. Table boundary scanning recognizes that escape before splitting
cells, while the inline scanner receives the logical pipe and parses one
`CrossLink`. There is no table-specific wikilink parser; the escape rule applies
while `tables` and `crossLinks` are both on.

Ordinary GFM tables keep the same algorithm and AST under every option set. The
inherited delimiter-row grammar is unchanged; a two-hyphen delimiter cell is a
positive example of it, not a separate rule.

## External image dimensions

Standard external images remain `Image` and gain:

```text
width: Int?
height: Int?
```

Under `imageDimensions`, the following complete alt-label suffixes are
recognized, using positive decimal pixel values and lowercase `x` with no
surrounding spaces:

```text
W
WxH
alt|W
alt|WxH
```

For a numeric-only label, visible alt content is empty. For a pipe form, the
unescaped final pipe introduces the size suffix and preceding bytes remain alt
content parsed by the existing inline engine. Width-only forms set
`height=null`; aspect-ratio preservation is renderer behavior. Zero, overflow,
signs, whitespace around `x`, missing components, or non-decimal components do
not produce dimensions and retain the complete authored label as alt content.

Internal image embeds remain `CrossLink`; their `label` stays raw until a
downstream resolver establishes the target kind. The parser never fetches an
image or reads intrinsic dimensions.

## Math and executable-looking code blocks

`$...$` produces the existing inline `Formula`; `$$...$$` produces `Formula`
or `FormulaBlock` according to the existing canonical placement rules. Formula
bodies are opaque strings. The parser does not validate TeX or run MathJax.

Fenced `mermaid` and `query` source remains `CodeBlock` with the authored info
string/language. Diagram rendering and search execution are downstream. Code
block bodies remain opaque to every OFM extension.

## Inherited HTML behavior

No Obsidian option changes CommonMark HTML recognition. An HTML
block remains an opaque `HTMLBlock`, and each inline HTML token remains an
`HTML` leaf. Paired inline opening and closing tags do not establish an element
region in the Markdown AST, so intervening source is parsed normally. For
example, `<span>**bold**</span>` produces `HTML`, `Strong`, and `HTML` siblings.

OFM extensions follow the same boundary: they are not recognized inside source
bytes already owned by an `HTML` or `HTMLBlock` token, but they remain enabled
between separate inline HTML tokens. HTML comments, declarations, processing
instructions, CDATA, malformed input, and all other cases retain the pinned
cmark behavior without an Obsidian-specific element stack or tokenizer.

## Cross-feature precedence

Within the shared block and inline passes, precedence is:

1. fenced/indented code and block HTML are opaque;
2. `>` containers, lists, and tables establish ownership before callout
   metadata and block-identifier attachment;
3. inline code, inline HTML token literals, and comments suppress other inline
   recognition only for the source bytes they own;
4. table boundary recognition preserves escaped wiki pipes for the shared
   inline wikilink scanner; and
5. footnote `Cite` nodes, wikilinks, highlights, and inherited delimiters compose
   in source order through the shared bracket/delimiter infrastructure.

No extension may rescan a completed AST node or reinterpret a field in a
binding. Invalid syntax falls back locally according to its owning module and
must not change later delimiter ownership.

## Required conformance cases

Tests must cover two-hyphen/aligned/pipe-optional tables; formatted cells;
escaped wikilink aliases and embed sizes in cells; every valid and invalid image
dimension form; inline/display math; `mermaid` and `query` blocks containing
Markdown-looking bytes; inherited inline-HTML and HTML-block behavior, including
CommonMark and OFM constructs between paired inline tags; every
inline/full/collapsed/shortcut/unresolved link form; direct and reference
images; unused and duplicate definitions; task items carrying block
identifiers; every pairwise opaque-context interaction; exact scopes;
allocation failure; and adversarial reference/table/HTML/delimiter inputs with
structural resource bounds.
