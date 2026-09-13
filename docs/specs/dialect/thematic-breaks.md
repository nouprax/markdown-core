# Thematic breaks

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Put three or more matching `-`, `*`, or `_` characters on a line to separate
sections. Spaces and tabs may appear between the characters.

```markdown
Before the break.

---

After the break.

* * *

___
```

Each separator becomes a `ThematicBreak`, with no inline content. Up to three
leading columns of indentation are allowed. Mixed characters or other text
on the line do not make a thematic break.

A dash line immediately below paragraph text can instead be a
[Setext heading](headings.md#underlined-headings). A complete
[table](tables.md) can also claim dash boundaries. At the beginning of a
document, a complete `---` [properties envelope](properties.md) takes precedence.
Blank lines help make the intended separation clear.
