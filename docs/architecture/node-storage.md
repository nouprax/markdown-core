# Node storage and lifetime

The engine node contains its tree links, source mapping, attributes, extension
state, and a union of typed node data pointers. Every union arm is a pointer;
adding fields to one kind cannot enlarge the common node. A kind with no
kind-specific fields has no data record. Field-bearing kinds own a
record containing their ordinary typed fields. Construction allocates the node
and its record together, with the typed pointer referring directly to that
record. A C99 allocation header provides scalar alignment for both objects;
allocation-header padding is included in measured memory costs.

All block, inline, and manual construction uses the same node constructor.
It makes one allocation for the node and its kind's record, establishes
defaults, and only then exposes the node. Failure releases all acquired storage.
Node data and its strings use the node's allocator. CrossLink keeps its explicit
path, optional anchor, optional label, and embedded flag, with ordinary owned
chunks; optional presence remains independent of string length.

Kind conversion preserves node identity and tree links. After containment
validation, it allocates a replacement record before releasing the old fields.
The original record shares the node's allocation and is reclaimed with the
node; replacement records are freed when replaced or when the node dies.
The typed view and allocation ownership are explicit: `as` points to the
current record, while `node_data_allocation` owns a replacement allocation, if
any. Ownership is never inferred by comparing potentially adjacent addresses.
`markdown_core_node_set_kind` distinguishes containment rejection from allocation
failure. Parser callers decline rejected conversions and set the OOM flag only
for allocation failure. Either failure leaves the original kind and all owned
values intact. A successful conversion releases node-valued fields through the
same iterative destruction walk used for ordinary tree destruction. The
extension's opaque state belongs to the node and extension, so it survives a
kind conversion.

HTML blocks keep their recognition state and eventual literal in distinct
fields of one data record throughout parsing. Converting a closed HTML comment
to Comment transfers its owned literal only after the new record can be
created. Setext headings also use the shared kind conversion operation.

Construction and kind conversion have different ownership constraints: an
unpublished node and its initial record can share an allocation, while a
replacement record must preserve the existing node's address. All kinds use
these same lifecycle rules. No per-kind pools, packed field offsets, or
cardinality-dependent storage paths are needed. Benchmarks measure parse time,
allocation work, and memory independently of the deterministic layout tests.

Tests protect the pointer-sized union, constructor allocation failures,
transactional kind conversion, containment rejection in parser conversions,
owned subtree release, and whole-parse OOM propagation. Platform builds verify
native alignment, and sanitizer suites exercise the same ownership paths.

Inline footnotes use the existing Footnote data record and one-item Cite.
During parsing, the Cite owns its Footnote as a structural child; the
Footnote owns the parsed inline body directly. Referenced definitions remain
at their block positions through the same phases. This temporary ownership
keeps consolidation, autolinking, extension-owned label traversal, and failure
cleanup on the ordinary tree algorithms without a second body registry.

Document finalization collects both forms from all owned trees before making
any mutation. It orders the F values by source start with eight stable byte
passes over their two 32-bit coordinates, bounding ordering work by O(F). It
reserves all authored ids in the shared key index and assigns inline ids in
that order. Collision probes across all inline ordinals consume disjoint authored
id namespaces, so their total is bounded by F plus the authored-id count.
Only after all allocations succeed does it transfer every Footnote into the
document's one value chain. A returned Cite has no structural child and owns
only Citation values naming ids; semantic cycles never become object cycles.
The bracket scanner tracks the most recent non-SP/TAB byte over disjoint
consumed token ranges, so rejecting empty bodies never rescans nested bodies.
