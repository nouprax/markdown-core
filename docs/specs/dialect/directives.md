# Directives

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Use a named directive to attach application-defined meaning to inline or block
content. The parser records the name, label, attributes, and body; it does not
execute or render the directive.

```markdown
A :badge[new]{.status level=3} release.
```

This produces an inline `Directive` named `badge`, with a label containing
`new`, class `status`, and record `level="3"`. Its `DirectiveLabel` is a separate
owned node, not an ordinary content child.

## Inline directives

A name is followed immediately by a bracketed label, an [attribute
container](attributes.md), or both; the part is what makes the colon a
directive:

```markdown
:name[label] :name{.class} :name[label]{.class}
```

A name is a string without spaces, taken as written: one or more bytes other
than a space, a tab, a line ending, `[`, `{`, or `:`. Nothing is classified by
Unicode category, so `:中文[中文]`, `:1a[x]` and `:a.b[x]` name `中文`, `1a`
and `a.b`. Case is preserved. A name with no part after it is text -- `:name`
alone, `12:30`, `http://x` -- and so is a colon adjacent to another colon, so
`:emoji:` has no directive meaning and `x ::a[y]` is text. `a:b[x]` contains a
directive at the colon.

Labels parse inline Markdown, can balance nested brackets, and may span soft
line breaks within one inline container. A backslash escapes the next byte
while locating the label's closer. Nesting beyond 32 brackets fails the label.
A part that fails to scan does not anchor the directive: with neither part
valid the colon is text. Once one part is valid the name commits, and an
invalid second part leaves the directive without it and resumes ordinary
parsing at the failed opener.

## Leaf directives

Use two colons on a line of their own:

```markdown
::video[Introduction]{src=intro.mp4}
```

The result is `DirectiveBlock` named `video`, with a label and attributes but
empty block content. The name follows the inline rule; the block forms need no
bracket part, since the fence anchors them. A space cannot separate the name
from its label or attributes. Only spaces/tabs may follow the accepted parts on that line.

## Container directives

Use three or more colons to open a named container:

```markdown
:::note[Remember]{.important}
A paragraph with **emphasis**.

- An item
:::
```

The `DirectiveBlock` contains a paragraph and a list. Its label is separate
from its block content. Named and nameless containers use the same body parser.

The closer is a line with at least as many colons as the opener and no other
non-whitespace content. A shorter closer stays body content. Use a longer
outer fence when nesting containers:

```markdown
::::outer
:::inner
Content.
:::
::::
```

An unclosed container runs through the end of its enclosing container or the
document. Opener lines permit up to three leading spaces, but no tab in the
leading indentation. Labels and attribute containers must close on the opener
line. Leaf and container openers can interrupt paragraphs.

## Nameless containers

Put an attribute container or one unbraced class word after a colon fence:

```markdown
::: {.warning}
Check this before continuing.
:::

::: compact
A compact section.
:::
```

These produce `DirectiveBlock` with `name=null`, `label=null`, and a class of
`warning` or `compact`. This is also known as a fenced div. Optional trailing
colons are allowed on the opening line. The unbraced word is one raw class;
`::: -` gives class `-`, whereas `::: {-}` gives `unnumbered`.

A bare `:::` line opens no container. `::: {}` is a valid empty-attribute opener.
Immediately following a fence with a name, as in `:::warning`, selects the
named form. Nameless containers use the same minimum closer length as named
containers, including when nested.

## Container boundaries

Closing fences inside code or other opaque blocks are body text. Quote and
list prefixes follow ordinary continuation rules; a missing outer prefix does
not close a paragraph that can continue lazily. Malformed opener lines return
to the normal block parser. Inline directive-looking text inside code, comments,
formulas, HTML tokens, or cross links remains literal.
