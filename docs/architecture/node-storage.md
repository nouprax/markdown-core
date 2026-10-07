# Node storage and lifetime

The engine node contains its reference count, its children tree, its id,
source mapping, attributes, element state, and a union of typed node data
pointers. Every union arm is a pointer;
adding fields to one kind cannot enlarge the common node. A kind with no
kind-specific fields has no data record. Field-bearing kinds own a
record containing their ordinary typed fields. Construction places the node
and its record together in one slot, with the typed pointer referring directly
to that record; a record larger than the slot's record space is owned apart
from the slot through `node_data_allocation`. Kind conversion uses the same
capacity and ownership rule. A C99 union provides scalar alignment
for the node and the record; the slot's header before them is padded to the
same alignment, and that padding is included in measured memory costs.

## Slots, slabs and the pool

A node's storage is a fixed-size slot: a header naming the slab it came from,
the node, and room for its kind's record. A parse takes slots from slabs --
one allocation holding many slots -- through a pool its caller lends it: a
session's, which outlives each of its edits, or one made for a single parse.
A caller with no parse takes one slot from the allocator; the header says
which, and nothing else about the storage is visible through the node.

A slab lives while anything holds it: every slot taken from it, and the pool
while that slab is the one it takes slots from. A slot released into a pool
goes back to it and is handed out again, initialized, before another slot is
taken from a slab, so the storage a parse holds is bounded by its peak live
node count rather than by how many nodes it made, and a session's edits reuse
the slots of the nodes they retire instead of pinning a slab per edit. A slot
released with no pool drops its hold, and the slab is freed with its last one
-- by whichever release that turns out to be. Disposing the pool drops the
holds the pool itself has (its released slots, its current slab) and nothing
else, so a finished tree keeps its slabs, and a subtree unlinked from a parsed
document outlives the document like a hand-built one: `markdown_core_node_free`
releases either. What a retained subtree keeps alive is its slabs, not its
nodes. The nodes of one slab are released from one thread at a time; two
sessions never share a slab.

Why: a node's chunk was larger than the C library's fast-path size classes, so
every release of one walked the allocator's merge path, and releasing the
finished tree cost more than a third of parsing it.

Resources are slots too. A resource is the destination and title a direct
`Link` or `Embedded` or a `Reference` states, owned by that one node; a
reference occurrence holds its label and no resource. It is taken from a
second pool of the parse, and `slab.h` is the one mechanism both pools use.
Its node releases it: into a pool, its slot goes back to that pool's resource
slabs; with none, it drops its slab hold. So the tree keeps its resource slabs
as it keeps its node slabs.

## Shared immutable nodes

A node is a shared immutable value (docs/plans/
2026-09-29-incremental-parsing.md, 5.11), as a tree-sitter subtree is: one
subtree may sit in the old tree and the new one at once, so a node has no
parent and no siblings. Its children are its children tree, a stem: a
balanced tree of at most 32 entries per stem, each stem counting the nodes
under it. Walks, the canonical dump and the bindings see the children in
order, and the C API reads them through a tree cursor that holds its path
on a stack of its own. A node or a stem counts the stems, fields and
builders that hold it. A node held once may change in place; one held more
often is copied before it changes. Releasing a tree walks it on an explicit
stack and frees each node and stem whose count reaches zero, so a subtree
another tree still holds stays.

While the parser builds a node, its place among the nodes being built is a
builder's (`markdown_core_member`): its owner, its siblings, its children and
the field roots it builds. Once the node's structure is complete the builder
freezes: the nodes of its children become the node's stem, which takes the
references their builders held. Builders live for one parse, in its pool.

A completed node holds every byte string it reads: numbering copies the
views its record borrows from the parse (`markdown_core_node_hold_strings`),
and a resource copies a borrowed destination or title, so a node the next
revision shares reads nothing another node owns.

