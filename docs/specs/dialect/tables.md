# Tables

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the one table model and every table syntax. Options: `tables` (default
`true`) for pipe tables; `tableCaptions`, `simpleTables`, `multilineTables`,
and `gridTables` (each default `false`), every one independent of the others.
Sources: cmark-gfm's table extension; Pandoc's `table_captions`,
`simple_tables`, `multiline_tables`, and `grid_tables`. Executable oracles:
cmark-gfm for pipe tables; the Pandoc 3.11 CLI for the other forms. Landing:
the model with `M6`; captions with `P11a`; simple, multiline, and grid tables
with `P11b`, `P11c`, and `P11d`. Until `M6` the current contract's `Table`,
`TableRow`, and `TableCell` stand.

## Model

```text
Table(caption: TableCaption?, columns: [TableColumn], head: [TableRow],
      content: [TableRow], foot: [TableRow])
TableCaption(content: [Markup])
TableColumn(alignment: none | left | center | right, relative: Double?)
TableRow(cells: [TableCell])
TableCell(rowspan: Int, colspan: Int, content: [Markup])
```

`Table`, `TableCaption`, `TableRow`, and `TableCell` are `Markup` kinds
reached through typed fields; `TableColumn` is a value with no scope.
`columns` is non-empty and defines the logical grid. `relative`, when
non-null, is a positive share derived from authored column widths as stated
per syntax; `null` means no width was authored, and the dump prints a double
as the shortest round-trip decimal. `head`, `content`, and `foot` may be
empty. Every span is at least one. Source syntax never selects a different
kind, and no field records which syntax produced a table.

`TableCell.content` is `[Markup]`: pipe and simple cells store their parsed
inline content directly, multiline and grid cells store the block sequence
the block parser produces from the cell text, and no cell is wrapped in or
unwrapped from a `Paragraph`. `TableCaption.content` is the caption
paragraph's parsed inline content. Tables, rows, cells, and captions receive
no attributes and no anchor from any attribute rule; a
[block identifier](block-identifiers.md) may populate `Table.anchor`.

### Logical grid

`TableRow.cells` holds the cells whose upper-left coordinate starts in that
row; a cell with `rowspan > 1` is owned once by that row and occupies later
rows without a placeholder. A consumer recovers every coordinate by processing
the rows of `head`, then `content`, then `foot`, top to bottom, with an
occupancy array of `columns.count` entries:

1. At the start of a row, positions retained by earlier cells with remaining
   `rowspan` are occupied.
2. Visit `cells` in order: advance past occupied positions to the first free
   column, place the cell there, and require its `colspan`-wide interval to be
   free and within the grid.
3. Mark that interval occupied for this row and the next `rowspan - 1` rows.
4. After the row, every column must be occupied; an empty `cells` array is
   valid only when incoming spans cover the whole row.
5. After a row group, no span may remain active; a cell never crosses a
   `head`, `content`, or `foot` boundary.

A candidate that would violate any step is not a table and follows the
syntax's fallback; the parser never emits a table that needs repair.

## Pipe tables

With `tables=true`, block-start step 11 opens a pipe table when the current
line is a delimiter row and a paragraph is open:

```text
delimiter-row = [ "|" ] marker *( "|" marker ) [ "|" ] *WSP EOL
marker        = *WSP [ ":" ] 1*"-" [ ":" ] *WSP
row           = [ "|" ] cell *( "|" cell ) [ "|" ] *WSP EOL
cell          = *( escaped-pipe / any scalar except "|", LF, and CR )
escaped-pipe  = "\|"
```

The header row is the last line of the open paragraph, and the table opens
only when that line parses as a row with exactly as many cells as the
delimiter row has markers. Earlier lines of the paragraph become a separate
`Paragraph` that keeps its authored spelling. Column alignment comes from the
markers' colons: left, right, both, or none. Every following line is a body
row until a blank line or a line that a block start of steps 1 through 10,
12, or 13 claims; a line without pipes is a one-cell row. In every row, a
`\|` becomes `|` before inline parsing, inside code spans included, and each
cell's content is trimmed of leading and trailing whitespace and parsed as
inline content. A row with fewer cells than the delimiter row is completed
with empty cells whose scope is the row's end; excess cells are dropped, so
every row has `columns.count` cells. A table that has completed more than
524288 cells accepts no further rows.

A pipe table produces `columns` with the delimiter row's alignments and
`relative=null`, `head` with the one header row, `content` with the body
rows, `foot=[]`, and cells with both spans equal to one. With `tables=false`,
the lines are paragraph text.

## Captions

With `tableCaptions=true`, a caption line is:

```text
caption-line = *3SP ( "Table:" / "table:" / ":" ) rest
```

where for the `:` form the next scalar is not punctuation, the word forms are
case-sensitive, and `rest` may be empty. A caption paragraph is one whose
first line is a caption line; its content is the paragraph's parsed inline
content after removing the marker and the whitespace after it, so a
multi-line caption contributes `SoftBreak` nodes and an empty marker gives
`TableCaption(content=[])`.

A caption paragraph is claimed by a table of any syntax that it precedes or
follows with zero or more blank lines and nothing else between. A preceding
caption is a table-candidate block start parsed in one lookahead with the
table after it; when no table follows, its bytes are released to paragraph
parsing. A following caption is claimed after the table is complete. When
both surround one table, the preceding caption owns it and the following
paragraph stays a paragraph; a caption paragraph between two tables belongs
to the preceding one. Placement is not stored. With the option off, no
paragraph is claimed. A caption line is tested before the definition-list
step, as the [definition lists](definition-lists.md) module states.

## Column arithmetic

