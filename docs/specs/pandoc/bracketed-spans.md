# Pandoc bracketed spans

Status: normative target module for `bracketed_spans`. Authority:
[Pandoc User's Guide — bracketed spans](https://pandoc.org/MANUAL.html#extension-bracketed_spans)
at the snapshot pinned by the [Pandoc extension index](../pandoc.md).

## Syntax and AST

```text
bracketed-span = "[" inline-content "]" attributes

Span(
  content: [Markup],
  scope: Scope
)
```

The attribute production, adjacency, universal-field projection, and `{}`
behavior are owned by the [Pandoc attribute contract](attributes.md).
`content` is inline content and may be empty. `scope` covers both square
brackets, the body, and the complete attribute container.

The brackets use the shared balanced-bracket scanner. Escaped brackets and
brackets owned by code or another complete inline construct do not close the
span. Body parsing follows the shared inline rules. Because `Span` is not a
link, complete direct or reference links may occur inside it; the normal
no-link-inside-link restriction still applies within each nested link.

## Precedence and fallback

The suffix determines the outer construct. A valid direct-link destination or
reference tail produces a link; attributes following that completed link are
claimed by the shared Link/Image attachment rule. A `{...}` immediately
following the first balanced `]` instead produces `Span`. Thus
`[text]{.key}` is not a shortcut reference. A bracketed citation candidate
followed by valid attributes has a `Span` as its outer node; an `@key` inside
it may still parse as a one-item author-in-text `Cite`. For example,
`[@foo]{.key}` is a `Span` containing a `Cite`, not one bracketed `Cite`.

An invalid or unclosed attribute container prevents span recognition without
invalidating otherwise legal bracket/link parsing. No partial `Span` is
emitted, and the failed `{` remains available as text.

Source owned by inline code, comments, or an HTML token is opaque. Paired
inline HTML tags do not suppress span recognition between them. Recognition
uses the shared bracket stack and may not search forward repeatedly from every
`[`.

## Required conformance cases

Tests must cover plain, formatted, empty, adjacent, and nested-bracket bodies;
all shared attribute forms; immediate versus spaced suffixes; direct,
full/collapsed/shortcut reference, citation, and link-attribute precedence;
escaped/unclosed/malformed input; code, comments, and HTML; exact content and
owner scopes; option-off behavior; nesting limits; allocation failure; and
size-doubling bracket/brace runs.
