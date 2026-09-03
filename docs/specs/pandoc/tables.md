# Pandoc simple, multiline, and grid tables

Status: normative target module for `simple_tables`, `multiline_tables`,
`grid_tables`, and `table_captions`. Authority: the Pandoc User's Guide
sections for
[table captions](https://pandoc.org/MANUAL.html#extension-table_captions),
[simple tables](https://pandoc.org/MANUAL.html#extension-simple_tables),
[multiline tables](https://pandoc.org/MANUAL.html#extension-multiline_tables),
and [grid tables](https://pandoc.org/MANUAL.html#extension-grid_tables), at the
snapshot pinned by the [Pandoc extension index](../pandoc.md).

## One table model

All table source syntaxes, including inherited pipe tables, project to one
consumer model:

```text
Table(
  caption: TableCaption?,
  columns: [TableColumn],
  head: [TableRow],
  content: [TableRow],
  foot: [TableRow],
  scope: Scope
)

TableCaption(
  content: [Markup],
  scope: Scope
)

TableColumn(
  alignment: none | left | center | right,
  relative: Double?
)

TableRow(
  cells: [TableCell],
  scope: Scope
)

TableCell(
  rowspan: Int,
  colspan: Int,
  content: [Markup],
  scope: Scope
)
```

`TableColumn` is a non-node value and has no scope. Every row and cell is
`Markup`. Cell content is block content for every syntax; a simple inline cell
is normalized to one paragraph. This uniformity is required because grid cells
may contain arbitrary blocks.

`TableCaption.content` is block content. The caption syntax in this module
contributes exactly one paragraph-like block, even when that paragraph spans
multiple physical source lines.

`columns` is non-empty and defines the logical grid. `relative`, when present,
is a positive normalized share derived from authored column widths; null means
no width was authored. `head`, `content`, and `foot` may be empty. Every span
is at least one, and the rectangular occupancy obtained after applying spans
must equal `columns.count` in every logical row.

Source syntax does not select a different AST kind. A parser may remember the
syntax internally for diagnostics, but it is not a consumer field.

## Logical grid ownership

`TableRow.cells` contains the cells whose upper-left logical coordinate starts
in that row. Row membership identifies a cell's anchor; it does not limit the
cell's extent. A cell with `rowspan > 1` remains owned exactly once by its
anchor row while occupying columns in subsequent rows. Those rows contain no
copy or placeholder for it. This is the same ownership relation as an HTML
`td` or `th` that remains a child of its originating `tr` while spanning later
rows.

Consumers recover every cell's logical coordinate by processing the rows of
each of `head`, `content`, and `foot` from top to bottom with a
`columns.count`-sized occupancy array:

1. At the start of a row, positions retained by earlier cells with remaining
   `rowspan` are already occupied.
2. Visit `TableRow.cells` in order. Advance past occupied positions to the
   first free column, place the cell there, and require the complete
   `colspan`-wide interval to be free and within the grid.
3. Mark that interval occupied for the current row and the next
   `rowspan - 1` rows. Continue after the placed interval; later cells may
   advance across active spans.
4. After the row, every logical column must be occupied. An empty `cells`
   collection is valid only when incoming row spans cover the complete row.
5. After the row group, no active span may remain. A cell cannot cross a
   `head`/`content`/`foot` boundary.

For example, this is a three-column table in which `B` occupies the middle
column of both rows; string literals abbreviate each cell's block content and
scopes are omitted:

```text
content: [
  TableRow(cells: [
    TableCell(rowspan: 1, colspan: 1, content: ["A"]),
    TableCell(rowspan: 2, colspan: 1, content: ["B"]),
    TableCell(rowspan: 1, colspan: 1, content: ["C"])
  ]),
  TableRow(cells: [
    TableCell(rowspan: 1, colspan: 1, content: ["D"]),
    TableCell(rowspan: 1, colspan: 1, content: ["E"])
  ])
]
```

On the second row, `D` is placed in column one, column two is skipped because
`B` still occupies it, and `E` is placed in column three. Thus interleaving is
fully represented without making a cell belong to multiple rows. An overrun,
overlap, uncovered coordinate, or span crossing a row-group boundary is an
invalid `Table` rather than a request for consumer repair.

## Table captions

With `table_captions=true`, a paragraph beginning exactly with `Table:`,
`table:`, or `:` followed by caption content may occur immediately before or
after any supported table. The marker is removed and the remaining paragraph
becomes `TableCaption.content`; a continued source line remains inline caption
content under ordinary paragraph rules.

The nearest eligible preceding caption is claimed before a following caption.
If both surround one table, the preceding caption owns it and the following
candidate remains ordinary paragraph content. Caption placement is authoring
syntax and is not stored. `Table.scope` spans the caption and table; the
caption's own scope includes its stripped marker.

With the option disabled, the paragraph is not claimed. An empty marker may
produce an empty caption; absence remains `caption=null`.

## Simple tables

A simple table uses one line per header/body row. One separator line contains
one dash run per column and establishes column boundaries. Header text is the
line immediately above that separator; body lines follow it. The table ends at
a blank line or at a matching closing separator followed by a blank line.

Header alignment relative to its separator run determines the column value:

- flush right and extended on the left: `right`;
- flush left and extended on the right: `left`;
- extended on both sides: `center`;
- flush on both sides: `none`.

The header may be omitted when a matching separator closes the table. In that
form, column boundaries and alignment are inferred using the first body line
and the separator runs, and `head=[]`. Simple tables never create spans other
than one.

## Multiline tables

Multiline tables use fixed column boundaries but allow one logical header or
body row to occupy multiple physical lines. Unless the header is omitted, a
full-width dash boundary begins the table before header content and a
column-segment dash boundary separates head from body. A full-width dash
boundary and then a blank line are required at the end.

Body rows are separated by blank lines. Physical lines within one row are
combined into the corresponding cells using the established column ranges.
Relative source widths populate `TableColumn.relative`. Row/column spans are
unsupported and remain one. A one-row multiline table requires the blank row
separator before its closing boundary so it cannot be mistaken for a simple
table.

The headerless form starts with the segmented column boundary and yields an
empty head. Multiline caption paragraphs may themselves continue across source
lines.

## Grid tables

A grid table's outer and internal boundaries are drawn with `+`, `-`, `=`, and
`|`. The top boundary establishes every vertical column boundary. A separator
containing `=` distinguishes head from body and may be omitted for a headerless
table. Multiple logical rows above it produce multiple head rows.

Cells may contain arbitrary block content: paragraphs, code blocks, lists,
headings, nested tables, and any enabled extension. Missing internal vertical
or horizontal boundary segments create `colspan > 1` or `rowspan > 1`;
spans are represented directly rather than by placeholder cells.

Colons at the edges of the header separator select column alignment using the
same left/right/both rule as pipe tables. For a headerless grid table, these
colons occur on the top boundary. Column widths populate `relative`.

A table foot is the final row group enclosed above and below by `=` boundaries.
It must be at the bottom and populates `foot`; equal-sign sections elsewhere
retain their head/body meaning or fail table recognition.

## Precedence, fallback, and complexity

Block classification must distinguish table boundaries from thematic breaks,
Setext headings, fenced code, and ordinary punctuation before committing.
Simple-table recognition cannot steal a thematic break without a complete
column/row shape. Multiline and grid candidates commit only after a valid
opening structure establishes a rectangular grid.

Malformed or nonrectangular candidates follow inherited block fallback without
partial table nodes. Code and other opaque block bodies suppress table
recognition. Nested grid-cell blocks use the normal block parser and nesting
limit.

Boundary discovery and cell extraction are linear in source bytes plus emitted
cells. Span occupancy may use a row-sized index but may not repeatedly rescan
prior rows. Allocation failure aborts the whole table.

## Required conformance cases

Tests must cover each syntax with and without head/caption; all alignments;
simple closing separators; multiline rows, widths, and one-row ambiguity;
grid multirow heads, block cell content, row/column spans, interleaved active
row spans, fully covered rows, alignment, and foot; rejection of overlap,
overrun, uncovered coordinates, and cross-group spans; caption
before/after/both and multiline/empty captions; coexistence with pipe tables;
thematic-break/Setext/code precedence; malformed grids; exact table, caption,
row, and cell scopes; option independence; allocation failure; deep nested
cells; and size-doubling rows, columns, and boundaries.
