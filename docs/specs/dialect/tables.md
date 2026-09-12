# Tables

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the one table model and every table syntax. Sources: cmark-gfm's table extension; Pandoc's `table_captions`,
`simple_tables`, `multiline_tables`, and `grid_tables`. Executable oracles:
cmark-gfm for pipe tables; the Pandoc 3.11 CLI for the other forms. Landing:
the model with `M6`; captions with `P11a`; simple, multiline, and grid tables
with `P11b`, `P11c`, and `P11d`. All four table forms and captions are implemented on every public surface. The
[example format](../dialect.md#examples) is defined by the index.

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
per syntax; `null` means no width was authored. `head`, `content`, and
`foot` may be empty. Every span is at least one. Source syntax never selects
a different kind, and no field records which syntax produced a table. In the
dump, `columns` prints as `[alignment:relative,...]`, a non-null caption
prints as a nested `TableCaption` line, and the three row groups print as
`TableHead`, `TableBody`, and `TableFoot` group lines whose rows
`Table.children` counts.

`TableCell.content` is `[Markup]`: pipe and simple cells store their parsed
inline content directly, multiline and grid cells store the block sequence
the block parser produces from the cell text, and no cell is wrapped in or
unwrapped from a `Paragraph`. `TableCaption.content` is the caption
paragraph's parsed inline content. Tables, rows, cells, and captions receive
no attributes and no anchor from any attribute rule; a
[block identifier](block-identifiers.md) may populate `Table.anchor`.

```````````````````````````````` example
| Left | Center | Right | None |
| :--- | :----: | ----: | ---- |
| a | *b* | `c` | d |
.
Document scope=1:1..3:21 anchor=null attributes={} children=1
└── Table scope=1:1..3:21 anchor=null attributes={} columns=[left:null,center:null,right:null,none:null] children=2
    ├── TableHead children=1
    │   └── TableRow scope=1:1..1:32 anchor=null attributes={} children=4
    │       ├── TableCell scope=1:2..1:7 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=1:3..1:6 anchor=null attributes={} literal="Left" children=0
    │       ├── TableCell scope=1:9..1:16 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=1:10..1:15 anchor=null attributes={} literal="Center" children=0
    │       ├── TableCell scope=1:18..1:24 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=1:19..1:23 anchor=null attributes={} literal="Right" children=0
    │       └── TableCell scope=1:26..1:31 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=1:27..1:30 anchor=null attributes={} literal="None" children=0
    ├── TableBody children=1
    │   └── TableRow scope=3:1..3:21 anchor=null attributes={} children=4
    │       ├── TableCell scope=3:2..3:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=3:3..3:3 anchor=null attributes={} literal="a" children=0
    │       ├── TableCell scope=3:6..3:10 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Emphasis scope=3:7..3:9 anchor=null attributes={} children=1
    │       │       └── Text scope=3:8..3:8 anchor=null attributes={} literal="b" children=0
    │       ├── TableCell scope=3:12..3:16 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Code scope=3:13..3:15 anchor=null attributes={} literal="c" children=0
    │       └── TableCell scope=3:18..3:20 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=3:19..3:19 anchor=null attributes={} literal="d" children=0
    └── TableFoot children=0
````````````````````````````````

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
The parser records source-defined rows and each starting cell once, with its
authored spans and content. It does not expand spans into a dense grid, insert
placeholder cells at covered coordinates, or synthesize rows for layout.
An explicit source boundary can define a row with no starting cells; that row
is retained with `cells=[]`. This differs from an authored empty cell, which
is a `TableCell` with empty `content`. Reconstructing occupied coordinates and
laying out the table are consumer responsibilities; temporary geometry used
to validate the source candidate does not become another public table model.

## Pipe tables

Block-start step 10 opens a pipe table when the current
line is a delimiter row and a paragraph is open:

```text
delimiter-row = [ "|" ] marker *( "|" marker ) [ "|" ] *WSP EOL
marker        = *WSP [ ":" ] 1*"-" [ ":" ] *WSP
row           = [ "|" ] cell *( "|" cell ) [ "|" ] *WSP EOL
cell          = *( escaped-pipe / any scalar except "|", LF, and CR )
escaped-pipe  = "\|"
```

The outer pipes are optional:

```````````````````````````````` example
a | b
--|--
c | d
.
Document scope=1:1..3:5 anchor=null attributes={} children=1
└── Table scope=1:1..3:5 anchor=null attributes={} columns=[none:null,none:null] children=2
    ├── TableHead children=1
    │   └── TableRow scope=1:1..1:5 anchor=null attributes={} children=2
    │       ├── TableCell scope=1:1..1:2 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=1:1..1:1 anchor=null attributes={} literal="a" children=0
    │       └── TableCell scope=1:4..1:5 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=1:5..1:5 anchor=null attributes={} literal="b" children=0
    ├── TableBody children=1
    │   └── TableRow scope=3:1..3:5 anchor=null attributes={} children=2
    │       ├── TableCell scope=3:1..3:2 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=3:1..3:1 anchor=null attributes={} literal="c" children=0
    │       └── TableCell scope=3:4..3:5 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=3:5..3:5 anchor=null attributes={} literal="d" children=0
    └── TableFoot children=0
````````````````````````````````

The header row is the last line of the open paragraph, and the table opens
only when that line parses as a row with exactly as many cells as the
delimiter row has markers. Earlier lines of the paragraph become a separate
`Paragraph` that keeps its authored spelling:

```````````````````````````````` example
para
line
a | b
-|-
c | d
.
Document scope=1:1..5:5 anchor=null attributes={} children=2
├── Paragraph scope=1:1..2:4 anchor=null attributes={} children=3
│   ├── Text scope=1:1..1:4 anchor=null attributes={} literal="para" children=0
│   ├── SoftBreak scope=1:5..1:5 anchor=null attributes={} children=0
│   └── Text scope=2:1..2:4 anchor=null attributes={} literal="line" children=0
└── Table scope=3:1..5:5 anchor=null attributes={} columns=[none:null,none:null] children=2
    ├── TableHead children=1
    │   └── TableRow scope=3:1..3:5 anchor=null attributes={} children=2
    │       ├── TableCell scope=3:1..3:2 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=3:1..3:1 anchor=null attributes={} literal="a" children=0
    │       └── TableCell scope=3:4..3:5 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=3:5..3:5 anchor=null attributes={} literal="b" children=0
    ├── TableBody children=1
    │   └── TableRow scope=5:1..5:5 anchor=null attributes={} children=2
    │       ├── TableCell scope=5:1..5:2 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=5:1..5:1 anchor=null attributes={} literal="c" children=0
    │       └── TableCell scope=5:4..5:5 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=5:5..5:5 anchor=null attributes={} literal="d" children=0
    └── TableFoot children=0
````````````````````````````````

Column alignment comes from the markers' colons: left, right, both, or none.
For pipe tables, every following line is a body row until a blank line or a line that a block
start of steps 1 through 9, 11, or 12 claims; a line without pipes is a
one-cell row. A row with fewer cells than the delimiter row is completed
with empty cells whose scope is the row's end; excess cells are dropped, so
every row has `columns.count` cells:

```````````````````````````````` example
| a | b |
| - | - |
no pipes
| short |
| x | y | extra |

| z |
.
Document scope=1:1..7:5 anchor=null attributes={} children=2
├── Table scope=1:1..5:17 anchor=null attributes={} columns=[none:null,none:null] children=4
│   ├── TableHead children=1
│   │   └── TableRow scope=1:1..1:9 anchor=null attributes={} children=2
│   │       ├── TableCell scope=1:2..1:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │       │   └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
│   │       └── TableCell scope=1:6..1:8 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │           └── Text scope=1:7..1:7 anchor=null attributes={} literal="b" children=0
│   ├── TableBody children=3
│   │   ├── TableRow scope=3:1..3:8 anchor=null attributes={} children=2
│   │   │   ├── TableCell scope=3:1..3:8 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │   │   │   └── Text scope=3:1..3:8 anchor=null attributes={} literal="no pipes" children=0
│   │   │   └── TableCell scope=3:8..3:8 anchor=null attributes={} rowspan=1 colspan=1 children=0
│   │   ├── TableRow scope=4:1..4:9 anchor=null attributes={} children=2
│   │   │   ├── TableCell scope=4:2..4:8 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │   │   │   └── Text scope=4:3..4:7 anchor=null attributes={} literal="short" children=0
│   │   │   └── TableCell scope=4:9..4:9 anchor=null attributes={} rowspan=1 colspan=1 children=0
│   │   └── TableRow scope=5:1..5:17 anchor=null attributes={} children=2
│   │       ├── TableCell scope=5:2..5:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │       │   └── Text scope=5:3..5:3 anchor=null attributes={} literal="x" children=0
│   │       └── TableCell scope=5:6..5:8 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │           └── Text scope=5:7..5:7 anchor=null attributes={} literal="y" children=0
│   └── TableFoot children=0
└── Paragraph scope=7:1..7:5 anchor=null attributes={} children=1
    └── Text scope=7:1..7:5 anchor=null attributes={} literal="| z |" children=0
````````````````````````````````

```````````````````````````````` example
| a |
| - |
| b |
# h
| c |
.
Document scope=1:1..5:5 anchor=null attributes={} children=3
├── Table scope=1:1..3:5 anchor=null attributes={} columns=[none:null] children=2
│   ├── TableHead children=1
│   │   └── TableRow scope=1:1..1:5 anchor=null attributes={} children=1
│   │       └── TableCell scope=1:2..1:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │           └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
│   ├── TableBody children=1
│   │   └── TableRow scope=3:1..3:5 anchor=null attributes={} children=1
│   │       └── TableCell scope=3:2..3:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │           └── Text scope=3:3..3:3 anchor=null attributes={} literal="b" children=0
│   └── TableFoot children=0
├── Heading scope=4:1..4:3 anchor="h" attributes={} level=1 children=1
│   └── Text scope=4:3..4:3 anchor=null attributes={} literal="h" children=0
└── Paragraph scope=5:1..5:5 anchor=null attributes={} children=1
    └── Text scope=5:1..5:5 anchor=null attributes={} literal="| c |" children=0
````````````````````````````````

In every row, a `\|` becomes `|` before inline parsing, inside code spans
included, and each cell's content is trimmed of leading and trailing
whitespace and parsed as inline content:

```````````````````````````````` example
| a | b |
| - | - |
| x \| y | `c \| d` |
.
Document scope=1:1..3:21 anchor=null attributes={} children=1
└── Table scope=1:1..3:21 anchor=null attributes={} columns=[none:null,none:null] children=2
    ├── TableHead children=1
    │   └── TableRow scope=1:1..1:9 anchor=null attributes={} children=2
    │       ├── TableCell scope=1:2..1:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
    │       └── TableCell scope=1:6..1:8 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=1:7..1:7 anchor=null attributes={} literal="b" children=0
    ├── TableBody children=1
    │   └── TableRow scope=3:1..3:21 anchor=null attributes={} children=2
    │       ├── TableCell scope=3:2..3:9 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=3:3..3:8 anchor=null attributes={} literal="x | y" children=0
    │       └── TableCell scope=3:11..3:20 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Code scope=3:12..3:19 anchor=null attributes={} literal="c | d" children=0
    └── TableFoot children=0
````````````````````````````````

A pipe table produces `columns` with the delimiter row's alignments and
`relative=null`, `head` with the one header row, `content` with the body
rows, `foot=[]`, and cells with both spans equal to one. A table whose
synthesized empty cells number more than 524288 accepts no further rows,
and a row holds at most 65535 cells: a header or delimiter row with more is
not a table, and a body row with more ends the table before it.

## Captions

A caption line is:

```text
caption-line = *3SP ( "Table:" / "table:" / ":" ) rest
```

where for the `:` form the next scalar is not punctuation, the word forms are
case-sensitive, and `rest` may be empty. A caption paragraph is one whose
first line is a caption line; its content is the paragraph's parsed inline
content after removing the marker and the whitespace after it. It ends at the
same interrupting block starts as an ordinary paragraph, including core and
extension openers; non-interrupting indentation, list markers and type-7 HTML
remain paragraph content. A preceding caption does not change which block
opener owns a prospective table header. A caption paragraph is claimed by a
table of any syntax that it precedes or follows
with zero or more blank lines and nothing else between:

```````````````````````````````` example
Table: Demo caption

| a |
| - |
| b |
.
Document scope=1:1..5:5 anchor=null attributes={} children=1
└── Table scope=1:1..5:5 anchor=null attributes={} columns=[none:null] children=2
    ├── TableCaption scope=1:1..1:19 anchor=null attributes={} children=1
    │   └── Text scope=1:8..1:19 anchor=null attributes={} literal="Demo caption" children=0
    ├── TableHead children=1
    │   └── TableRow scope=3:1..3:5 anchor=null attributes={} children=1
    │       └── TableCell scope=3:2..3:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=3:3..3:3 anchor=null attributes={} literal="a" children=0
    ├── TableBody children=1
    │   └── TableRow scope=5:1..5:5 anchor=null attributes={} children=1
    │       └── TableCell scope=5:2..5:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=5:3..5:3 anchor=null attributes={} literal="b" children=0
    └── TableFoot children=0
````````````````````````````````

```````````````````````````````` example
| a |
| - |
| b |

: Demo
.
Document scope=1:1..5:6 anchor=null attributes={} children=1
└── Table scope=1:1..5:6 anchor=null attributes={} columns=[none:null] children=2
    ├── TableCaption scope=5:1..5:6 anchor=null attributes={} children=1
    │   └── Text scope=5:3..5:6 anchor=null attributes={} literal="Demo" children=0
    ├── TableHead children=1
    │   └── TableRow scope=1:1..1:5 anchor=null attributes={} children=1
    │       └── TableCell scope=1:2..1:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
    ├── TableBody children=1
    │   └── TableRow scope=3:1..3:5 anchor=null attributes={} children=1
    │       └── TableCell scope=3:2..3:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=3:3..3:3 anchor=null attributes={} literal="b" children=0
    └── TableFoot children=0
````````````````````````````````

A multi-line caption contributes `SoftBreak` nodes. A table claims its
preceding caption if present, otherwise its following caption. A table with
a preceding caption leaves the following caption candidate unconsumed: it can
become the preceding caption of the next table, or a paragraph when no table
follows. Thus `caption A / table 1 / caption B / table 2` gives A to table 1
and B to table 2; without A, B belongs to table 1. The pinned Pandoc canaries
verify both cases for every table form and caption marker. Placement is not
stored:

```````````````````````````````` example
table: first
second

| a |
| - |
| b |

:
.
Document scope=1:1..8:1 anchor=null attributes={} children=2
├── Table scope=1:1..6:5 anchor=null attributes={} columns=[none:null] children=2
│   ├── TableCaption scope=1:1..2:6 anchor=null attributes={} children=3
│   │   ├── Text scope=1:8..1:12 anchor=null attributes={} literal="first" children=0
│   │   ├── SoftBreak scope=1:13..1:13 anchor=null attributes={} children=0
│   │   └── Text scope=2:1..2:6 anchor=null attributes={} literal="second" children=0
│   ├── TableHead children=1
│   │   └── TableRow scope=4:1..4:5 anchor=null attributes={} children=1
│   │       └── TableCell scope=4:2..4:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │           └── Text scope=4:3..4:3 anchor=null attributes={} literal="a" children=0
│   ├── TableBody children=1
│   │   └── TableRow scope=6:1..6:5 anchor=null attributes={} children=1
│   │       └── TableCell scope=6:2..6:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │           └── Text scope=6:3..6:3 anchor=null attributes={} literal="b" children=0
│   └── TableFoot children=0
└── Paragraph scope=8:1..8:1 anchor=null attributes={} children=1
    └── Text scope=8:1..8:1 anchor=null attributes={} literal=":" children=0
````````````````````````````````

A preceding caption is a table-candidate block start parsed in one lookahead
with the table after it; when no table follows, its bytes are released to
paragraph parsing. A following caption is claimed after the table is
complete:

```````````````````````````````` example
Table: no table here
.
Document scope=1:1..1:20 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:20 anchor=null attributes={} children=1
    └── Text scope=1:1..1:20 anchor=null attributes={} literal="Table: no table here" children=0
````````````````````````````````

A caption line is tested before the definition-list step, as the
[definition lists](definition-lists.md) module states.

## Column arithmetic

All column positions in the syntaxes below count Unicode scalars of the line
after tab expansion to four-column stops; there is no display-width
computation. Where widths are authored, `relative[i]` is `w[i] / sum(w)` in
IEEE-754 double division with no other constant, and `w[i]` is stated per
syntax; the dump prints a double as the shortest decimal that round-trips.

## Simple tables

A simple table is recognized by this grammar:

```text
separator = *3SP dash-run *( 1*SP dash-run ) *SP EOL
dash-run  = 1*"-"
```

The header line is the nonblank line immediately before the separator and
must be the first line of a paragraph candidate. Body rows are every following
line until a blank line, or until a footer line of the same shape as the
separator followed by a blank line or the end of the document; at least one
body row or a footer is required.

Within a simple-table body, lines beginning with `#`, `>`, or code fences
remain rows and their cells are parsed as inline content. Shared block-start
precedence decides whether a table can open at its header and whether a caption
paragraph continues; it does not interrupt these already-owned simple rows.
A blank line ends the body and lets a following heading, quote, or code fence
open its own block. The input-only Pandoc 3.11 witnesses
`simple-table-{heading,quote,fence}-{in-body,after-blank}` in the
[oracle corpus](../../../specs/oracles/pandoc/corpus.json) verify both sides
of this boundary without a compatibility waiver.

```````````````````````````````` example
h    i
---- ----
a    b
# h

# outside
.
Document scope=1:1..6:9 anchor=null attributes={} children=2
├── Table scope=1:1..4:3 anchor=null attributes={} columns=[left:null,left:null] children=3
│   ├── TableHead children=1
│   │   └── TableRow scope=1:1..1:6 anchor=null attributes={} children=2
│   │       ├── TableCell scope=1:1..1:1 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │       │   └── Text scope=1:1..1:1 anchor=null attributes={} literal="h" children=0
│   │       └── TableCell scope=1:6..1:6 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │           └── Text scope=1:6..1:6 anchor=null attributes={} literal="i" children=0
│   ├── TableBody children=2
│   │   ├── TableRow scope=3:1..3:6 anchor=null attributes={} children=2
│   │   │   ├── TableCell scope=3:1..3:1 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │   │   │   └── Text scope=3:1..3:1 anchor=null attributes={} literal="a" children=0
│   │   │   └── TableCell scope=3:6..3:6 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │   │       └── Text scope=3:6..3:6 anchor=null attributes={} literal="b" children=0
│   │   └── TableRow scope=4:1..4:3 anchor=null attributes={} children=2
│   │       ├── TableCell scope=4:1..4:3 anchor=null attributes={} rowspan=1 colspan=1 children=1
│   │       │   └── Text scope=4:1..4:3 anchor=null attributes={} literal="# h" children=0
│   │       └── TableCell scope=4:3..4:3 anchor=null attributes={} rowspan=1 colspan=1 children=0
│   └── TableFoot children=0
└── Heading scope=6:1..6:9 anchor="outside" attributes={} level=1 children=1
    └── Text scope=6:3..6:9 anchor=null attributes={} literal="outside" children=0
````````````````````````````````

Columns are cut at the start position of each dash run: bytes before the
first run belong to column one, bytes from the last run's start to the end
of the line belong to the last column, and each segment is trimmed and
parsed as inline content. Alignment: with the header's segment right-trimmed,
let `leftSpace` mean the segment begins with a space or tab and `rightSpace`
mean its length is less than the dash run's; `(true, false)` is `right`,
`(false, true)` is `left`, `(true, true)` is `center`, and `(false, false)`
or an empty segment is `none`. `relative` is `null` for every column, and
every span is one:

```````````````````````````````` example
  Right Left     Center   Default
------  ------  --------  -------
    12  12          12    12
     1  1           1     1
.
Document scope=1:1..4:27 anchor=null attributes={} children=1
└── Table scope=1:1..4:27 anchor=null attributes={} columns=[right:null,left:null,center:null,none:null] children=3
    ├── TableHead children=1
    │   └── TableRow scope=1:1..1:33 anchor=null attributes={} children=4
    │       ├── TableCell scope=1:3..1:7 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=1:3..1:7 anchor=null attributes={} literal="Right" children=0
    │       ├── TableCell scope=1:9..1:12 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=1:9..1:12 anchor=null attributes={} literal="Left" children=0
    │       ├── TableCell scope=1:18..1:23 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=1:18..1:23 anchor=null attributes={} literal="Center" children=0
    │       └── TableCell scope=1:27..1:33 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=1:27..1:33 anchor=null attributes={} literal="Default" children=0
    ├── TableBody children=2
    │   ├── TableRow scope=3:1..3:28 anchor=null attributes={} children=4
    │   │   ├── TableCell scope=3:5..3:6 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │   │   │   └── Text scope=3:5..3:6 anchor=null attributes={} literal="12" children=0
    │   │   ├── TableCell scope=3:9..3:10 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │   │   │   └── Text scope=3:9..3:10 anchor=null attributes={} literal="12" children=0
    │   │   ├── TableCell scope=3:21..3:22 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │   │   │   └── Text scope=3:21..3:22 anchor=null attributes={} literal="12" children=0
    │   │   └── TableCell scope=3:27..3:28 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │   │       └── Text scope=3:27..3:28 anchor=null attributes={} literal="12" children=0
    │   └── TableRow scope=4:1..4:27 anchor=null attributes={} children=4
    │       ├── TableCell scope=4:6..4:6 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=4:6..4:6 anchor=null attributes={} literal="1" children=0
    │       ├── TableCell scope=4:9..4:9 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=4:9..4:9 anchor=null attributes={} literal="1" children=0
    │       ├── TableCell scope=4:21..4:21 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=4:21..4:21 anchor=null attributes={} literal="1" children=0
    │       └── TableCell scope=4:27..4:27 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=4:27..4:27 anchor=null attributes={} literal="1" children=0
    └── TableFoot children=0
````````````````````````````````

The header may be omitted when a footer closes the table; then `head=[]`,
and column boundaries and alignment are inferred from the separator runs and
the first body line:

```````````````````````````````` example
----  ----
a     b
c     d
----  ----
.
Document scope=1:1..4:10 anchor=null attributes={} children=1
└── Table scope=1:1..4:10 anchor=null attributes={} columns=[left:null,left:null] children=2
    ├── TableHead children=0
    ├── TableBody children=2
    │   ├── TableRow scope=2:1..2:7 anchor=null attributes={} children=2
    │   │   ├── TableCell scope=2:1..2:1 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │   │   │   └── Text scope=2:1..2:1 anchor=null attributes={} literal="a" children=0
    │   │   └── TableCell scope=2:7..2:7 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │   │       └── Text scope=2:7..2:7 anchor=null attributes={} literal="b" children=0
    │   └── TableRow scope=3:1..3:7 anchor=null attributes={} children=2
    │       ├── TableCell scope=3:1..3:1 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=3:1..3:1 anchor=null attributes={} literal="c" children=0
    │       └── TableCell scope=3:7..3:7 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=3:7..3:7 anchor=null attributes={} literal="d" children=0
    └── TableFoot children=0
````````````````````````````````

The header is one line, as in Pandoc. When two or more lines precede the
separator, the line before it is not the first line of the paragraph
candidate, so there is no table: the lines stay a paragraph, and the dash
line, completing no candidate, is a thematic break:

```````````````````````````````` example
Right   Left
More    Lines
-----   -----
12      12
.
Document scope=1:1..4:10 anchor=null attributes={} children=3
├── Paragraph scope=1:1..2:13 anchor=null attributes={} children=3
│   ├── Text scope=1:1..1:12 anchor=null attributes={} literal="Right   Left" children=0
│   ├── SoftBreak scope=1:13..1:13 anchor=null attributes={} children=0
│   └── Text scope=2:1..2:13 anchor=null attributes={} literal="More    Lines" children=0
├── ThematicBreak scope=3:1..3:13 anchor=null attributes={} children=0
└── Paragraph scope=4:1..4:10 anchor=null attributes={} children=1
    └── Text scope=4:1..4:10 anchor=null attributes={} literal="12      12" children=0
````````````````````````````````

## Multiline tables

A multiline table is recognized by this grammar:

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
column positions. Right-trim each segment and remove the common leading-space
indent of the cell's nonempty segments, preserving relative indentation within
the cell. Join those segments with LF and parse them as a block sequence,
including header cells. `w[i]` is the scalar count from the start of dash run
`i` to the start of run `i+1`, the last run being its own length. Alignment
follows the simple-table rule, and every span is one:

```````````````````````````````` example
----------------
Left       Right
------- --------
a       b
c

d       e
----------------
.
Document scope=1:1..8:16 anchor=null attributes={} children=1
└── Table scope=1:1..8:16 anchor=null attributes={} columns=[left:0.5,right:0.5] children=3
    ├── TableHead children=1
    │   └── TableRow scope=2:1..2:16 anchor=null attributes={} children=2
    │       ├── TableCell scope=2:1..2:8 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Paragraph scope=2:1..2:4 anchor=null attributes={} children=1
    │       │       └── Text scope=2:1..2:4 anchor=null attributes={} literal="Left" children=0
    │       └── TableCell scope=2:9..2:16 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Paragraph scope=2:12..2:16 anchor=null attributes={} children=1
    │               └── Text scope=2:12..2:16 anchor=null attributes={} literal="Right" children=0
    ├── TableBody children=2
    │   ├── TableRow scope=4:1..5:1 anchor=null attributes={} children=2
    │   │   ├── TableCell scope=4:1..5:1 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │   │   │   └── Paragraph scope=4:1..5:1 anchor=null attributes={} children=3
    │   │   │       ├── Text scope=4:1..4:1 anchor=null attributes={} literal="a" children=0
    │   │   │       ├── SoftBreak scope=4:10..4:10 anchor=null attributes={} children=0
    │   │   │       └── Text scope=5:1..5:1 anchor=null attributes={} literal="c" children=0
    │   │   └── TableCell scope=4:9..4:9 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │   │       └── Paragraph scope=4:9..4:9 anchor=null attributes={} children=1
    │   │           └── Text scope=4:9..4:9 anchor=null attributes={} literal="b" children=0
    │   └── TableRow scope=7:1..7:9 anchor=null attributes={} children=2
    │       ├── TableCell scope=7:1..7:8 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Paragraph scope=7:1..7:1 anchor=null attributes={} children=1
    │       │       └── Text scope=7:1..7:1 anchor=null attributes={} literal="d" children=0
    │       └── TableCell scope=7:9..7:9 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Paragraph scope=7:9..7:9 anchor=null attributes={} children=1
    │               └── Text scope=7:9..7:9 anchor=null attributes={} literal="e" children=0
    └── TableFoot children=0
````````````````````````````````

## Grid tables

A grid table's lines begin and end with `|` or `+` at the table margin.
Its column boundaries are the union of `+` positions connected to the outer
border by horizontal `-` or `=` segments (with optional edge colons).
Other `+` and `|` characters remain cell content, including nested grids.

Source boundary lines define elementary row/column regions. A missing vertical
wall on any physical content line joins neighboring regions; a missing
horizontal segment joins regions above and below it. Every connected region
must form one rectangle within one row group. Its width and height are the
cell's `colspan` and `rowspan`; the cell is stored once in its starting row.
This checks the complete cell boundary, including a wall present on only some
of its content lines. A nonrectangular region rejects the candidate.

A line whose segments are all `=` is a head separator when it is the first
such line and there is no foot yet, and the foot is the final row group
enclosed above and below by `=` lines; at most one head separator and at
most one foot exist, and any other `=` line fails recognition. With a head
separator only its colons select alignment; without one the top line's do,
and colons elsewhere are ignored. `w[i]` is the scalar count strictly between
the column's boundary positions:

```````````````````````````````` example
+---+---+
| a | b |
+===+===+
| c     |
+---+---+
.
Document scope=1:1..5:9 anchor=null attributes={} children=1
└── Table scope=1:1..5:9 anchor=null attributes={} columns=[none:0.5,none:0.5] children=2
    ├── TableHead children=1
    │   └── TableRow scope=2:1..2:9 anchor=null attributes={} children=2
    │       ├── TableCell scope=2:2..2:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Paragraph scope=2:3..2:3 anchor=null attributes={} children=1
    │       │       └── Text scope=2:3..2:3 anchor=null attributes={} literal="a" children=0
    │       └── TableCell scope=2:6..2:8 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Paragraph scope=2:7..2:7 anchor=null attributes={} children=1
    │               └── Text scope=2:7..2:7 anchor=null attributes={} literal="b" children=0
    ├── TableBody children=1
    │   └── TableRow scope=4:1..4:9 anchor=null attributes={} children=1
    │       └── TableCell scope=4:2..4:8 anchor=null attributes={} rowspan=1 colspan=2 children=1
    │           └── Paragraph scope=4:3..4:3 anchor=null attributes={} children=1
    │               └── Text scope=4:3..4:3 anchor=null attributes={} literal="c" children=0
    └── TableFoot children=0
````````````````````````````````

Cell text is, per line, the scalars strictly between the cell's boundary
positions, right-trimmed; if every non-empty line begins with a space, one
space is removed from each; the lines are joined with LF and parsed by the
block parser, so cells hold paragraphs, code, lists, headings, nested tables,
and every enabled block. Logical rows are defined by the source's structural
boundary lines, independently of whether any cell starts in the row. An
interior boundary such as `+   +   +` retains both cells across the boundary
and starts another row with no new cells; the parser preserves that row's
empty `cells` array. A cell that spans down is owned by the row in which it
starts, and its scope extends below that row's last line:

```````````````````````````````` example
+-------+-------+
| first | - i   |
| again +-------+
|       | x     |
+-------+-------+
.
Document scope=1:1..5:17 anchor=null attributes={} children=1
└── Table scope=1:1..5:17 anchor=null attributes={} columns=[none:0.5,none:0.5] children=2
    ├── TableHead children=0
    ├── TableBody children=2
    │   ├── TableRow scope=2:1..3:17 anchor=null attributes={} children=2
    │   │   ├── TableCell scope=2:2..4:8 anchor=null attributes={} rowspan=2 colspan=1 children=1
    │   │   │   └── Paragraph scope=2:3..3:7 anchor=null attributes={} children=3
    │   │   │       ├── Text scope=2:3..2:7 anchor=null attributes={} literal="first" children=0
    │   │   │       ├── SoftBreak scope=2:18..2:18 anchor=null attributes={} children=0
    │   │   │       └── Text scope=3:3..3:7 anchor=null attributes={} literal="again" children=0
    │   │   └── TableCell scope=2:10..2:16 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │   │       └── List scope=2:11..2:13 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=true children=1
    │   │           └── ListItem scope=2:11..2:13 anchor=null attributes={} marker=null children=1
    │   │               └── Paragraph scope=2:13..2:13 anchor=null attributes={} children=1
    │   │                   └── Text scope=2:13..2:13 anchor=null attributes={} literal="i" children=0
    │   └── TableRow scope=4:1..4:17 anchor=null attributes={} children=1
    │       └── TableCell scope=4:10..4:16 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Paragraph scope=4:11..4:11 anchor=null attributes={} children=1
    │               └── Text scope=4:11..4:11 anchor=null attributes={} literal="x" children=0
    └── TableFoot children=0
````````````````````````````````

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
follows the [cross links](cross-links.md) rule, and
the [comments](comments.md) rule states how comments interact with cell
boundaries.

## Scopes

`Table.scope` spans the caption, when one is claimed, and the table.
`TableCaption.scope` includes its marker. A pipe row covers its line and a
pipe cell the bytes between its pipes; a completed pipe cell's scope is the
row's end. A simple-table row covers its line, and a simple cell covers its
segment trimmed of leading and trailing whitespace, or the one byte at the
segment's start when the segment is empty, or the line's last byte when the
segment lies beyond the line's end. A multiline row covers its physical
lines and a grid row its physical lines following its opening boundary,
through the line before the next row begins (excluding the table's final
closing boundary). A row with no physical body lines uses its closing boundary
line as its source extent. Rows do not require a starting cell to have a scope.
A multiline or grid cell covers the region between
its column boundaries on its first line through the same region on its last
line, clipped to each line's end. A grid cell with no physical body lines uses
the corresponding segment of its closing boundary. Its descendants use original-source
coordinates, so a `SoftBreak` produced by joining two segments covers the
physical line ending of the earlier segment's line, and such a range may
include other cells' bytes. A grid cell whose `rowspan` exceeds one ends
below its owning row's last line; this is the one case in which a child's
scope leaves its parent's. `canonical-ast.md` records both exceptions to
the contiguous-range and containment rules, and the scope-containment gate
ledgers the second with `P11d`.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover, for
pipe tables, two-hyphen and aligned markers, every interrupting block start,
and the cell limit; for captions, between two tables, punctuation after the
colon, and definition-list precedence; for simple tables, closing
separators, a second line before the separator, and Setext and
thematic-break precedence;
for multiline tables, headerless forms and the one-row rule; for grid
tables, multi-row heads, interleaved active spans, source-defined fully covered
rows with `cells=[]`, authored empty cells with `content=[]`,
alignment, foot, and rejection of overlap, overrun, uncovered coordinates,
cross-group spans, and stray `=` lines; and for all, exact table, caption,
row, and cell scopes, allocation failure, deep nested cells, and size-doubling rows, columns, and boundaries.
