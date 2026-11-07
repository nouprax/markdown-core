# Parser input and scratch ownership

The parse transaction owns input geometry, content mapping runs, reusable
workspaces, and the node pool. A completed document retains only its AST values
and node storage. Scratch never becomes an AST field.

## One physical input index

`markdown_core_input_line` records the raw start, content end, optional-fact
index, and the offset where the line's own bytes begin past the prefixes of
the containers it continues, of one physical line. The next-line offset is derived from the following record or the scan
frontier; NUL counts live only in the optional record. Container-prefix lookahead and
table-search facts are addressed through that same entry. Compact geometry
is stored for each line; a separate grow-only vector holds optional facts only
for lines needing grammar state or a normalized view. The facts contain no copy
of line geometry and have no independent line index or lifetime. The document
driver, speculative
readers, properties envelope, and mapped cell driver extend and consult one
index. There is no eager mapped-input scan followed by a second driver scan.

Geometry is **16 bytes per visited line** on the supported ABIs. Parser
initialization reserves eight entries (128 bytes), including for an
empty document, alongside the current-line buffer. For L > 0 lines its vector
reserves C = max(8, next_power_of_two(L))
entries, or 16C resident bytes. Thus for L >= 8 the geometry alone occupies
[16L, 32L) bytes: for one-byte LF-only lines it is 16–32 times input size;
for two-byte `x\n` lines it is 8–16 times input size. This excludes the source,
AST, allocator headers, optional facts and normalized payloads. It is a linear
space bound with a substantial short-line constant, not a constant-space claim.
The optional record is 64 bytes on LP64/LLP64 and is reserved only for queried
or NUL-bearing lines; its vector has the same doubling bound. Capacity survives
input changes, so the bound uses the maximum visited-line and fact counts of
any active input during the parse, not just the final input's length.
For N NUL-bearing lines, normalized views additionally retain one pointer-sized
header per line plus its raw content length, two extra bytes per NUL, and LF/NUL
terminators. On LP64/LLP64, repeated `\0\n` therefore requests
16C + 64C + 13L bytes for geometry, facts and normalized views: for L >= 8,
[93L, 173L), or 46.5–86.5 times that two-byte-per-line input. This excludes
other parser workspaces, the AST and allocator overhead; neither this bound
nor the LF-only geometry bound is a bound on total parse memory or process RSS.
Tests use empty, one-character and NUL-bearing lines across capacity boundaries
and assert record size, independent geometry/fact capacity bounds, one normalized
view per NUL-bearing line, and one frontier advance per source byte.

The source driver advances through the static inline scanner in `blocks.c`.
External lookahead calls a wrapper around that same scanner; there is one
physical scanning algorithm, not separate driver and speculative scanners.
The inner span walk uses bounded pointers and stops at CR, LF or NUL; only a
NUL boundary updates the normalization count. One bounded span scanner probes
whole machine words for these three bytes and resolves a matching word or tail
bytewise. Unsigned zero-byte tests are endian-independent, and fixed-width
`memcpy` avoids alignment and aliasing assumptions. Every probe stays within
the input; no padding or sentinel is required. Geometry excludes
physical terminators, so the grammar's mutable content/LF/NUL line is built
with one reservation and copy, without testing and appending the terminator
as a second buffer operation. All 256 byte values at every position across
three machine words are checked against the physical-line and normalization
contracts, including the scalar tail after a split.
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

Index lookups are constant time once a line exists. Extension advances each
raw byte once; a span boundary may add one word probe before byte resolution.
Thus span-search byte inspections stay bounded by nine times the input length even
for delimiter/NUL-only inputs, without an input-size or grammar-specific
algorithm. CR, LF, and CRLF each terminate one physical line.
Pointers into the vector expire on growth, so consumers carry indices or
geometry values across calls that can extend it. Properties stores the envelope
start index and loads geometry by value. Lookahead reacquires its fact record
by line number after element callbacks. Table separator operations retain dash
offsets and fetch interval values; they never return a borrowed dash pointer. Changing the active input
resets its facts and used length while retaining vector capacity.

NUL replacement belongs to the input view, before block grammar reads it.
A line containing NUL owns one immutable UTF-8 replacement view through its
optional facts record, shared by the driver and lookahead. Its storage stays
stable while the index grows or
another line is normalized, and is released when that input ends. Ordinary
lines borrow the source. A separate ownership chain visits only allocated
views during disposal. The native cmark byte-column convention is preserved.
Properties still validates its authored source bytes; normalization does not
make an invalid metadata member valid.

This matters for tables: speculative lines are subsequently copied into
mapped cell inputs. Passing raw NUL into those cells previously let their
driver expand bytes a second time, invalidating the content-to-source map.
Both paths now construct cells from the same normalized lines.

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
