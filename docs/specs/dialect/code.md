# Code

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Wrap a short fragment in backticks to preserve it as inline code.

```markdown
Use `*literal*` in this example.
```

The result contains a `Code` node whose literal is `*literal*`. No emphasis
is parsed inside it.

## Include a backtick

Use a longer matching run when the content contains backticks:

```markdown
`` a ` b ``
```

The literal is `` a ` b ``. If a code span begins and ends with a space and is not
entirely spaces, one space is removed from each end. Line endings become spaces.
Escapes and character references remain literal. Opening and closing runs must
have the same length; runs longer than 80 backticks are ordinary text.

An adjacent [attribute container](attributes.md#inline-code) attaches to the
code, as in `` `value`{.language} ``. It is not part of the code's literal.

## Fenced code

Put a fence of at least three backticks or tildes before and after the body.
You can add a language label after the opening fence.

````markdown
```python
print("*still code*")
```
````

This produces a `CodeBlock` with `language="python"`, `info="python"`,
`fenced=true`, and `closed=true`. The body remains literal. The parser does not
run the code or apply syntax highlighting.

The closing fence uses the same character and is at least as long as the
opener. It has no non-whitespace text after it. An unclosed fence still produces
a code block, with `closed=false`, through the end of its containing block or
document. Opening fences allow up to three leading spaces. A backtick fence's
info string cannot contain a backtick.

The info string is trimmed and decodes CommonMark escapes and character
references. Its first space/tab-delimited token is the language, preserving
case. A missing or empty info string gives `info=null` and `language=null`.
Trailing [attributes](attributes.md#fenced-code) are removed before deriving
these values; a class does not supply a language.

A fence whose resulting info is exactly `formula` is a
[formula block](formulas.md#code-fences). Other language labels keep ordinary
code-block behavior.

## Indented code

Indent a block by four columns:

```markdown
    first line
    *literal second line*
```

The result is an indented `CodeBlock`, with `fenced=false`, `closed=true`, and
no info or language. The structural indentation is removed; the remaining body
is literal. An indented code block cannot interrupt an existing paragraph, so
separate it from preceding prose with a blank line.
