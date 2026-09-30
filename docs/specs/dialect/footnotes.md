# Footnotes

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Write a call as `[^label]` and define its body with `[^label]:`.

```markdown
A statement with a note.[^source]

[^source]: The supporting detail.
```

The paragraph contains a `Cite` with a footnote reference to the label
`source`. The definition is a `Footnote` block with the label `source`, in the
content where it was written. `Document.footnotes` lists every footnote in
source order, and `Document.footnote(for:)` answers the one a label names.
The parser assigns no displayed footnote number.

## Definition bodies

Indent continuation lines by at least four columns. Bodies can hold paragraphs
and other blocks:

```markdown
Read this.[^detail]

[^detail]: First paragraph.

    Second paragraph with *emphasis*.
```

The footnote contains two paragraphs. Definitions can appear before or after
calls. Labels must be nonempty and contain no whitespace, with a 1,000-byte limit.
Accepted labels use the inherited reference-label case normalization. Definition nesting must
remain below depth 100.

Unreferenced definitions are retained. Repeated calls share one definition by
label. For duplicate normalized labels, the first definition is the resolution
target; later definitions remain where they were written. No authored
footnote is discarded simply because it is unused or duplicated.

## Undefined calls

```markdown
An undefined call [^missing] stays in the paragraph.
```

With no matching definition, the brackets remain ordinary inline source and
allocate no footnote. An escaped or character-reference caret cannot open a
call. Link tails, resolving references, and attribute spans have earlier
[bracket precedence](links-and-images.md#bracket-precedence): `[^note](/url)`
is a link, and `[^note]{.class}` is a span.

## Inline footnotes

Write `^[body]` to define a note at its call site:

```markdown
A sentence.^[An inline note with *emphasis*.]
```

This produces a one-item `Cite` whose `Citation` owns the note: a `Footnote`
with a null label and parsed inline content. Unlike a referenced definition,
the inline body is not wrapped in a paragraph. The call and footnote cover the
same authored occurrence. `Document.footnotes` lists referenced and inline
definitions together in source order.

The body must contain something other than spaces/tabs. Nested brackets and
nested inline notes are allowed. Its closing bracket finishes the note without
claiming a following link or attribute tail: `^[note](url)` leaves `(url)` text.
At `^[`, inline-footnote recognition takes precedence over superscript.
An escaped caret does not open a note.

## Walking and resolution

Document traversal visits metadata when present, then content; a definition
is visited where it was written, and an inline note under its `Citation`.
Referenced calls store labels, never copied bodies. A footnote can refer to itself or another note
without creating an AST ownership cycle; a consumer that follows references
must handle semantic cycles itself. Code, formulas, comments, HTML tokens, and
cross links protect their bodies from footnote recognition.
