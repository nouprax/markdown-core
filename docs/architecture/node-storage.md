# Node storage and lifetime

The engine node contains its tree links, source mapping, attributes, element
state, and a union of typed node data pointers. Every union arm is a pointer;
adding fields to one kind cannot enlarge the common node. A kind with no
kind-specific fields has no data record. Field-bearing kinds own a
record containing their ordinary typed fields. Construction places the node
and its record together in one cell, with the typed pointer referring directly
to that record; a record larger than the cell's record space is owned apart
from the cell through `node_data_allocation`, exactly as a replacement record
is, so release has one rule for both. A C99 union provides scalar alignment
for the node and the record; the cell's header before them is padded to the
same alignment, and that padding is included in measured memory costs.

## Cells, slabs and the pool

A node's storage is a fixed-size cell: a header naming the slab it came from,
the node, and room for its kind's record. A parse takes cells from slabs --
one allocation holding many cells -- through a pool the parser owns, and a
caller with no parse takes one cell from the allocator; the header says which,
and nothing else about the storage is visible through the node.

A slab lives while anything holds it: every cell taken from it, and the pool
while that slab is the one it takes cells from. A cell released during the
parse goes back to the pool and is handed out again, zeroed, before another
cell is taken from a slab, so the storage a parse holds is bounded by its peak
live node count rather than by how many nodes it made. A cell released with no
pool drops its hold, and the slab is freed with its last one -- by whichever
release that turns out to be. Disposing the pool drops the holds the pool
itself has (its released cells, its current slab) and nothing else, so the
finished tree keeps its slabs, and a subtree unlinked from a parsed document
outlives the document like a hand-built one: `markdown_core_node_free` releases
either. What a retained subtree keeps alive is its slabs, not its nodes. The
nodes of one slab are released from one thread at a time; two parses never
share a slab.

Why: a node's chunk was larger than the C library's fast-path size classes, so
every release of one walked the allocator's merge path, and releasing the
finished tree cost more than a third of parsing it.

All block, inline, and manual construction uses the same node constructor.
It takes one cell for the node and its kind's record, establishes defaults,
and only then exposes the node. Failure releases all acquired storage; a slab
that cannot be allocated refuses the node and leaves the pool usable.
Node data and its strings use the library's allocator.

`CrossLink` stores a `markdown_core_cross_reference` record containing its raw
path, optional anchor, and optional label. `CrossEmbedded` stores a
`markdown_core_cross_embedded` record containing the same reference fields and
optional dimensions. The two kinds share the reference layout, while each
occurrence owns its string chunks; optional presence remains independent of
string length. The node kind identifies a link or transclusion, with no
embedded flag in either record.

Only `Embedded` and `CrossEmbedded` expose `Dimensions(width, height?)`. The
optional value is stored inline in the occurrence's typed record, without a
separate allocation, and its lifetime ends with that record. A `Embedded` node
retains its own dimensions even when its destination and title come from a
resource shared with other resolved references. Cross references own their raw
destination fields directly and do not share a resource with a definition.

Parser construction transfers a detached, independently owned subtree through
`markdown_core_node_attach_owned`. The caller establishes disjoint ownership
by creating the subtree or detaching it from a known separate owner; merely
having no parent is not proof of disjointness. The operation checks local
containment and allocator invariants, then splices once in constant time.
The arbitrary mutation API checks ancestry, allocator and containment once
before unlinking, then commits through the same non-failing splice. In
particular, a custom containment predicate observes the original tree and is
never called again after detachment. Rejection leaves both trees unchanged.
Kind conversion changes no edges and checks only containment.
There is no safety mode, ancestry cache, or separate inline splice algorithm.
A source-boundary audit keeps arbitrary reparenting out of parser construction;
regression inputs vary nesting depth and autolink count independently.

Kind conversion preserves node identity and tree links. After containment
validation, it allocates a replacement record before releasing the old fields.
The original record shares the node's cell and is reclaimed with the
node, unless it did not fit the cell; that record, and every replacement
record, is freed when replaced or when the node dies.
The typed view and allocation ownership are explicit: `as` points to the
current record, while `node_data_allocation` owns a replacement allocation, if
any. Ownership is never inferred by comparing potentially adjacent addresses.
`markdown_core_node_set_kind` distinguishes containment rejection from allocation
failure. Parser callers decline rejected conversions and set the OOM flag only
for allocation failure. Either failure leaves the original kind and all owned
values intact. A successful conversion releases node-valued fields through the
same iterative destruction walk used for ordinary tree destruction. The
element's opaque state belongs to the node and element, so it survives a
kind conversion.

HTML blocks keep their recognition state and eventual literal in distinct
fields of one data record throughout parsing. Converting a closed HTML comment
to Comment transfers its owned literal only after the new record can be
created. Setext headings also use the shared kind conversion operation.

Construction and kind conversion have different ownership constraints: an
unpublished node and its initial record can share a cell, while a
replacement record must preserve the existing node's address. All kinds use
these same lifecycle rules. No per-kind pools, packed field offsets, or
cardinality-dependent storage paths are needed. Benchmarks measure parse time,
allocation work, and memory independently of the deterministic layout tests.

Tests protect the pointer-sized union, constructor allocation failures,
transactional kind conversion, containment rejection in parser conversions,
owned subtree release, whole-parse OOM propagation, and the pool's claims in
allocator counts: one allocation per slab of many cells, a released cell
reused before a slab is touched, a refused slab refusing only the node, and a
slab freed by the last of its cells after the pool is gone. Platform builds verify
native alignment, and sanitizer suites exercise the same ownership paths.