A session's parse continues the previous document (5.9). Each node decides
which old node it continues, and takes that node's id or the next, as it
settles, when its kind and its range are final. When it equals that old
node -- kind, scalars, extent, runs, and every relation holding the same
objects -- the old node takes its place in its owner and the new node is
released, so an unchanged subtree is the old object.

Reference-map records are not slots. Every record lives exactly as long as
its map, so records are carved from blocks the map owns and freed with it,
rather than allocated one each.

All block, inline, and manual construction uses the same node constructor.
It takes one slot for the node and its kind's record, establishes defaults,
and only then exposes the node. Failure releases all acquired storage; a slab
that cannot be allocated refuses the node and leaves the pool usable.
Node data and its strings use the library's allocator.

Initialization clears the whole node and exactly the active inline record
(including the alignment gap before it). Spare record capacity has no live
object and is not read or initialized. Records larger than the slot's capacity
are separately zero-allocated. Fresh, recycled and standalone slots use this
same constructor; the pool's slab header remains outside object initialization.

An empty node's content borrows the strbuf sentinel. Creating a block does not
reserve content storage; the first write acquires it through the ordinary
buffer growth operation. The streaming line writer acquires the former
32-byte minimum reservation on its first nonempty append, reserving the whole
write (including any partial-tab expansion) in one growth; producers of already
delimited values continue to use ordinary writes through the same strbuf API.
This preserves the established streaming growth policy without allocating
for blocks that never receive content. Successful growth always establishes `ptr[size] == 0`,
including the first allocation, which cannot copy the sentinel's NUL byte.
Failed growth preserves the old storage and terminated value and records OOM.

`CrossLink` stores a `markdown_core_cross_reference` record containing its raw
path, optional anchor, and optional label. `CrossEmbedded` stores a
`markdown_core_cross_embedded` record containing the same reference fields and
optional dimensions. The two kinds share the reference layout, while each
occurrence owns its string chunks; optional presence remains independent of
string length. The node kind identifies a link or transclusion, with no
embedded flag in either record.

Only `Embedded` and `CrossEmbedded` expose `Dimensions(width, height?)`. The
optional value is stored inline in the occurrence's typed record, without a
separate allocation, and its lifetime ends with that record. Cross references
own their raw destination fields directly.

Parser construction links a detached builder under its owner
(`markdown_core_member_attach`) once the containment decision is made; it
asserts the pure built-in containment rule in Debug/ASan. Built-in elements
declare the parent-kind domain retained by their payload across conversion;
an unrelated kind cannot silently inherit a different containment policy.
Dynamic callbacks remain decision operations and are never replayed by an
assertion. Internal inline constructors must return a detached token
admitted by their fixed grammar owner. The private element API states this
obligation; arbitrary third-party descriptors are not an installed or
supported extension surface. Rejection leaves both trees unchanged. Optional rewrites, including formula
promotion and email splitting, validate before allocating or consuming the
old node; rejection preserves the authored content. A constructed inline
token rejected by its destination remains owned by the parser and is released
before the parse fails with `MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED`.
Parser and inline transactions use the same error enum; allocation failures use
`MARKDOWN_CORE_PARSE_ALLOCATION_FAILED`, and propagation preserves the first
cause. Buffer allocation flags remain allocation-only. Table lead splitting
checks acceptance before conversion, so a refused optional split preserves the
complete original paragraph. There is no per-node allocator identity to check.
Kind conversion changes no edges and checks only containment.
There is no safety mode, ancestry cache, or separate inline splice algorithm.
Regression inputs vary nesting depth and autolink count independently.

