# Parser input and scratch ownership

The parse transaction owns input geometry, content mapping runs, reusable
workspaces, and the node pool. A completed document retains only its AST values
and node storage. Scratch never becomes an AST field.

## One physical input index

`markdown_core_input_line` records the raw start and content end of one
physical line, the offset where the line's own bytes begin past the prefixes
of the containers it continues, and how far the readings that began on it
read. The next-line offset is derived from the line's terminator. The lines
live in one tree (`tree.h`) that the document driver, speculative readers,
the properties envelope and the mapped cell driver extend and consult. There
is no eager mapped-input scan followed by a second driver scan.

A line holds nothing beyond its geometry: **20 bytes per visited line**,
kept in the tree's leaves. What a container-prefix lookahead learns of the
lines it visits -- the deepest container it matched on each, the line's state
past that prefix, and the runs of blank lines it stepped over -- belongs to
one reading of the input, the scan memo (`blocks.c`, THE SCAN MEMO). The memo
is a window of lines that grows as the reading's lookaheads overlap and starts
anew when one begins past it; each reading starts it empty, so an edit has
nothing on its lines to invalidate, and a line the focus passes keeps only
its bytes.

The source driver advances through the static inline scanner in `blocks.c`.
External lookahead calls a wrapper around that same scanner; there is one
physical scanning algorithm, not separate driver and speculative scanners.
The inner span walk uses bounded pointers and stops at CR or LF. One bounded
span scanner probes whole machine words for these two bytes and resolves a
matching word or tail bytewise. Unsigned zero-byte tests are
endian-independent, and fixed-width `memcpy` avoids alignment and aliasing
assumptions. Every probe stays within the input; no padding or sentinel is
required. All 256 byte values at every position across three machine words
are checked against the physical-line contract, including the scalar tail
after a split.
The active grammar line always has exactly one terminal LF. Advancing grammar
cursors does not change its extent, so both block-end and last-line columns
derive the content length from that shared invariant instead of inspecting
and stripping line endings again. Debug/ASan asserts the invariant.
Source-column projection checks the root-input identity in a header inline;
only mapped cell input enters the out-of-line mapping operation. The same
wrapper serves all producers, including the driver's per-line finalization.
Parser creation reserves the current line and the line index at fixed sizes.
Every allocation proportional to the input happens as a stage grows them. The
benchmark measures only the two stages, so it cannot see work moved into
creation. Keeping input-proportional reservation out of initialization is an
invariant for review to hold; no number will report a breach.

Index lookups are logarithmic in the tree. Extension advances each raw byte
once; a span boundary may add one word probe before byte resolution. CR, LF,
and CRLF each terminate one physical line.
Pointers into the tree and the memo expire on growth, so consumers carry
line numbers or geometry values across calls that can extend them. Properties
stores the envelope start index and loads geometry by value. Lookahead
reacquires its memo entry by line number after element callbacks. Table
separator operations retain dash offsets and fetch interval values; they
never return a borrowed dash pointer.

## The text as written

A NUL is a byte of the text like any other control byte. The parse reads the
text as it is written, validating and repairing nothing: no line has a view
of its own, a NUL stays in the text of the node that holds it, and every
reader ends at its length, never at a 0 byte. The scanners read a NUL as the
control byte it is (`scanner_common.re`), the inline cursor's end is its
position (`markdown_core_inline_is_eof`), and lists of keys carry their sizes
(`buffer.h`). A text with NUL parses as the same text with `\x01` in its
place does, which the API tests and a differential check over the fixtures
hold. Writing U+FFFD for it, as CommonMark asks of HTML output, is a
renderer's job; the AST carries the byte.

`input_line_work` counts bytes consumed by the physical geometry frontier,
excluding bounded word lookahead. The independent
properties closing-fence search is counted by `properties_line_work`; it does
not derive line geometry. Metadata's one-token classification cache retains
the boundary line's key classification for the next field. Plain keys borrow
source spans; quoted keys own decoded strings. Unknown and duplicate fields
are rejected before value validation or decoding.

## Content maps are values

`markdown_core_content_map` is the three-field view `(first, count, offset)`
into parser-owned mapping runs. Nodes hold this value, and slicing, lookup,
consolidation, and email splitting use it directly. A mapping view is not a
partially initialized node and owns no storage. Source operands are const;
destination views may alias their source when taking a slice.

A paragraph's mapping is in the coordinates of its text, its content less
`cut_front`: marks record the text's length as each line arrives. Reading
reference definitions grows `cut_front` by what they read and rebases the
mapping by the same count. Content that arrives after a setext probe read
definitions -- including a probe that read the entire content -- is marked in
the same coordinates, so one mapping serves the probe and the close.

## Scratch lasts for the parse

The table element keeps no workspace of its own. A pipe table is admitted at
its delimiter row from the current line alone; the head row is the open
paragraph's last line, already captured. Container continuation can strip a
prefix but cannot add or split cells. The indexed physical extent keeps CR,
CRLF and unterminated EOF consistent with ordinary input. A row's cells borrow
its line while it is parsed, and table columns are copied into
document-owned storage at commitment.

Reservations check overflow, retain the old pointer and capacity on failure,
and return the new pointer for typed assignment. They never access a typed
pointer object through `void **`. A failed later reservation can therefore
dispose every earlier successful growth through the same workspace owner.

Retained scratch trades a bounded high-water memory footprint for allocation
reuse. It ends with the parser, including failed transactions, and never
survives in the returned document. Node slab ownership remains described in
[node-storage.md](node-storage.md).
