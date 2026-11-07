# Tables

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Markdown Core supports pipe tables, as GitHub Flavored Markdown writes them.
A table produces a `Table`: its column descriptions, one head row, and its
body rows. Pandoc's simple, multiline and grid tables and table captions are
not part of the dialect; their source is ordinary Markdown.

## Pipe tables

Use pipes to separate columns and a dash row below the header:

```markdown
| Item | Count | Status |
| :--- | ---: | :---: |
| Apples | 4 | **Ready** |
| Pears | 2 | Pending |
```

This produces a three-column table with one header row and two body rows.
The columns are left-, right-, and center-aligned. Cells contain parsed inline
content, so `Ready` is strong text. Without colons, alignment is `none`.

Outer pipes are optional. Each delimiter cell needs at least one dash, and
the header and delimiter must have the same number of cells. The header is
the last line of an open paragraph; earlier paragraph lines remain prose.

Short body rows gain empty cells; excess cells are dropped. A nonblank line
without a pipe can become a one-cell body row. A blank line or an eligible
interrupting block start ends the table. Rows have a 65,535-cell limit, and
the [global limits](../dialect.md#limits) bound filled-in empty cells.

### Pipes inside cells

Escape a pipe that belongs to cell content, including code or a cross-link label:

```markdown
| Code | Link |
| --- | --- |
| `a \| b` | [[Note\|Read more]] |
```

The code literal contains `a | b` and the cross link has label `Read more`.
Cell splitting happens before inline parsing; an unescaped pipe splits a cell
even inside a comment or cross-link candidate. Cell content trims surrounding
whitespace, and escaped pipes are contracted before inline parsing. Every
other byte stays as written, a tab included, in the header and body rows
alike.

## Source positions

A pipe row has no shared geometry and begins at its own first non-space
character. A table begins at its head row; the delimiter row is part of the
table but of no row. Cells retain original document coordinates. See the
[coordinate contract](../canonical-ast.md#coordinates) for these precise cases,
and [typed table ownership](../canonical-ast.md#typed-table-ownership) for
walking a table's rows.

Tables, rows, and cells accept no attribute suffix. A
[standalone block identifier](block-identifiers.md#lists-quotes-and-tables)
can attach an anchor to the table.

## Competing block syntax

Setext headings take precedence over table candidates. A pipe table is
recognized at its delimiter row, which makes the last line of the open
paragraph its head row. Code and other opaque blocks retain table-looking
source as literal content.
