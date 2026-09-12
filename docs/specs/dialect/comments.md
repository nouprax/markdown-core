# Comments

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Use `%%` or an HTML comment to add a comment. Both forms are retained in the
AST, so an application can display, hide, or inspect them.

```markdown
Visible %%private *note*%% text. <!-- another note -->
```

The paragraph contains two `Comment` nodes with embedded placement. Their
literals are `private *note*` and ` another note `; the asterisks inside the
first are not emphasis. Removing comments from rendered output is a consumer
choice, not a parse operation.

## Block comments

Put `%%` on opening and closing lines by themselves:

```markdown
%%
A note for editors.
**This stays literal.**
%%
```

The result is one standalone `Comment`. Fences may have zero to three leading
spaces after container prefixes and whitespace after the marker. The closing
fence must occur within the same container. Container prefixes are removed
from the body; its remaining line endings are preserved.

A complete block comment can interrupt a paragraph. An opening `%%` without
an eligible closing fence does not hide the rest of the document: normal
Markdown parsing resumes.

## HTML comments

Inline `<!-- ... -->` produces the same comment kind. At a block start, an
HTML-comment block becomes `Comment` only if the line ending its first `-->`
has nothing but whitespace after that closer. Otherwise the inherited HTML
block remains `HTMLBlock`. The short forms `<!-->` and `<!--->` are empty
comments under the inherited token rules.

## Boundaries

Percent comments end at the first later `%%`; they do not nest. Backslashes
inside a comment do not escape its closer. `%%%%` is an empty inline comment,
and `%%%a%%%` has comment literal `%a` followed by a literal `%`. An escaped
opening percent does not start a comment. Inline comments cannot cross block
boundaries.

In a pipe-table row, an unescaped pipe splits cells before comments are parsed.
Use `\|` to keep a pipe inside the cell's comment. Code, formulas, HTML tokens,
and complete cross links keep their own comment-looking bytes literal.