Link reference definitions are recognized during block parsing so paragraph
content and Setext classification can use the remaining text. A finalized
paragraph containing only definitions stays in its parent's child chain with
the internal `REFERENCE_DEFINITION_ONLY` flag. Later block identifiers see
that paragraph in source order and cannot attach across it. After all block
syntax and anchor decisions finish, one iterative postorder pass discards
these paragraphs and derives list layout from the cleaned semantic children,
before inline parsing. The document owns them through their parents,
including on parse failure. An intentionally empty anchored list-item
paragraph is not a definition and survives this cleanup. No definition node
reaches the public AST.

Inline footnotes use the existing Footnote data record and one-item Cite.
A successful close transfers the parsed inline body directly to
Document.footnotes. A Cite never has a Footnote child: its Citation names the
value by id. Authored definitions remain in the block tree until their bodies
have been parsed. In both cases the document already owns the node, including
on parse failure.

Both forms register in one parser collection when their syntax commits. Its
entries borrow the Footnote and, for an inline form, its Citation. Failed
candidates never register. Inline parsing cannot retract a committed note:
referenced-call conversion may discard parsed label content, but its defined
label cannot contain `]`, so it cannot enclose a completed inline footnote.
The document's inline-value chain is also the work queue for deferred directive
labels in those bodies; newly produced notes append to it and are processed
once by the same field parser.

Finalization processes only the F registered values, with no tree walk to
discover footnotes. Registration order differs from source order: definitions
precede inline parsing, nested bodies close inside out, and directive labels
parse after the main tree. A stable counting pass per byte of the packed
32-bit source coordinates bounds ordering work by O(F): the keys are computed
once, only the bytes on which some key differs are passed over, and an input
already in order is left where it is. All authored ids are reserved before
inline ids are assigned. Collision probes consume disjoint authored-id
namespaces, so their total is bounded by F plus the authored-id count.
After every allocation succeeds, finalization moves the values into one
source-ordered document chain and discards the parser collection.

Consolidation and element postprocessing begin only after finalization.
Their common tree-phase walker visits Document.footnotes and element-owned
fields from their live owner slots. Callbacks receive resolved ids and the
completed ownership model; removing a document value cannot leave a pointer
in a parser index. OOM cleanup uses the document's existing ownership graph,
and semantic reference cycles never become object cycles.

The bracket scanner tracks the most recent non-SP/TAB byte over disjoint
consumed token ranges, so rejecting empty bodies never rescans nested bodies.
Only `^[` terminates an ordinary text run; other carets incur the same
allocation work as other text. Bare autolinks use the enclosing inline
context's start and closing delimiter, preserving the footnote boundary.

Element-owned fields participate in the same iterative destruction walk.
Before freeing an element payload, the core visits its owned-root slots,
splices their chains into the walk, and clears the slots. The element frees
only its remaining value storage. Directive labels use this contract, and
table captions use the same operation. The operation allocates nothing and does
not recurse through field nesting. Kind conversion continues to preserve the
element's opaque state, including its owned fields.

## Mapped table block inputs

Table candidates retain source slices and temporary geometry until recognition
succeeds. A grid uses connected source regions to validate complete rectangles and group
boundaries, then emits sparse anchor cells. Its compacted active frontier holds
at most twice the column count; closed regions are the final candidate cells,
ordered by their source anchors with the shared stable radix operation. Temporary
geometry is released on acceptance, rejection, or allocation failure. No
occupied-coordinate matrix becomes part of the public AST.

Multiline and grid cell bodies enqueue mapped inputs on their owning nodes.
The parser drains that queue, including newly discovered nested cells, before
running document-wide completion and inline parsing. Each input uses the same
block parser, reference map, heading registry and definition owner. The active
block root bounds finalization without creating a second Document or recursing
into the document parser. Content marks compose through nested slices when
blocks and inline payloads are created; scopes are never repaired afterward.

Queued inputs borrow nodes owned by the document. Cell buffers and maps live
until their block parse ends; pending buffers remain node-owned on failure.
The lookahead cache belongs to the active input and resets only its used slots.
Failed multiline suffix queries retain container-and-offset-qualified absence
facts so later candidates do not repeatedly scan the same suffix.

Generated scanners accept exact read-only slices. Their cursor and marker
are offsets; a virtual NUL at the slice limit handles termination without
writing a sentinel, requiring padding or forming an out-of-bounds pointer.
Both scanner families are reproducible raw output of the pinned re2c version.

A table query borrows its current line, immutable input lines and the parser's
normalized EOF line until commitment finishes. One parser-owned line workspace
is reused between queries; per-query column geometry and separator intervals
are released before the next query. Deferred cell parsing starts after this
borrow ends. Dash-run facts are scanned once per captured line and reused by
all candidate grammars. Intervals are materialized only when needed; paragraph
header precedence is queried only after its separator grammar matches.

Streaming block opening and captured table/caption queries share one core
prefix recognizer. It returns borrowed marker facts; only streaming commitment
opens nodes. Element probes share their producers' grammar and obtain later
lines through a caller-owned reader, using either the current container
lookahead or captured source lines. Probing a comment closer therefore does
not nest a parser transaction. Paragraph interruption keeps its real list,
HTML and indentation rules, and queries do not alter the streaming line's
thematic-break failure cache.

Deferred cells can register headings and references out of physical order.
Document completion stably orders entries by original line and column using the
same key-aware radix operation. Explicit reference definitions take priority over
implicit heading definitions, then the earliest authored definition wins.

The table caption is an independent element-owned root, visited before rows.
C exposes it through `markdown_core_node_table_caption`; Swift, Kotlin and ES
copy it with the rest of the immutable result. JNI uses the shared optional
node-field continuation, and Wasm's fixed node record uses its owner-typed
`fieldIndex` for either a directive label or a table caption. The row chain and
its head/body/foot counts continue to describe rows alone.
