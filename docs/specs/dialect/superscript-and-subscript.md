# Superscript and subscript

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Wrap text in carets for superscript or single tildes for subscript.

```markdown
2^10^ and H~2~O
```

This produces `Superscript` containing `10` and `Subscript` containing `2`.
The numbers are text, not values calculated by the parser.

## Include formatting or spaces

The body supports inline formatting, but raw whitespace invalidates a script
pair. Escape an ASCII space when the intended script contains multiple words:

```markdown
x^a\ b^ and y~*small*~
```

The superscript contains `a`, a non-breaking space (U+00A0), and `b`. The
subscript contains emphasis. The escape-to-NBSP conversion applies only inside
a successfully completed script; outside it, backslash-space stays literal.
A character reference that decodes to whitespace does not invalidate the
pair, because this restriction applies to the authored source.

Raw whitespace in a nested inline field, such as a directive label, also
invalidates its enclosing script. Literal bodies owned by code, formulas,
comments, HTML tokens, and cross links keep their own contents.

## Delimiter boundaries

Same-kind scripts do not nest: a matching delimiter closes the pending script
before another can open. `^^` is a valid empty superscript. `~~` belongs to
[strikethrough](strikethrough.md), so it is not an empty subscript; runs of three
or more tildes remain literal inline text.

```markdown
x^two words^ and x~two words~
```

These pairs remain text because their bodies contain raw spaces. Unmatched
and escaped delimiters are text as well.

A caret immediately followed by `[` first attempts an
[inline footnote](footnotes.md#inline-footnotes). Write `^\[note]^` if a
superscript should begin with a literal opening bracket.