All column positions in the syntaxes below count Unicode scalars of the line
after tab expansion to four-column stops; there is no display-width
computation. Where widths are authored, `relative[i]` is `w[i] / sum(w)` in
IEEE-754 double division with no other constant, and `w[i]` is stated per
syntax.

## Simple tables

With `simpleTables=true`:

```text
separator = *3SP dash-run *( 1*SP dash-run ) *SP EOL
dash-run  = 1*"-"
```

The header line is the nonblank line immediately before the separator and
must be the first line of a paragraph candidate. Body rows are every following
line until a blank line, or until a footer line of the same shape as the
separator followed by a blank line or the end of the document; at least one
body row or a footer is required. The header may be omitted when a footer
closes the table; then `head=[]`, and column boundaries and alignment are
inferred from the separator runs and the first body line.

Columns are cut at the start position of each dash run: bytes before the
first run belong to column one, bytes from the last run's start to the end
of the line belong to the last column, and each segment is trimmed and
parsed as inline content. Alignment: with the header's segment right-trimmed,
let `leftSpace` mean the segment begins with a space or tab and `rightSpace`
mean its length is less than the dash run's; `(true, false)` is `right`,
`(false, true)` is `left`, `(true, true)` is `center`, and `(false, false)`
or an empty segment is `none`. A multi-line header uses the shortest
non-empty line's segment; a headerless table uses the first body line.
`relative` is `null` for every column, and every span is one.

## Multiline tables

With `multilineTables=true`:

```text
full-boundary = *3SP 3*"-" *SP EOL
```

A full-width boundary is one dash run; the segment boundary is the
simple-table separator. With a header, the table is a full boundary, the
header block of every nonblank line up to the segment boundary, the segment
boundary, the body, and a full boundary followed by a blank line or the end
of the document. Without a header it begins at the segment boundary and
`head=[]`. Body rows are separated by blank lines, and the last row may be
followed directly by the closing boundary unless it is the only row, in which
case the candidate is retried as a simple table and otherwise follows the
inherited fallback. A row's physical lines are cut at the segment boundary's
column positions; each cell's per-line segments are joined with LF and parsed
as a block sequence. `w[i]` is the scalar count from the start of dash run
`i` to the start of run `i+1`, the last run being its own length. Alignment
follows the simple-table rule, and every span is one.

## Grid tables

With `gridTables=true`, a grid table's lines begin and end with `|` or `+` at
the table margin, and the column boundary set is the union of the `+`
positions on every horizontal boundary line; every `+` and `|` must sit at a
boundary position or the candidate fails. Between adjacent boundary positions
a segment is horizontal (all `-` or all `=`, with optional edge colons) or
cell text. A cell anchored at row `r` and column `c` spans right until a `|`
or `+` at a boundary position and down until the first line on which its
full width is horizontal; the result is `colspan` and `rowspan` stored once
in the anchor row, and the logical grid rules above validate the result.

A line whose segments are all `=` is a head separator when it is the first
such line and there is no foot yet, and the foot is the final row group
enclosed above and below by `=` lines; at most one head separator and at
most one foot exist, and any other `=` line fails recognition. With a head
separator only its colons select alignment; without one the top line's do,
and colons elsewhere are ignored. `w[i]` is the scalar count strictly between
the column's boundary positions. Cell text is, per line, the scalars strictly
between the cell's boundary positions, right-trimmed; if every non-empty line
begins with a space, one space is removed from each; the lines are joined
with LF and parsed by the block parser, so cells hold paragraphs, code, lists,
headings, nested tables, and every enabled block.

## Block-start order and fallback

At one block start, table candidates are tried as: grid when the line begins
with `+`, then multiline with a header, then simple, then headerless
multiline; a pipe table is decided by the inherited rule at the delimiter row.
A Setext heading beats every candidate: a single dash run without internal
whitespace that the inherited grammar reads as an underline is an underline.
A complete simple, multiline, or grid candidate beats a thematic break and a
paragraph; a dash line that completes no candidate is a thematic break; a
code fence is never claimed. Each line is scanned at most twice. Multiline
and grid candidates commit only after a valid opening structure establishes a
rectangular grid, and a malformed or nonrectangular candidate restarts
inherited block parsing at its first line with no partial table. Code and
other opaque blocks suppress recognition. A `\|` inside a cell of any syntax
follows the [cross links](cross-links.md) rule while `crossLinks` is on, and
the [comments](comments.md) rule states how comments interact with cell
boundaries.

## Scopes

`Table.scope` spans the caption, when one is claimed, and the table.
`TableCaption.scope` includes its marker. Rows and cells cover their authored
bytes; a completed pipe cell's scope is the row's end. A multiline or grid
cell, and its descendants, use original-source coordinates from the first
scalar of its first segment to the last scalar of its last segment, a range
that may include other cells' bytes; `canonical-ast.md` records this
exception to the contiguous-range rule.

## Required conformance cases

Tests cover, for pipe tables, the delimiter grammar, optional outer pipes,
two-hyphen and aligned markers, header splitting from a multi-line paragraph,
rows without pipes, every interrupting block start, escaped pipes inside and
outside code spans, short and long rows, and the cell limit; for captions,
before, after, both, between two tables, multi-line, empty, punctuation after
the colon, definition-list precedence, and the option off; for simple tables,
every alignment, closing separators, headerless forms, and Setext and
thematic-break precedence; for multiline tables, rows, widths, the one-row
rule, and headerless forms; for grid tables, multi-row heads, block cell
content, row and column spans, interleaved active spans, fully covered rows,
alignment, foot, and rejection of overlap, overrun, uncovered coordinates,
cross-group spans, and stray `=` lines; and for all, exact table, caption,
row, and cell scopes, each option independently on and off, allocation
failure, deep nested cells, and size-doubling rows, columns, and boundaries.
