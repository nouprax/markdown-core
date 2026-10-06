# Swift value storage

The public AST is immutable and `Sendable`. A parse copies each native node
exactly once into one record, then frees the C document before returning. No
record holds a native pointer, a container view, or a reference to its parent.

Every Markup kind is a struct holding `let record`, a final class of the kind
that inherits the internal base `MarkupRecord`. The base holds what every kind
has (`id`, `extent`, `runs`, `anchor`, `attributes`) and every owned child record, in
one array in canonical walk order across the node's relations. A kind adds
only `let` scalars and says how its relations partition that array
(`relation(at:)`): a table's caption, head, body and foot; a citation's inline
note, prefix and suffix; a definition's term and each body. Public getters
return views of that array; they never copy descendants. Liveness is ARC: a
retained node keeps its own subtree alive and nothing else.

`MarkupCollection<Element>` is a slice of the owner's child records. Count,
indexing and obtaining the view are O(1); iteration projects each element as
it is read, and `Array(relation)` materializes one on request.
`MarkupGroups<Element>` is the owner and the group boundaries, and returns a
`MarkupCollection<Element>` per group in O(1), keeping empty groups. Groups
have no record of their own.

NO OPERATION RECURSES OVER TREE EDGES. Release, equality, the walker,
conversion, scope queries, hit testing and the dump each keep an explicit
work stack, so the call stack is constant in tree depth. Release is the
base's `deinit`: it moves its children into a local array and drains it,
first moving the children of every child that `isKnownUniquelyReferenced`
reports unshared, so no `deinit` has anything left to release. A child a view
still holds is only released, which ends at a count decrement. That move is
the only write to a record, and it happens only to a record nothing else
references, which is why records are `@unchecked Sendable`; the invariant is
stated on `MarkupRecord`, and each subclass restates the conformance as
Swift requires.

Equality is deep value equality: kind, id, extent, runs, anchor,
attributes, the kind's scalars and pairwise-equal children in every relation,
checked from a stack of record pairs with an identity shortcut. Hashing reads only the id.
Every kind is `Hashable` and `Identifiable`; `isEqual(_:)` compares two
`any Markup`. `description` is the kind and id; `dump(in:)` draws a tree.

The conversion queues native nodes breadth first, so each node's children
follow it, and builds records from the last queued node back to the root; no
partially built node is ever visible. Only Markup nodes enter the queue.
Each node's fields, attributes included, are the ones written on it, read
from that node alone.

A document stores its text unit and its definition tables: every footnote
(definitions and inline notes), every specimen and every reference in source
order, as records of the tree, and a map from each label's UTF-8 bytes to its
first definition, built with the document. The reference map is copied from
the engine's label table, so each resolving label names the `Reference` or
`Heading` record the engine resolves it to; the binding normalizes no label.
A label is never compared under Unicode equivalence. There is no lazy cache
and no lock.

Scopes are not stored. `scope(of:in:)` walks the document once to the node's
source ranges, a window less the gaps between the runs that place it: a
block's range and its own runs, or the source a node inside an inline root's
content was read from and the root's runs. It converts each range
with the source's line starts to lines and columns in the document's unit;
`node(at:in:)` converts the position to a byte offset and returns the last
node in walk order one of whose ranges holds it. Both
mirror the C engine's rule exactly. The dump computes its scopes the same
way, always in UTF-8 columns. Each checks its argument once, at the public
function, and throws `MarkdownCoreError` with `.outOfBounds` as C does: a
scope or dump whose source ends before the node does, and a position whose
line or column is below 1. The `SourceLines` helpers beneath them assume that
check.

Tests cover canonical dumps, fresh-parse ids numbered 1 through n in
completion order, deep equality, both units' scopes and hit testing, the definition
tables and reference resolution, concurrent reads, and the last release of
retained groups. The 30,000 and 65,536-level trees run on a thread with a
512 KiB stack: release while a view holds a subtree, equality at the deepest
leaf, walking, scope lookup, hit testing and `description`.
