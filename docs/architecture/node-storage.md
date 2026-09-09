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
Node data and its strings use the node's allocator.

`CrossLink` stores a `markdown_core_cross_reference` record containing its raw
path, optional anchor, and optional label. `CrossEmbedded` stores a
`markdown_core_cross_embedded` record containing the same reference fields and
optional dimensions. The two kinds share the reference layout, while each
occurrence owns its string chunks; optional presence remains independent of
string length. The node kind identifies a link or transclusion, with no
embedded flag in either record.

Only `Media` and `CrossEmbedded` expose `Dimensions(width, height?)`. The
optional value is stored inline in the occurrence's typed record, without a
separate allocation, and its lifetime ends with that record. A `Media` node
retains its own dimensions even when its destination and title come from a
resource shared with other resolved references. Cross references own their raw
destination fields directly and do not share a resource with a definition.

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
parse after the main tree. Eight stable byte passes over the two 32-bit source
coordinates bound ordering work by O(F). All authored ids are reserved before
inline ids are assigned. Collision probes consume disjoint authored-id
namespaces, so their total is bounded by F plus the authored-id count.
After every allocation succeeds, finalization moves the values into one
source-ordered document chain and discards the parser collection.

Consolidation and extension postprocessing begin only after finalization.
Their common tree-phase walker visits Document.footnotes and extension-owned
fields from their live owner slots. Callbacks receive resolved ids and the
completed ownership model; removing a document value cannot leave a pointer
in a parser index. OOM cleanup uses the document's existing ownership graph,
and semantic reference cycles never become object cycles.

The bracket scanner tracks the most recent non-SP/TAB byte over disjoint
consumed token ranges, so rejecting empty bodies never rescans nested bodies.
Only `^[` terminates an ordinary text run; other carets incur the same
allocation work as other text. Bare autolinks use the enclosing inline
context's start and closing delimiter, preserving the footnote boundary.
