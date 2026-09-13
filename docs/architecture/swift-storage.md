# Swift value storage

The public AST is immutable and `Sendable`. A parse copies each native AST value
exactly once into a flat `StoredMarkup` array, then frees the C document before
returning. Records contain owned scalar values and integer relation indices.
They never contain a container view or a reference back to `MarkupStore`.
The unique Document payload is stored indirectly: its inline Metadata value
must not determine the stride of every ordinary node in the record array.
That one box contains scalars and indices, so it adds no recursive tree edge.

A container or scoped value declares `@Stored var fields: Fields`. `Stored`
is a typed value reference containing the store and a private record index;
it queries the payload without storing or caching a copy. `MarkupStore` owns
the exhaustive field lookup, node projection, and collection construction.
Individual markup types do not inspect records, match storage enum cases,
or implement index checks. The wrapper adds no heap allocation or synchronization.
The index identifies an occurrence within a particular store; it is not a
source coordinate, and querying by kind alone cannot distinguish two paragraphs.

Each container or scoped type declares its nested `Fields` first, followed by
`@Stored var fields: Fields`, then its public properties and methods. Native
conversion initializers remain in `extension Type.Fields` blocks.

Scalar properties read through `fields`; `$fields` exposes the owning store
for relation queries. Node relations are `MarkupCollection<Element>` values
carrying that store and a copy-on-write array of indices. Count,
indexing, and obtaining the relation view are O(1), with no materialization of
descendants. Iteration visits the requested elements. `Array(relation)` explicitly
materializes an array for consumers that require one. Leaves carry their scalar
values directly. The exhaustive stored enum prevents a recursively owned
container from entering storage through an erased `any Markup` field.

Collections are stored directly in their owning node's fields. Callout and
directive content use `[Int]`; definition content uses `[[Int]]` to preserve
body boundaries, including empty bodies. A `MarkupGroups<Element>` view shares
the outer array and returns a `MarkupCollection<Element>` for each inner array
in O(1), without mapping or allocating the groups on access. Groups have no
record, queued handle, or stored-kind dispatch of their own. Nested definitions
still refer to nodes by index, so array nesting is bounded by the field's shape,
not the depth of the document.

The projection queue holds native handles only during construction. Indices
refer to queue positions, so no partially initialized semantic nodes or repair
pass are needed. Only markup and scoped AST values enter the queue. Their owned
relations enqueue the referenced nodes: generic children, labels, captions,
callout titles, definition terms and the blocks in each body, footnotes,
specimens, and citation affixes. Reference resources are copied once
per native identity and remain shared scalar resources across occurrences.

This ownership graph has bounded ARC destruction depth. Releasing the last
store owner destroys a flat array of scalars and index arrays; it cannot recurse
through tree edges. A retained container or relation keeps the whole immutable
Swift store alive until its last owner is released. This is an explicit lifetime
tradeoff: extracting a subtree does not copy it or sever it from the store.
There is no native handle, mutation, lazy cache, cleanup queue, or lock.

The former `[Node]` child properties are now `MarkupCollection<Node>` (including
`MarkupCollection<any Markup>`); `Definition.content` is `MarkupGroups<any Markup>`.
Callers using array-specific APIs should explicitly construct `Array(...)`.
Property names, ordering, scalar semantics, visitors, and native coordinates are
unchanged. Tests cover canonical projections, shared-resource identity, public
collection consumption, concurrent reads, and normal root/subtree release at
30,000 and 65,536 levels. The release tests assert that the store is reclaimed;
they do not keep the next child alive to manually dismantle ancestors.
Grouped-relation tests cover empty and multiblock bodies, wide definitions,
concurrent reads, and the last release of retained outer and inner collections.
Typed-reference tests distinguish same-kind occurrences and different stores
after the roots are released. Layout checks keep container views within the
existential inline buffer, including the root with its large metadata payload.
