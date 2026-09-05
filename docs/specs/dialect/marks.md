# Marks

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `marks` (default `false`). Source: Obsidian's `==highlight==`.
Executable oracle: `@quartz-community/remark-obsidian`, whose one-text-child
content model is a registered projection. Landing: `O2`.

## Model

```text
Mark(content: [Markup])
```

`Mark` is an inline kind whose content is parsed by the shared inline parser
and may contain any inline construct whose delimiters nest legally inside it.

## Syntax

`==` is a delimiter on the shared stack at inline step C5. A candidate is a
maximal run of unescaped `=` of length exactly two; runs of one or of three or
more are text. A candidate can open if and only if it is left-flanking and can
close if and only if it is right-flanking, under the CommonMark definitions
with the same character classes as `*`, so intraword pairs are allowed.
Matching uses the inherited process-emphasis algorithm without the rule of
three. A matched pair with an empty region, `====`, is text. `==a==b==` is
`Mark("a")` followed by text `b==`, and `if a == b and c == d` contains no
mark because neither run can open.

Validity is decided on source; `==%%c%%==` is a `Mark` whose content is one
`Comment`. Block structure is decided first, so a Setext underline of `=` is
never a closer. Bare autolinks run after delimiter processing over `Text`
only, so `http://x/?a==b== c` ends its URL at the mark boundary.

## Option behavior and fallback

With `marks=false`, `=` runs are text. With the option on, an escaped `\=`
never delimits, an unmatched candidate is text, and a failed pair cannot
consume equals signs needed by a later valid pair. Code spans, HTML tokens,
comments, formulas, and cross links are opaque. Pandoc's `mark` extension
delimits the same bytes with a different boundary rule; the
[conflicts](conflicts.md) register records the difference.

## Scopes

`Mark.scope` covers both delimiter runs and the body.

## Required conformance cases

Tests cover plain and formatted bodies, adjacent and intraword marks, marks
inside table cells, callout titles, and footnote content, escaped and
unmatched runs, empty and triple-equals forms, a comment as sole content,
Setext and autolink boundaries, code, comments, HTML, and formulas, exact
scopes, option-off output, allocation failure, deep mixed-delimiter input, and
size-doubling equals runs.
