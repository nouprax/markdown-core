# Bracketed spans

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `bracketedSpans` (default `false`). Source: Pandoc's
`bracketed_spans`. Executable oracle: the Pandoc 3.11 CLI under
`specs/oracles/pandoc/`. Landing: `P5`.

## Model and syntax

```text
bracketed-span = "[" inline-content "]" attributes

Span(content: [Markup])
```

`Span` is an inline kind. Its content is inline content and may be empty; its
attribute container populates the universal `anchor` and `attributes` fields
under the [attributes](attributes.md) module, and `{}` yields a `Span` with
`anchor=null` and `Attributes.empty`.

Recognition is alternative 4 of the bracket procedure of the
[links and images](links-and-images.md) module: at the `]` that balances the
opener, after a valid direct tail and a resolving reference tail have been
excluded, a valid attribute container beginning at the byte after `]`
produces the span. The brackets use the inherited balanced-bracket scanner:
escaped brackets and brackets owned by code or by a completed inline
construct do not close the span, and the body follows the shared inline
rules. Because a `Span` is not a link, complete links may occur inside it,
each subject to its own no-link-inside-link restriction.

`[text]{.key}` is therefore a `Span`, not a shortcut reference, even when a
definition `text` exists. `[@foo]{.key}` is a `Span` containing an
author-in-text `Cite`. A text directive's label is claimed by the directive
scanner before this procedure, and a `Span` may occur inside such a label. A
cross link is complete at its `]]`, so `[[wiki]]{.x}` is a cross link followed
by text.

## Fallback

An invalid or unclosed container fails this alternative without invalidating
the bracket pair: the cite, shortcut, and literal alternatives then apply to
the same pair and the `{` is text. No partial `Span` is emitted. With
`bracketedSpans=false`, `[text]{.x}` follows the inherited alternatives and
the container is literal text, so `[@foo]{.key}` under `citations` alone is a
bracketed `Cite` followed by literal `{.key}`. Code spans, comments, HTML
tokens, formulas, and cross links are opaque; recognition uses the shared
bracket stack and never searches forward from every `[`.

## Scopes

`Span.scope` covers both brackets, the body, and the complete container.

## Required conformance cases

Tests cover plain, formatted, empty, adjacent, and nested-bracket bodies;
every shared attribute form; immediate versus spaced containers; direct,
full, collapsed, and shortcut reference precedence; citation and link
attribute precedence; directive labels and cross links; escaped, unclosed, and
malformed input; code, comments, HTML, and formulas; exact scopes; option-off
output; nesting limits; allocation failure; and size-doubling bracket and
brace runs.
