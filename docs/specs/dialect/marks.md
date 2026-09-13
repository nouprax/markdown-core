# Highlights

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Wrap text in two equals signs to mark it for highlighting.

```markdown
Remember ==this part==.
```

The result contains a `Mark` node with the text `this part`. The parser records
the highlight; the application chooses its color and presentation.

## Format highlighted text

```markdown
==Read the **important** [details](/details).==
```

The highlight contains ordinary parsed inline content, including strong text
and a link. It can span soft line breaks inside a paragraph, but not a block
boundary.

Pairs use CommonMark's asterisk flanking rules without the rule of three:
opening pairs need non-whitespace after them, closing pairs need non-whitespace
before them, and punctuation affects whether a pair can open or close.
Intraword highlights are allowed.

```markdown
a==b==c

==d====e==

===a===
```

The first example highlights `b`; the second has two adjacent highlights.
The last has one literal `=` on each side of the highlighted `a`. Matching
consumes pairs, so repeated pairs can nest.

## When signs stay literal

`if a == b` and an unmatched `==` remain text. Escape a sign with `\=` to
prevent it from starting a highlight. Code, formula and comment bodies, HTML
tokens, cross links, and automatic URL tokens protect their own equals signs.
A Setext heading underline is decided before inline highlights.
