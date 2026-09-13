# Swift value storage

The public AST is immutable and `Sendable`. A parse copies each native value
exactly once into a flat `StoredMarkup` array, then frees the C document before
returning. Records contain owned scalar values and integer relation indices.
They never contain a container view or a reference back to `ValueTree`.
The unique Document payload is stored indirectly: its inline Metadata value
must not determine the stride of every ordinary node in the record array.
That one box contains scalars and indices, so it adds no recursive tree edge.

A container or scoped value is a small `(ValueTree, index)` view. Its scalar
fields read that record; its node relations are `MarkupCollection<Element>`
values carrying the same store and a copy-on-write array of indices. Count,
indexing, and obtaining the relation view are O(1), with no materialization of
descendants. Iteration visits the requested elements. `Array(relation)` explicitly
materializes an array for consumers that require one. Leaves carry their scalar
values directly. The exhaustive stored enum prevents a recursively owned
container from entering storage through an erased `any Markup` field.

The projection queue holds native handles only during construction. Indices
refer to queue positions, so no partially initialized semantic nodes or repair
pass are needed. Each facade ownership relation has one queue edge: generic
children, labels, captions, callout titles, definition terms and bodies,
footnotes, specimens, and citation affixes. Reference resources are copied once
per native identity and remain shared scalar resources across occurrences.

This ownership graph has bounded ARC destruction depth. Releasing the last
store owner destroys a flat array of scalars and index arrays; it cannot recurse
through tree edges. A retained container or relation keeps the whole immutable
Swift store alive until its last owner is released. This is an explicit lifetime
tradeoff: extracting a subtree does not copy it or sever it from the store.
There is no native handle, mutation, lazy cache, cleanup queue, or lock.

The former `[Node]` child properties are now `MarkupCollection<Node>` (including
`MarkupCollection<any Markup>` and collections of definition-body collections).
Callers using array-specific APIs should explicitly construct `Array(...)`.
Property names, ordering, scalar semantics, visitors, and native coordinates are
unchanged. Tests cover canonical projections, shared-resource identity, public
collection consumption, concurrent reads, and normal root/subtree release at
30,000 and 65,536 levels. The release tests assert that the store is reclaimed;
they do not keep the next child alive to manually dismantle ancestors.
