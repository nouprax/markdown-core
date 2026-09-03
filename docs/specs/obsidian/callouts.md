# OFM callouts

Status: normative target module. Authority:
[Callouts](https://obsidian.md/help/callouts) and the inherited CommonMark block
quote grammar at the snapshot pinned by the
[OFM contract index](../obsidian-flavored-markdown.md).

## One quoted-container model

Every source container introduced by `>` produces `Callout`. A plain quoted
block is a callout without metadata. The target canonical AST contains no
`BlockQuote` kind, alias, wrapper, or compatibility projection.

```text
Callout(
  variant: String?,
  fold: none | expanded | collapsed,
  title: [Markup]?,
  content: [Markup],
  scope: Scope
)
```

The inherited CommonMark algorithm continues to own `>` prefix, laziness,
continuation, blank-line, and nesting rules. This module changes the semantic
node name and adds optional Obsidian metadata; it does not implement another
container parser.

## Metadata grammar

Metadata is recognized only at the start of the first content line after the
inherited quote prefix and optional quote padding.

```text
metadata-line = "[!" type "]" [ fold-marker ] [ title-separator title ]
type          = 1*( ASCII-letter / DIGIT / "_" / "-" )
fold-marker   = "+" / "-"
title-separator = 1*( ASCII-space / ASCII-tab )
```

`+` means initially expanded and `-` means initially collapsed. The marker must
immediately follow `]`. A title is the non-empty remainder after its structural
separator; trailing source spaces remain subject to inherited inline rules. A
line with no non-space remainder has no custom title.

The type character set is Markdown Core's explicit malformed-marker boundary.
The official help defines type identifiers, shows hyphenated custom types, and
makes matching case-insensitive, but does not publish a complete error grammar.

## Field invariants

- `variant == null` means no valid metadata line. Then `title == null`,
  `fold == none`, and every quoted source byte remains represented by
  `content`.
- A non-null `variant` is the non-empty, ASCII-lowercased value of the authored
  type identifier. The source scope retains the authored case.
- `fold == none` with a non-null variant means no `+` or `-` was authored.
- `title == null` means no non-empty custom title was authored. A title is
  parsed inline content and is not part of structural `content`.
- With valid metadata, the complete metadata line is removed from `content`.
  The following quoted blocks form the body; the body may be empty.
- `scope` covers every quote marker, metadata, title, and body.

Block-identifier attachment contributes its own field and scope extension under
the separate block-identifiers spec. It is not callout metadata and is not
defined by this module.

Default titles, built-in aliases, icons, colors, CSS custom types, and current
fold-open state are renderer/runtime data. An unsupported source type remains
its normalized `variant`; the parser does not replace it with `note`.

## Nesting and fallback

Nested quote depth produces nested `Callout` nodes through the inherited
container recursion. Metadata is evaluated independently for each node's first
content line. Titles and bodies may contain every inline/block construct legal
in their respective content model, including wikilinks and embeds.

An invalid marker, a marker after the first content line, or a marker separated
from the first position by other content does not populate metadata. It remains
ordinary content in a metadata-free `Callout`. This fallback does not create a
second quoted-container kind.

## Required conformance cases

| Source shape | Required state |
| --- | --- |
| `> quote` | `variant=null`, `title=null`, `fold=none` |
| `> [!info]` | `variant="info"`, empty body |
| `> [!TIP] Title` | `variant="tip"`, inline title, empty body |
| `> [!faq]+ Title` | `fold=expanded` |
| `> [!faq]- Title` | `fold=collapsed` |
| `> [!custom-type]` | custom type retained as `variant` |

Tests must also cover formatted titles, body blocks, title-only values, every
built-in alias spelling, unsupported types, invalid type characters, misplaced
markers, nested metadata-free/metadata-bearing combinations, attached block
identifiers, inherited lazy continuation, exact scopes, allocation failure, and
adversarial quote depth.
