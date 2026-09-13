# Key index

The private index shared by references, heading anchors, resource identities,
footnotes, specimens, citation resolution and attribute facts is a compressed binary radix tree.
There is one index algorithm for all key lengths, insertion orders and clients.

## Keys and lifetime

A key is a borrowed, length-delimited byte string. Embedded NUL and the empty key
are representable; normalization belongs to each caller. The index neither
copies nor frees keys or values. A slot contains either a caller-owned pointer
or a counter. An occupied lookup never reallocates. A vacant `entry` may grow the
vector, and must be `commit`ted before another index operation. `commit`
borrows immutable bytes equal to the query and publishes the prepared edge
without allocating. OOM is terminal for the owning parse. Destruction can abandon
a pending entry after a caller allocation failure; recovery and retry are not
part of the contract. Reentry and invalid commits abort in every build mode. The reentry
check happens before touching the prepared node: a commit-time pointer check
alone cannot distinguish two entries that reuse the same vector position.

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
when expected size is zero; the first allocation is one record, 48 bytes. All consumers use the same
geometric growth rule. Capacity never includes a
separate hash load-factor reserve.

## Failure and verification

All size arithmetic is checked before allocation. Failed allocation is reported
to the owner, which makes the error sticky and destroys the complete parse
transaction. A failed `realloc` retains its allocation for that cleanup; this is
not a promise that parsing can continue. No externally visible parse API changes.
Index destruction frees one allocation and never recursively walks its branches.

`key_index_radix` tests binary keys, prefix chains including the empty key,
ascending/descending/permuted insertion, replacement and duplicate handling. It
checks every branch edge's strict bit progress and valid reference, establishing
the structural bound rather than a timing threshold. The pathological reference
workload now targets the actual **baseline** FNV/finalizer hash instead of cmark's
unrelated sdbm hash. The comparison experiment replays these collisions through
reference, heading, footnote and specimen consumers and compares complete dumps.
The collision replay is historical semantic coverage, not a timeout-based
complexity gate. A separate `reference_unresolved_scale` case retains 49,999
definitions and missing-reference paragraphs, checking literal text, node counts
and complete document release without reducing the former document scale.
`key_index_adversarial` builds prefix-first, extension-first and
permuted sets at `MAX_LINK_LABEL_LENGTH` (1,000 bytes), with a neighbour for every
presence/value bit. It counts structural branch visits, verifies strict progress
and reaches a 9,000-branch path. Thus long shared prefixes and the full key-length
bound are exercised without adding instrumentation to production lookup loops.
`key_index_failure` injects growth and caller key-allocation failures at
capacities 1 through 128 and verifies complete release, including a prepared
entry whose key allocation failed. Separate CTest processes require SIGABRT for reentry, wrong-slot, null-key
and repeated commits, including Release/NDEBUG. OOM injection and sanitizers
also cover parse transaction failure and release.

## Third-party hash-table candidates

Vendoring a hash table is possible, but a replacement needs evidence from the
actual parser consumers. Neither candidate below establishes the deterministic
key-length work bound used here:

- [khash](https://raw.githubusercontent.com/attractivechaos/klib/master/khash.h)
  permits custom hash/equality and reports allocation failure. Its probing can
  still visit many entries under collisions. Adapting allocator ownership and
  byte-slice keys is required.
- [stb_ds](https://raw.githubusercontent.com/nothings/stb/master/stb_ds.h) offers
  a stronger SipHash option, but its global mutable seed, unchecked allocation
  paths and allocator context require changes for this parser's concurrency and
  sticky-OOM contracts.

No third-party implementation has been benchmarked or adopted in this change.
A future comparison should measure complete parse/free, live and requested bytes,
misses, duplicate keys, long common prefixes and collisions for each actual hash,
then run allocator-failure and concurrent-parse tests. Randomized hashing alone
would not preserve the current deterministic worst-case guarantee.
