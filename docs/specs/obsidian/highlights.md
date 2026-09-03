# OFM highlights

Status: normative target module. Authority:
[Basic formatting syntax — Bold, italics, highlights](https://obsidian.md/help/syntax#Bold,%20italics,%20highlights)
at the snapshot pinned by the
[OFM contract index](../obsidian-flavored-markdown.md).

## Syntax and AST

Exactly two unescaped equals signs open and close a highlight within one inline
container. A delimiter run longer than two is not a highlight delimiter. A
valid highlight has non-empty source content. The exact-run and non-empty rules
are Markdown Core's explicit malformed-input boundary because the help page
documents the valid `==text==` form but does not publish an error grammar.

```text
Mark(
  content: [Markup],
  scope: Scope
)
```

The body is parsed through the shared inline engine. It may therefore contain
text, emphasis, strong, strikethrough, links, wikilinks, footnote
`Cite` nodes, and other legal inline nodes. `scope` covers both `==`
delimiter runs and the body.
This compositional content model is normative even though the supplementary
oracle currently represents the body as one text child.

## Delimiter behavior

`Mark` recognition participates in the shared delimiter stack and cannot use a
whole-document regex. Inline code, comments, and source bytes owned by an HTML
token are opaque; paired inline HTML tags do not suppress recognition between
them. Escaped equals signs do not delimit. Delimiter pairing remains local to
the current inline container and obeys source order with the existing emphasis
and strikethrough delimiters.

An empty pair, a longer delimiter run, or an unmatched opener/closer remains
text. Failed recognition cannot consume equals signs needed by a later valid
pair. Nesting must use the normal inline recursion/stack limit and remain
allocation-failure strict.

## Required conformance cases

Tests must cover plain and formatted bodies, adjacent highlights, highlights
inside table cells, callout titles, and `Footnote` content; escaped and
unmatched runs; empty and triple-equals forms; code/comments/HTML; exact scopes;
allocation failure; deep mixed-delimiter input; and size-doubling equals runs.
