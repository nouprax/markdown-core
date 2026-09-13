# Key index

The private index shared by references, heading anchors, resource identities,
footnotes, specimens and citation resolution is a compressed binary radix tree.
There is one index algorithm for all key lengths, insertion orders and clients.

## Keys and lifetime

A key is a borrowed, length-delimited byte string. Embedded NUL and the empty key
are representable; normalization belongs to each caller. The index neither
copies nor frees keys or values. A slot contains either a caller-owned pointer
or a counter. An occupied lookup never reallocates. A vacant `entry` may grow the
vector, and must be `commit`ted before any other index operation. `commit` supplies
the durable key bytes and publishes the prepared edge without allocating.

Reference winner selection stays in `index_map`: explicit declarations precede
implicit headings, then the earliest source key wins. Tree shape does not affect
semantic ordering. Heading suffix counters and shared resource identities retain
the same entry lifetime rules.

## Representation and work bound

Each key byte contributes a presence bit followed by its eight value bits.
End-of-key has no presence bit. Thus a key and its extension differ even if the
extension is NUL. A branch stores the first distinguishing byte offset and mask;
every edge to another branch strictly increases the offset or decreases the mask
at the same offset. No branch is allocated for a shared prefix.

A lookup descends at most `9 * query_length + 1` branches and compares one leaf.
If a branch lies beyond the query's terminator, every key below it is too long;
its resident leaf supplies the absent-key check (or the insertion split), without
walking a suffix the query cannot share. Insertion finds that first differing
bit, then walks the same ordered branch space to publish its new branch. Total
work is O(sum of key byte lengths), plus amortized O(number of keys) vector growth.
This bound is independent of hash distributions and insertion order.

One dense record contains one slot and the branch created by its insertion
(the first record has no branch). Child references encode record indices and a
leaf bit, so geometric growth copies records without reinserting or rehashing.
The record is 48 bytes on the measured arm64 ABI. Initial allocation is lazy
when expected size is zero; the first allocation is eight records, 384 bytes,
versus the former sixteen hash slots, 512 bytes. Capacity never includes a
separate hash load-factor reserve.

## Failure and verification

All size arithmetic is checked before allocation. Failed growth leaves the
existing vector and published root intact; parser callers make allocation loss
sticky and destroy the transaction. No externally visible parse API changes.
Index destruction frees one allocation and never recursively walks its branches.

`key_index_radix` tests binary keys, prefix chains including the empty key,
ascending/descending/permuted insertion, replacement and duplicate handling. It
checks every branch edge's strict bit progress and valid reference, establishing
the structural bound rather than a timing threshold. The pathological reference
workload now targets the actual **baseline** FNV/finalizer hash instead of cmark's
unrelated sdbm hash. The comparison experiment replays these collisions through
reference, heading, footnote and specimen consumers and compares complete dumps.
OOM injection and sanitizers cover parse transaction failure and release.
