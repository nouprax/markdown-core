# Node storage and lifetime

The engine node contains its tree links, source mapping, attributes, extension
state, and a union of typed payload pointers. Every union arm is a pointer;
adding fields to one kind cannot enlarge the common node. A kind with no
type-specific fields has no payload allocation. Field-bearing kinds own a
record containing their ordinary typed fields. Construction allocates the node
and its record together, with the typed pointer referring directly to that
record. A C99 allocation header provides scalar alignment for both objects;
allocation-header padding is included in measured memory costs.

All block, inline, and manual construction uses the same node constructor.
It makes one allocation for the node and its kind's record, establishes
defaults, and only then exposes the node. Failure releases all acquired storage. Payloads and
their strings use the node's allocator. CrossLink keeps its explicit path,
optional anchor, optional label, and embedded flag, with ordinary owned
chunks; optional presence remains independent of string length.

Type conversion preserves node identity and tree links. After containment
validation, it allocates a replacement record before releasing the old fields.
The original record shares the node's allocation and is reclaimed with the
node; replacement records are freed when replaced or when the node dies.
The typed view and allocation ownership are explicit: `as` points to the
current record, while `payload_allocation` owns a replacement allocation, if
any. Ownership is never inferred by comparing potentially adjacent addresses.
Failure leaves the original type and all owned values intact. A successful
conversion releases node-valued fields through the same iterative
destruction walk used for ordinary tree destruction. The extension's opaque
state belongs to the node and extension, so it survives a type conversion.

HTML blocks keep their recognition state and eventual literal in distinct
fields of one payload throughout parsing. Converting a closed HTML comment
to Comment transfers its owned literal only after the new payload can be
created. Setext headings also use the shared type conversion operation.

Construction and type conversion have different ownership constraints: an
unpublished node and its initial record can share an allocation, while a
replacement record must preserve the existing node's address. All kinds use
these same lifecycle rules. No per-kind pools, packed field offsets, or
cardinality-dependent storage paths are needed. Benchmarks measure parse time,
allocation work, and memory independently of the deterministic layout tests.

Tests protect the pointer-sized union, constructor allocation failures,
transactional type conversion, owned subtree release, and whole-parse OOM
propagation. Platform builds verify native alignment, and sanitizer suites
exercise the same ownership paths.
