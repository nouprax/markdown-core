# Basics

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Separate paragraphs with a blank line. Text on consecutive nonblank lines
belongs to the same paragraph unless another block rule starts a new block.

```markdown
A short paragraph.
Still the same paragraph.

A second paragraph.
```

The result is two `Paragraph` nodes. The first contains text on either side of
a `SoftBreak`. An empty document has no content nodes. Spaces and tabs on an
otherwise empty line still count as a blank line.

## Indentation

Use indentation to continue content inside a list or another container. Tabs
advance to the next four-column stop when deciding block structure. Four
columns of indentation can start [code](code.md#indented-code); many other
block markers permit at most three leading columns.

The exact continuation column depends on the containing syntax. See
[lists](lists.md), [definition lists](definition-lists.md), and
[quotes](callouts.md) for examples.

## Escape punctuation

Put a backslash before ASCII punctuation to use it as text:

```markdown
\*literal asterisks\* and a\*b
```

The paragraph contains the text `*literal asterisks* and a*b`, with no emphasis.
A backslash before an ordinary letter remains a backslash. Escapes do not run
inside code or other literal bodies unless that syntax explicitly allows them.

## Character references

Named and numeric HTML character references decode in ordinary inline text:

```markdown
Fish &amp; chips: &#35;1, &#x41;.
```

The paragraph's text is `Fish & chips: #1, A.`. Decoded punctuation does not
start a second pass of Markdown parsing: `&#35;` does not create a heading.
Adjacent text pieces are merged into one `Text` node. Invalid references remain
ordinary text; code and raw HTML preserve their literal spelling.

## Literal punctuation

```markdown
"Quotes", 'apostrophes', --, ---, and ... stay as typed.
```

There is no automatic conversion to curly quotes, dashes, or an ellipsis.
Unicode characters, including emoji, are accepted as text; `:tada:` has no
emoji-shortcode meaning.

## Learn the block and inline forms

- [Headings](headings.md) and [thematic breaks](thematic-breaks.md) divide a document.
- [Emphasis](emphasis.md) and [line breaks](line-breaks.md) format paragraph content.
- [Code](code.md) preserves literal source.
- [HTML](html.md) explains where Markdown remains active around raw tags.
- [Links and images](links-and-images.md) introduce destinations and references.

These pages describe the CommonMark foundation directly. The
[full guide](../dialect.md) adds the remaining supported syntax.
