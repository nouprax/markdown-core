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

The name may be followed immediately by an optional bracketed label, then an
optional [attribute container](attributes.md):

```markdown
:name :name[label] :name{.class}
```

All three are directives. Names begin with a Unicode letter and continue with
letters, numbers, combining marks, hyphens, or underscores; a name cannot end
with a hyphen or underscore. Case is preserved. `12:30` is text, while `a:b`
contains a directive at the colon. Adjacent colons or a colon immediately after
the name prevent the inline form, so `:emoji:` has no directive meaning.

Labels parse inline Markdown, can balance nested brackets, and may span soft
line breaks within one inline container. A backslash escapes the next byte
while locating the label's closer. Nesting beyond 32 brackets fails the label.
The name commits independently: an incomplete label or invalid attribute
container leaves the directive without that part and resumes ordinary parsing
at the failed opener.

## Leaf directives

Use two colons on a line of their own:

```markdown
::video[Introduction]{src=intro.mp4}
```

The result is `DirectiveBlock` named `video`, with a label and attributes but
empty block content. A space cannot separate the name from its label or
attributes. Only spaces/tabs may follow the accepted parts on that line.

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
