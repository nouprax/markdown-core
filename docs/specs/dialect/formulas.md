# Formulas

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Use dollar delimiters to store mathematical source as a formula.

```markdown
The area is $\pi r^2$.
```

The paragraph contains an embedded `Formula` whose literal is `\pi r^2`.
The body is opaque to Markdown: the caret does not start a superscript. The
parser does not validate TeX or typeset the expression.

## Inline forms

| Spelling | Placement | Body |
| --- | --- | --- |
| `$x$` | Embedded | `x` |
| `` $`x`$ `` | Embedded | `x`, excluding the backticks |
| `$$x$$` | Standalone | `x` |
| `\\(x\\)` | Embedded | `x` |
| `\\[x\\]` | Standalone | `x` |

The last two spellings contain **two literal backslashes** before each bracket.
A single backslash follows ordinary Markdown escaping and does not open a
formula.

```markdown
Compare \\(a+b\\) with $$c+d$$ in this sentence.
```

Both formulas stay inside the paragraph. Their placement modes differ. When
an anonymous paragraph with no attributes contains only one standalone
formula, it becomes a `FormulaBlock`. An anchored or attributed paragraph is
retained.

## Dollar boundaries

A single-dollar opener must be followed by a non-whitespace character; its
closer must be preceded by non-whitespace and must not be followed by an ASCII
digit. Dollar checks use ASCII whitespace. `$$` is tried before `$`.

```markdown
Prices are $5 and $10.
```

These price-like pairs remain ordinary text. Unmatched formula openers release
their source for normal Markdown parsing.

If an inline body has padding at both ends and is not entirely padding, one
space, LF, or CR byte is removed from each end; tabs are not padding. For the
backtick-wrapped dollar form, both inner backticks must be present. Inside the
backslash-bracket forms, a single-backslash escaped matching bracket becomes
the bracket itself; other body bytes stay literal.

## Formula blocks

Put delimiters on lines of their own:

```markdown
$$
x^2 + y^2 = z^2
$$
```

This produces a `FormulaBlock` with literal `x^2 + y^2 = z^2`. Block bodies
lose leading and trailing ASCII whitespace and retain their interior bytes.
The `\\[` and `\\]` lines provide the other block form.

Delimiters allow zero to three leading spaces after enclosing container
prefixes and only whitespace after the marker. An unclosed valid block still
produces a formula through the end of its containing block or document.

## Code fences

A code fence whose info string is exactly `formula` also creates a formula block:

````markdown
```formula
E = mc^2
```
````

Other info strings keep ordinary [code-block](code.md) behavior. The parser
chooses no math renderer and adds no generated equation number.
