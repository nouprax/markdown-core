# Quotes and callouts

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Start a line with `>` to quote content. Keep the prefix on blank lines when
the blank belongs to the same quote.

```markdown
> A quoted paragraph.
>
> A second paragraph with **emphasis**.
```

The result is one `Callout` containing two paragraphs. For an ordinary quote,
`variant`, `collapsed`, and `title` are all null. There is no separate quote
node kind. Prefixes, indentation, lazy continuation, and nested quotes follow
CommonMark's block-quote rules.

## Add a type and title

Write `[!type]` on the quote's first line. Text after it becomes an optional
title, parsed as inline Markdown.

```markdown
> [!warning] Read **carefully**
> This is the body.
```

The callout stores `variant="warning"`; its title contains text and strong
emphasis, while its body contains a paragraph. The title is a separate
inline-node list, visited before the body. It is not a body paragraph.

Types contain one or more ASCII letters, digits, underscores, or hyphens.
They are preserved exactly, including case. `[!NOTE]` and custom types are
accepted, with no built-in list, alias mapping, icon, color, or generated title.

## Fold state

Put `+` or `-` immediately after the marker:

```markdown
> [!tip]+ Expanded initially
> Helpful text.

> [!warning]- Collapsed initially
> More detail.
```

The first callout has `collapsed=false`, the second `collapsed=true`. Without
a sign the field is null. The application decides how to implement folding.
A missing title remains null, rather than becoming the type's display name.

## Metadata boundaries

The metadata marker must be on the first quote line, after the quote prefix
and optional spacing, with at most three additional spaces. Four spaces,
a blank first line, earlier content, or an invalid type prevents metadata
recognition; the text remains ordinary quote content. A space, tab, or line
ending must follow the marker or fold sign. Trailing title spaces/tabs are
removed.

Nested quotes have their own first-line metadata. Code, lists, tables, and
other blocks can appear in the body. A block-identifier-looking suffix in a
title stays title text; a body paragraph can receive its own
[block identifier](block-identifiers.md).
