# Block identifiers

[Syntax guide](../dialect.md) · [Documentation](../README.md)

End a paragraph with `#id#` to give it an anchor.

```markdown
A paragraph worth linking to. #summary#
```

The paragraph has `anchor="summary"`. Its visible text excludes the marker
and separating whitespace; its source range still includes them. This dialect
uses `#id#` for declarations, rather than Obsidian's `^id` spelling.

IDs contain one or more ASCII letters, digits, or hyphens. Underscores,
Unicode letters, escapes, and empty IDs do not qualify.

## Paragraphs and list items

A marker must be the last non-space/tab content of the paragraph's final line,
preceded by a space or tab. It can also stand on a final line after earlier
paragraph text, indented by no more than three spaces; that removes the
preceding break along with the marker.

```markdown
- [x] Finish the review #reviewed#
```

A suffix on the list item's opening paragraph attaches to `ListItem`, giving
it anchor `reviewed`. A suffix on a later paragraph inside that item attaches
to that `Paragraph`. An item containing only the marker retains an empty
paragraph after removal.

## Lists, quotes, and tables

Place an identifier on its own line, separated by blank lines from the preceding
eligible block and any following content:

```markdown
- One
- Two

#steps#

Continue here.
```

The `List` receives anchor `steps`; the identifier creates no paragraph.
This standalone form also attaches to a preceding `Callout` or `Table` in the
same container. End of input can replace the following blank line.

The eligible block must be immediately adjacent across those blank lines.
A reference definition between it and the marker prevents attachment even
though the definition emits no AST node. A table caption can belong to the
table before its identifier attaches.

## Boundaries

Only a node without an anchor accepts a block identifier. Otherwise the marker
stays content. Headings use [attributes](attributes.md#headings), and a marker
in a callout title stays literal. An escaped opening hash prevents recognition.

Suffix recognition is a paragraph-level rule: inline code or an HTML comment
does not independently shield a final identifier candidate. Opaque block
content, including fenced code, does. An anchored paragraph containing only a
standalone formula stays a paragraph rather than becoming `FormulaBlock`.

Use `[[Note#^summary]]` or `[[#^summary]]` to refer to the block through a
[cross link](cross-links.md). The stored reference anchor is `summary`.
