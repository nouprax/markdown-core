# Pandoc superscript and subscript

Status: normative target module for the independent `superscript` and
`subscript` options. Authority:
[Pandoc User's Guide — superscripts and subscripts](https://pandoc.org/MANUAL.html#extension-superscript-subscript)
and the pinned reader's
[`litBetweenNoSpace`](https://github.com/jgm/pandoc/blob/b913622e1ff87c69ab8b1a606577122e220925cd/src/Text/Pandoc/Readers/Markdown.hs#L211-L214)
at the snapshot pinned by the [Pandoc extension index](../pandoc.md).

## AST

```text
Superscript(
  content: [Markup],
  scope: Scope
)

Subscript(
  content: [Markup],
  scope: Scope
)
```

Both are inline `Markup` kinds. Content is reparsed through the shared inline
engine after delimiter and escape processing, so legal formatting such as
`^*x*^` remains structured. Scope includes both delimiters and the body.

## Delimiters

With `superscript=true`, one unescaped `^` opens and the next eligible `^`
closes. With `subscript=true`, one unescaped `~` opens and the next eligible
`~` closes. The body may be empty: `^^` produces an empty `Superscript`, and
`~~` produces an empty `Subscript` when no higher-priority valid strikeout
claims the tildes. This follows from Pandoc's zero-or-more `manyTill` body
parser and is reproduced by the pinned 3.11 CLI as `Superscript []` and
`Subscript []`.

An unescaped whitespace or source line ending before the closer invalidates
the candidate. An ASCII space may be included as `\ `; following Pandoc, that
escape becomes U+00A0 NO-BREAK SPACE in content. Other whitespace remains
invalid. Delimiter characters may likewise be included only through the shared
escape mechanism.

Examples:

```markdown
H~2~O
2^10^
P~a\ cat~
```

produce a subscript `2`, superscript `10`, and subscript containing `a`, a
no-break space, and `cat`, respectively.

## Precedence and fallback

The inline-footnote opener `^[` is tested before superscript, so a valid
enabled inline footnote retains footnote meaning. A valid `~~...~~` strikeout
is tested before subscript. Single-tilde subscript parsing cannot steal one
character from a valid double-tilde strikeout.

Inline code, comments, and HTML-token bytes are opaque. Paired inline HTML tags
do not suppress recognition between them. An unmatched delimiter or candidate
containing unescaped whitespace remains literal and cannot hide a later valid
candidate.

The two options are independent: disabling superscript does not disable
subscript or change strikeout/footnote rules, and vice versa.

## Required conformance cases

Tests must cover plain, empty, formatted, escaped-space, escaped-delimiter,
adjacent, and intraword forms; unescaped ASCII and Unicode whitespace;
newlines; unmatched delimiters; `^[...]` footnote and `~~...~~` strikeout
precedence; code, comments, HTML, and other inline nesting; exact scopes;
independent option gates; allocation failure; and adversarial caret/tilde runs.
