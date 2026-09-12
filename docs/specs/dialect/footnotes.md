# Footnotes

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Write a call as `[^label]` and define its body with `[^label]:`.

```markdown
A statement with a note.[^source]

[^source]: The supporting detail.
```

The paragraph contains a `Cite` with a footnote reference to ID `source`.
The definition becomes a document-owned `Footnote`; it is stored in
`Document.footnotes`, not in the paragraph or the document's ordinary content.
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
ID. For duplicate normalized IDs, the first definition is the resolution target;
later definitions remain in the collection in source order. No authored
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

This produces a one-item `Cite` and a `Footnote` with parsed inline content.
Unlike a referenced definition, the inline body is not wrapped in a paragraph.
The call and footnote cover the same authored occurrence. The collection holds
referenced and inline definitions together in source order.

The body must contain something other than spaces/tabs. Nested brackets and
nested inline notes are allowed. Its closing bracket finishes the note without
claiming a following link or attribute tail: `^[note](url)` leaves `(url)` text.
At `^[`, inline-footnote recognition takes precedence over superscript.
An escaped caret does not open a note.

Inline notes receive generated IDs `inline-N` in opening-position order, with
an outer note before its nested notes. If that ID is already authored, the
smallest free `inline-N-K` suffix is used. Applications should treat these as
opaque identities, not display numbers.

## Walking and resolution

Document traversal visits content, then footnotes, then specimens. Calls store
ID edges, never copied bodies. A footnote can refer to itself or another note
without creating an AST ownership cycle; a consumer that follows references
must handle semantic cycles itself. Code, formulas, comments, HTML tokens, and
cross links protect their bodies from footnote recognition.
