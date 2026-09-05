# Strikethrough

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `strikethrough` (default `true`). Source: cmark-gfm's strikethrough
extension. Executable oracle: cmark-gfm under `specs/oracles/cmark-gfm/`.
Landing: present; the single-tilde interaction with `subscript` lands with
`P6`.

## Model

```text
Strikethrough(content: [Markup])
```

`Strikethrough` is an inline kind whose content is parsed by the shared inline
parser.

## Syntax

Tildes are delimiters on the shared stack at inline steps C2 and C3. A run of
one or two unescaped tildes is one delimiter unit; a run of three or more is
text. A unit can open when it is left-flanking and close when it is
right-flanking under the CommonMark definitions, and `~` itself is transparent
to the flanking tests of every delimiter: when the scalar next to a delimiter
run is a tilde, the test looks past the tilde run to the next scalar, so
`*~~a~~*` is `Emphasis(Strikethrough("a"))`. A closer matches the nearest
unmatched opener of the same length; a one-tilde unit and a two-tilde unit
never match each other. Matching uses the inherited process-emphasis
algorithm without the rule of three.

`~~a~~` and `~a~` are both `Strikethrough("a")` while `subscript` is off. With
`subscript` on, the [superscript and subscript](superscript-and-subscript.md)
module owns single tildes: a run of one tilde is never a strikethrough
delimiter, a run of two is strikethrough when matched under the rules above
and otherwise two subscript units, and runs of three or more stay text. This
is the one place where two sources define the same bytes differently; the
[conflicts](conflicts.md) register records it.

## Option behavior and fallback

With `strikethrough=false`, every tilde is text, and with `subscript` also off
nothing recognizes a tilde. An escaped `\~` never delimits. Code spans, HTML
tokens, comments, formulas, and cross links are opaque. An unmatched unit is
text.

## Scopes

`Strikethrough.scope` covers both tilde runs and the body.

## Required conformance cases

The GFM strikethrough sections of the specification corpus and the
repository's extension fixtures remain byte-identical. Tests also cover one-
and two-tilde forms, mismatched lengths, runs of three, tildes beside
emphasis delimiters, escaped tildes, opaque contexts, exact scopes, the
option off, every combination with `subscript`, allocation failure, and
size-doubling tilde runs.
