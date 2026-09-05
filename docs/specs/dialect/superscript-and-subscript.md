# Superscript and subscript

Status: normative module of the [Markdown Core dialect](../dialect.md).
Options: `superscript` and `subscript` (each default `false`, independent).
Source: Pandoc's `superscript` and `subscript` extensions. Executable
oracle: the Pandoc 3.11 CLI under `specs/oracles/pandoc/`. Landing: `P6`,
which also removes the harness-only double-tilde strikethrough flag.

## Model

```text
Superscript(content: [Markup])
Subscript(content: [Markup])
```

Both are inline kinds whose content is parsed by the shared inline parser, so
`^*x*^` is a superscript containing `Emphasis`.

## Syntax

With `superscript=true`, every unescaped `^` is a delimiter unit at inline
step C4; with `subscript=true`, tildes are delimiter units at step C3 under
the rule below. Units of one kind match by this procedure, applied left to
right within one inline container:

- A unit that finds an unmatched opener of its own kind on the stack closes
  it; otherwise it opens. Same-kind delimiters therefore never nest.
- When the scanner reaches an unescaped whitespace scalar or a line ending, or
  the end of the inline container, every unmatched opener of both kinds is
  removed and its byte is text. A body therefore never contains unescaped
  whitespace.
- Inside a body candidate, `\ ` (a backslash followed by an ASCII space) is
  not whitespace and yields U+00A0 NO-BREAK SPACE in the content; elsewhere
  the inherited literal applies. A character reference that decodes to
  whitespace never invalidates a candidate.
- A body may be empty: `^^` is an empty `Superscript`, and `~~` is an empty
  `Subscript` when it is not a matched strikethrough.

`H~2~O` is a subscript `2`; `2^10^` is a superscript `10`; `P~a\ cat~` is a
subscript containing `a`, a no-break space, and `cat`.

Tildes: with `subscript` on, a run of one tilde is a subscript unit and never
a strikethrough delimiter; a run of two tildes is a strikethrough delimiter,
matched under the [strikethrough](strikethrough.md) rules, and becomes two
subscript units only if it ends unmatched; runs of three or more are text.
With `subscript` off, inherited strikethrough behavior is unchanged. The
[conflicts](conflicts.md) register records this rule as the resolution of the
one collision between the GFM and Pandoc sources.

Carets: an unescaped `^` immediately followed by `[` is an inline-footnote
opener under `inlineFootnotes` and `footnotes`, tested before this module.
A `^` removed by [block identifier](block-identifiers.md) attachment is never
a delimiter, because that attachment is decided before inline parsing. A `^`
or `~` owned by an autolink, code span, HTML token, comment, formula, or cross
link is opaque.

## Option behavior and fallback

With `superscript=false`, `^` is text; with `subscript=false`, `~` follows the
strikethrough module alone. A failed candidate is text and cannot hide a later
valid candidate. Delimiter scalars enter a body only through the shared escape
mechanism.

## Scopes

Both scopes cover the two delimiters and the body, including an escaped
space.

## Required conformance cases

Tests cover plain, empty, formatted, escaped-space, escaped-delimiter,
adjacent, and intraword forms; unescaped ASCII and Unicode whitespace,
newlines, and character references; unmatched delimiters; `^[` footnote and
`~~` strikethrough precedence; a caret removed by a block identifier; code,
comments, HTML, formulas, autolinks, and other inline nesting; exact scopes;
each option independently on and off; allocation failure; and adversarial
caret and tilde runs.
