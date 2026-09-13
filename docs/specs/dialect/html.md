# HTML

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Raw HTML can appear inside a paragraph or as a block. The parser retains its
source; it does not build a DOM or render HTML.

```markdown
A <span>**bold**</span> word.
```

The tags become separate `HTML` nodes. Between them, `**bold**` becomes `Strong`.
A pair of inline tags does not turn its intervening content into a literal region.

## HTML blocks

```markdown
<div>
**literal Markdown**
</div>
```

The complete block becomes `HTMLBlock`; its body is not parsed as Markdown.
CommonMark's HTML-block rules decide where it ends. In particular, `script`,
`pre`, `style`, and `textarea` blocks end at their corresponding closing tag;
ordinary block-tag forms generally end at a blank line. Processing
instructions, declarations, CDATA, and complete standalone tags use the
corresponding CommonMark rules. Some standalone-tag forms cannot interrupt a
paragraph.

Invalid tag syntax falls back to ordinary inline or block parsing. HTML tokens
and blocks keep their bytes as written, including character references. If an
application renders raw HTML, its rendering and sanitization policies are
outside the parser.

## Comments

HTML comments produce retained `Comment` nodes under the
[comment rules](comments.md), including the block boundary distinction when
other text follows a closing `-->`.
