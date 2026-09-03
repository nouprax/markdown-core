# Pandoc heading anchors and implicit references

Status: normative target module for Markdown Core `auto_anchors` and
`implicit_header_references`. Attribute syntax and attachment, including
`header_attributes`, are defined only by the
[Pandoc attribute contract](attributes.md). Authority:
[automatic identifiers](https://pandoc.org/MANUAL.html#extension-auto_identifiers),
[GFM automatic identifiers](https://pandoc.org/MANUAL.html#extension-gfm_auto_identifiers),
and
[implicit heading references](https://pandoc.org/MANUAL.html#extension-implicit_header_references)
at the snapshot pinned by the [Pandoc extension index](../pandoc.md).

`auto_anchors` is the public Markdown Core option name. It composes Pandoc's
`auto_identifiers` with `gfm_auto_identifiers`; the non-GFM Pandoc slug
algorithm is not another mode in this contract.

## GFM automatic anchors

With `auto_anchors=true`, every heading lacking a non-empty explicit `id`
receives a synthesized `id` attribute. Generate its base from the parsed
heading inline content in this order:

1. Project to visible plain text: remove formatting containers while retaining
   their text, retain link labels and code literals, and remove footnote
   content/calls.
2. Replace recognized emoji shortcodes and Unicode emoji with their canonical
   GitHub alias names, without surrounding colons.
3. Apply Unicode lowercase mapping.
4. Replace each Unicode whitespace scalar with one ASCII `-`; do not collapse
   adjacent replacements.
5. Remove every scalar except Unicode letters/numbers, Unicode combining
   marks, connector punctuation, `-`, and `_`.
6. If the result is empty, use `section`.

Punctuation removal does not insert whitespace, so separated source fragments
may concatenate. Formatting delimiters never contribute. For example,
`## My Header` receives `id="my-header"`.

Generated and explicit IDs share one document registry. If the base already
exists, append `-N` using the smallest positive decimal `N` not yet registered.
An explicit duplicate remains as authored and may produce a diagnostic, but it
still occupies the registry; it is not silently renamed. Generation is in
heading source order and is deterministic.

The generated attribute has no source scope. The `Heading.scope` remains the
authored heading range.

## Implicit heading references

With `implicit_header_references=true`, each heading with a non-empty final
`id` contributes a virtual reference definition. Its key is the authored
heading-label source after removing the ATX/Setext heading syntax, optional ATX
closing hashes, and trailing heading attributes, then applying inherited
case-insensitive reference-label normalization. Inline markup remains part of
the label source: `# *Foo*` is referenced by `[*Foo*]`, not `[Foo]`. The
virtual definition targets `#` followed by the final ID, which is independently
derived from visible content when automatic.

All ordinary reference spellings may resolve it:

```markdown
[First chapter]
[First chapter][]
[go there][First chapter]
```

The result is an ordinary resolved `Link(destination="#id", content, ...)`;
the virtual definition and lookup label are not public AST nodes. Link
attributes written at the occurrence remain governed by the Pandoc attribute
contract.

An explicit source reference definition always wins over a virtual heading
definition. If multiple headings have the same normalized authored label, the
virtual reference targets the first, even though later headings have distinct
automatic IDs. Explicit fragment links remain available for later headings.
Unresolved candidates retain inherited fallback.

## Required conformance cases

Tests must cover explicit IDs composed with auto anchors; formatting, links,
code, and footnotes in heading text; Unicode case/marks/connectors/whitespace;
punctuation removal; emoji aliases; empty bases; duplicate bases and explicit
duplicates; all three reference spellings; Unicode/case-folded labels;
formatted heading labels and their plain-text mismatch; explicit-definition
priority; duplicate heading labels; occurrence attributes; exact scopes;
independent option gates; allocation failure; and large duplicate heading sets.
