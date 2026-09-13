# Insertions

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Wrap text in two plus signs to mark it as inserted.

```markdown
We added ++a new paragraph++.
```

The paragraph contains an `Insertion` node with parsed inline content. A
renderer can underline or otherwise distinguish the addition; no styling is
stored in the AST.

## Nest and combine

```markdown
++Read **this** first.++

++++nested++++

+++added+++
```

The first insertion contains strong text. The second nests two insertions.
The third leaves one literal plus outside each side of a single insertion.
Pairs can span a soft line break within the same inline container.

## Matching rules

Matching consumes two signs at a time. Opening pairs must be followed by
non-whitespace, closing pairs must be preceded by non-whitespace, and the
CommonMark asterisk punctuation/flanking rules apply without the rule of three.
Intraword insertion is allowed. Units in one uninterrupted run cannot match
each other, so `++++` and `a++++b` remain text.

Single plus signs, unmatched pairs, and escaped `\+` are literal. Crossing
delimiters do not gain an extra pairing to repair the source. Code, formulas,
comments, HTML tokens, cross links, and automatic URL tokens protect their
contents. At a block start, `+ item` can instead be a [bullet list](lists.md).
