# Strikethrough

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Wrap text in exactly two tildes to mark it as deleted.

```markdown
The ~~old wording~~ has changed.
```

The paragraph contains a `Strikethrough` with the text `old wording`. A renderer
can draw a line through that content. The tildes are excluded from the visible
content but included in the node's scope.

## Combine formatting

```markdown
~~This is *no longer* current.~~
```

The strikethrough contains text and nested emphasis. Links and other inline
syntax can also appear inside it. The opening pair must be followed by
non-whitespace and the closing pair preceded by non-whitespace, following
CommonMark's asterisk flanking rules without the rule of three. Intraword
strikethrough is allowed.

## Literal tildes

A single tilde belongs to [subscript](superscript-and-subscript.md), so `~x~`
is not strikethrough. Runs of three or more tildes are literal inline text;
at a block start, three tildes may instead open a [code fence](code.md).

```markdown
Inline ~~~text~~~ and \~\~literal\~\~
```

In this paragraph, the first run remains literal and the escaped pairs produce
ordinary `~~literal~~` text. An unmatched pair is also text. Tildes inside
code, formulas, comments, raw HTML tokens, or cross links do not participate.
