# Headings

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Start a line with one to six `#` characters and a space to create a heading.
The number of signs is its level.

```markdown
# Document title
## A section
### A smaller section
```

This produces three `Heading` nodes with levels 1, 2, and 3. Their visible
content excludes the signs. Each heading receives an [anchor](anchors.md),
such as `document-title`, for use by applications that support navigation.

An empty heading such as `#` is valid. A nonempty heading needs whitespace
after its opening signs: `#hashtag` remains paragraph text. Seven or more signs
do not open a heading. Up to three leading spaces are allowed.

## Underlined headings

Put `=` or `-` on a line below paragraph text to make a level-one or level-two
heading:

```markdown
Document title
==============

A section
---------
```

The result is two headings. These are called Setext headings. Their content
may span multiple lines; the underline itself is excluded from that content.
A valid Setext underline takes precedence over a thematic break or table.

## Format a heading

Heading content is parsed as inline Markdown. An optional closing run of `#`
signs, preceded by whitespace, is removed from an ATX heading.

```markdown
## A *small* `example` ## {#demo .compact}
```

The heading contains text, `Emphasis`, and `Code`. Its explicit anchor is `demo`
and its classes contain `compact`. The closing signs and
[attribute suffix](attributes.md#headings) are absent from visible content.
The scope still includes the complete authored heading.

See [anchors](anchors.md) for generated IDs, duplicate headings, and implicit
links to heading text.
