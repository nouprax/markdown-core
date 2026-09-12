# Tables

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Markdown Core supports pipe, simple, multiline, and grid tables. All four
produce the same `Table` model: optional caption, column descriptions, and
head, body, and foot rows. The syntax controls cell content and geometry;
the AST does not store a separate table kind for each spelling.

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
Column relative widths are null, and every cell has span one.

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
whitespace, and escaped pipes are contracted before inline parsing.

## Captions

Put a caption paragraph before or after any supported table. Begin it with
`Table:`, `table:`, or `:`:

```markdown
Table: Fruit available **today**

| Item | Count |
| --- | ---: |
| Apples | 4 |
```

The `Table` owns a `TableCaption` with parsed inline content. The marker is
removed from its content but remains in its scope. A multiline caption can
contain soft breaks. An empty caption is valid.

Only blank lines may separate a caption from its table, and zero blank lines
are allowed where the table grammar has already ended. A preceding caption
wins; otherwise a following caption attaches. Between two tables, a caption
belongs to the earlier table if that table lacks a caption, otherwise it can
belong to the next. A caption without a table stays an ordinary paragraph.
The `:` form cannot be followed immediately by punctuation.

In a simple table, a caption-looking line remains a body row until a blank line
or footer ends the table. A complete caption/table candidate has precedence
over a definition list.

## Simple tables

Use aligned text columns separated by spaces, with dash runs under the headings:

```markdown
Name    Count
------  -----
Apples      4
Pears       2
------  -----
```

The dash runs define the column starts. This produces a two-column table with
inline content in its cells. Column alignment follows the header's placement
within its segment; relative widths are null and cell spans are one.

The header must be one line and the first line of its paragraph candidate.
Body rows continue until a blank line or a footer with the same dash-run
intervals. At least one body row or a footer is required. A footer ends the
table immediately, so prose or a caption can follow without a blank line.
Inside an already-open simple table, heading, quote, and fence-looking lines
remain inline cell text.

You can omit the header when a matching footer closes the table:

```markdown
------  -----
Apples      4
Pears       2
------  -----
```

This has an empty head group. Alignment is inferred from the first body line.
For alignment, a right-trimmed segment with leading space but no room on the
right is right-aligned; room on the right only means left; room on both means
center; neither, or an empty segment, means none. Text before the first dash
run belongs to the first column, and text after the last start belongs to the
last column. Segments are trimmed before inline parsing.

## Multiline tables

Use a full-width dash boundary and separate logical rows with blank lines:

```markdown
----------------
Left       Right
------- --------
a       b
c

d       e
----------------
```

The first body row's left cell contains a paragraph with `a`, a soft break,
and `c`; its right cell contains `b`. The second row contains `d` and `e`.
Header and body cells hold block content, including paragraph nodes. Column
width shares are 0.5 and 0.5 in this example.

A headed form begins with a full boundary, one or more header lines, and a
segmented dash boundary. A headerless form begins at the segmented boundary.
The closing full boundary must be followed by a blank line or end of input.
Body rows are separated by blank lines. A sole row immediately followed by the
closing boundary is retried as a simple table rather than accepted as this form.

Each physical line is cut at the segmented boundary's column starts. Segments
are right-trimmed, their shared leading indentation is removed, and they are
joined with LF for block parsing. Relative indentation within a cell survives.
All spans are one. Alignment uses the simple-table rule.

## Grid tables

Draw borders with `+`, `-`, and `|`. Use an `=` boundary to separate the head:

```markdown
+-------+-------+
| Name  | Count |
+=======+=======+
| Apple | 4     |
+-------+-------+
| Pear  | 2     |
+-------+-------+
```

This table has one head row and two body rows. Cells hold ordinary block
content, so these cells each contain a paragraph. Relative widths are 0.5 and
0.5. A grid cell can also contain code, a list, headings, or a nested table.

### Merged cells

Omit an interior wall to span columns:

```markdown
+---+---+
| a | b |
+===+===+
| c     |
+---+---+
```

The body row contains one cell with `colspan=2`. Omit a horizontal segment to
span rows. A cell is stored once, in the row where it starts, with `rowspan`
and `colspan`; covered coordinates do not receive placeholder cells.

An explicit boundary can start a row with no new cells:

```markdown
+---+---+
| a + b |
+---+---+
```

The middle `+` lies on a complete vertical edge. The result has two logical
rows and two cells with `rowspan=2`; the second row has `cells=[]`. That differs
from an authored empty cell, whose own `content` is empty.

### Boundaries and row groups

Column boundaries are connected `+` positions across the table's horizontal
border segments. Missing walls join adjacent regions, and each joined cell
must form one rectangle. Interior `+` characters do not become structural
unless they lie on a completed cell edge. Nonrectangular or inconsistent
geometry rejects the candidate as a whole.

The first eligible full `=` boundary separates a head. A final group enclosed
above and below by `=` boundaries is a foot. There is at most one of each;
other `=` boundaries invalidate the table. No cell spans across row groups.
Alignment comes from colons on the head separator, or the top border when
there is no head; colons elsewhere do not set it.

A candidate includes consecutive nonblank lines beginning with `+` or `|` at
its margin. A wrong-width marker-led line invalidates it. Separate following
prose beginning with either marker by a blank line so it cannot be mistaken
for another table line. Failure resumes ordinary block parsing at the first
line, without keeping a partial table or discarding trailing prose.

## Column widths and source positions

Columns count Unicode scalars after four-column tab expansion, not display
width. For multiline tables, each width runs from a dash run's start to the
next start, with the last run using its own length. For grids, it is the count
strictly between borders. A relative width is that count divided by their sum,
using double precision. Pipe and simple tables have no authored relative widths.

A table's scope includes its caption. Cells retain original document coordinates
even when their text is assembled from several line segments. A cell spanning
rows can extend below its owning row's range. See the
[coordinate contract](../canonical-ast.md#coordinates) for these precise cases,
and [typed table ownership](../canonical-ast.md#typed-table-ownership) for
walking and reconstructing occupied coordinates.

Tables, rows, cells, and captions accept no attribute suffix. A
[standalone block identifier](block-identifiers.md#lists-quotes-and-tables)
can attach an anchor to the table.

## Competing block syntax

Setext headings take precedence over table candidates. Complete simple,
multiline, and grid candidates take precedence over thematic breaks and
paragraphs. At a potential start, the forms are tried as grid, headed multiline,
simple, then headerless multiline; pipe tables are recognized at their delimiter
row. Dash lines that complete no table return to ordinary block rules. Code
and other opaque blocks retain table-looking source as literal content.