Kind conversion preserves node identity and children. After containment
validation, it reserves an external replacement before releasing the old
fields if the new record exceeds slot capacity. A record that fits already
has storage: the conversion releases the old fields, zeroes the new active
record in the slot, establishes defaults and commits the new kind. These last
operations cannot fail and never overwrite a still-live old field. The old
record is freed only when it was external; slot storage stays with the node.
The typed view and allocation ownership are explicit: `as` points to the
current record, while `node_data_allocation` owns whichever record is not in
the slot, if any, because it exceeds the slot's record space. Ownership is
never inferred by comparing potentially adjacent
addresses.
`markdown_core_node_set_kind` distinguishes containment rejection from allocation
failure. Parser callers decline rejected conversions and set the OOM flag only
for allocation failure. Either failure leaves the original kind and all owned
values intact. A successful conversion releases node-valued fields through the
same iterative destruction walk used for ordinary tree destruction. The
element's opaque state belongs to the node and element, so it survives a
kind conversion. So does the attribute value.

HTML blocks keep their recognition state and eventual literal in distinct
fields of one data record throughout parsing. Converting a closed HTML comment
to Comment transfers its owned literal only after the new record can be
created. Setext headings also use the shared kind conversion operation.

Construction and kind conversion use one record capacity/ownership model.
Their initialization order differs because conversion must destroy the old
fields before reusing their bytes, while construction has no old live fields.
All kinds use these same lifecycle rules. No per-kind pools, packed field offsets, or
cardinality-dependent storage paths are needed. Benchmarks measure parse time,
allocation work, and memory independently of the deterministic layout tests.

Tests protect the pointer-sized union, constructor allocation failures,
transactional kind conversion, containment rejection in parser conversions,
owned subtree release, whole-parse OOM propagation, and the pool's claims in
allocator counts: one allocation per slab of many slots, a released slot
reused before a slab is touched, a refused slab refusing only the node, and a
slab freed by the last of its slots after the pool is gone. Platform builds verify
native alignment, and sanitizer suites exercise the same ownership paths.

Link reference definitions are recognized during block parsing so paragraph
content and Setext classification can use the remaining text. Each definition
becomes a `Reference` node in its parent's child chain where it was written,
before what remains of the paragraph. A paragraph that held only definitions
is released when it is finalized; its References stay, so later block
identifiers see them in source order and cannot attach across them. The
blank-line facts skip References, and list layout, the list's close step,
reads the semantic children around them. An
intentionally empty anchored list-item paragraph is not a definition and
stays.

Inline footnotes use the existing Footnote data record and one-item Cite.
A successful close moves the parsed inline body into a Footnote that the
Cite's Citation owns as its `note` field, as it owns its affixes. Authored
definitions are Footnote and Specimen blocks in the tree where they were
read. In both cases the tree owns the node, including on parse failure.

Calls resolve through the parser's label maps while parsing; a footnote label
map and a specimen key index hold the first definition of each label. A
definition is numbered where it is, like any other node.

A node is complete when it is made. A block settles as it closes: its
element's `finalize_block` runs, its runs are placed, and it completes. A
block settles once every block it holds has settled; one that closes while the
last block under it is still open (a new list closes the old one before its
items) settles as that block does. Completing a node numbers each node it holds
that is not numbered yet, in canonical field order: its extent, measured from
the end of the node before it in its relation or from the owner's start. A
numbered node settles once nothing it waits on is pending -- its inline root,
its block input, its anchor, the nodes it holds -- and takes its id as it
settles. Its parse-time place becomes its extent. A Footnote,
Specimen, Reference or Heading enters the document's definition tables at its
source start as it is numbered, and a node holding inline content is queued as
an inline root. Completion is idempotent: a node that gains a node later (a
table gaining a trailing caption) completes again and numbers only the new
one.

After block parsing the document is prepared; then each inline root, in the
order it was queued, parses its content and completes its tree in one pass,
and the root completes last. The document settles last when the tree is
published, so a fresh parse's ids are 1 through its node count in the order
its nodes settle and the document holds the last id. A numbered node holds only its
extent, so headings and specimen definitions record their source start when
they register (`markdown_core_source_entry`) and are ordered by that start.

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

Allocation failure and semantic refusal can occur in one token construction.
The transaction retains the first cause: a constructor may return an owned
node after its literal allocation failed, and a later policy refusal releases
that node without replacing the allocation error. Setup callbacks run only
after the initial parser structures have been created successfully.
