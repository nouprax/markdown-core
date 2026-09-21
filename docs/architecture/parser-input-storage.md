# Parser input and scratch ownership

The parse transaction owns input geometry, content mapping runs, reusable
workspaces, and the node pool. A completed document retains only its AST values
and node storage. Scratch never becomes an AST field.

## One physical input index

`markdown_core_input_line` records the raw start, content end, next-line
offset, and NUL count of one physical line. Container-prefix lookahead and
table-search facts are addressed through that same entry. Compact geometry
is stored for each line; a separate grow-only vector holds optional facts only
for lines needing grammar state or a normalized view. The facts contain no copy
of line geometry and have no independent line index or lifetime. The document
driver, speculative
readers, properties envelope, and mapped cell driver extend and consult one
index. There is no eager mapped-input scan followed by a second driver scan.

Index lookups are constant time once a line exists. Extension scans each raw
byte once for geometry; CR, LF, and CRLF each terminate one physical line.
Pointers into the vector expire on growth, so consumers carry indices or
geometry values across calls that can extend it. Changing the active input
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

`input_line_work` counts bytes scanned for physical geometry. The independent
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

Removing reference definitions changes a paragraph's mapping only if it has
consumed a definition prefix. That fact persists across a setext probe and
finalization, including when the first probe consumed the entire content and
new content arrived later.

## Scratch lasts for the parse

The table element owns one parser workspace for captured lines, column maps,
dash intervals, candidates, frontier state, closed regions, boundaries, and
sorting scratch. Candidate completion resets used lengths, flags, and owned
ranges; disposal releases capacity. Column and dash views store offsets into
their vectors, never pointers that a later reservation can invalidate.
Table AST columns are copied into document-owned storage at commitment.

The grid frontier remains proportional to width and closed regions to output.
There is no rows-by-width dense grid. Stable source ordering shares one radix
workspace across table regions, headings, footnotes, and specimens. Its space
depends on entries, not the coordinate range, and at most eight byte passes
order any input. Repeating bounded-size candidates does not allocate more
scratch after the high-water capacity has been reached.

Reservations check overflow, retain the old pointer and capacity on failure,
and return the new pointer for typed assignment. They never access a typed
pointer object through `void **`. A failed later reservation can therefore
dispose every earlier successful growth through the same workspace owner.

Retained scratch trades a bounded high-water memory footprint for allocation
reuse. It ends with the parser, including failed transactions, and never
survives in the returned document. Node slab ownership remains described in
[node-storage.md](node-storage.md).
