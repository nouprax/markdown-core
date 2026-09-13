# Attribute recognition

The baseline parser recognizes attributes on demand over an immutable source
extent. A single forward member recognizer serves headings, spans, links,
references, code and directives. Values are decoded only when their owner
commits. The public parse operation remains one-shot.

## Facts and ownership

A private fact contains a source offset, a member-suffix result and an unquoted
value boundary. An unresolved result is distinct from failure (`0`) and a
successful exclusive end. The parser's radix index uses fixed-width offset keys;
records and their borrowed keys live in geometrically growing, stable arenas.
The index may move without moving keys. No source byte requires an index entry.

Facts are created for queried candidates, joins after values and braces crossed
while scanning bare values. They do not own decoded names, classes or records.
A caller owns one parser for its immutable extent and releases it once. Heading
suspension transfers that owner along with the inline state; it does not clone
its allocations. Directive block recognition and value commitment share the
parser held by the parsed opener. Definition-list reference probes recognize
without allocating attribute values.

A physical-line probe and a later assembled paragraph are different source
extents. In particular, a quote unresolved at line EOF can close in the paragraph.
Failure facts cannot be shared across those extents merely because their prefixes
match. With demand-driven facts, reference parsing touches its queried definition
attributes; subsequent inline parsing does not rebuild an index over those
consumed definitions or the whole remaining paragraph. Cross-version reuse belongs
to the source/dependency model described below, not a cache keyed by a raw pointer.

## Recognition and work

The recognizer advances through members without a recursive C stack. When a value
ends, the remaining member grammar is memoized at that join. A resolved join
finishes all pending member suffixes with the same answer. Grammar positions
strictly advance, so a pending chain cannot cycle.

Bare values need their own shared lexical fact. For example, many nested
`{k=` prefixes may all reach one space and then a long list of classes. A scanner
records the unquoted continuation after every brace it crosses. An earlier or
later candidate can therefore reuse both the bare-value boundary and the grammar
suffix after it. Memoizing just successful containers would miss both sources of
quadratic work.

Quoted values obey the existing priority: the first unescaped matching quote
before a blank line wins, even if a brace precedes it. Without that quote, the
value falls back to the bare-value grammar. An opening value quote immediately
follows `=`; it cannot be escaped by the preceding value. For each quote kind,
scans from distinct assignments do not overlap: the next opening quote would
close the earlier scan. Revisited assignments are covered by member-join facts.
CRLF is one line ending; horizontal whitespace between two line endings forms a
blank line. Escapes hide ASCII punctuation, including braces and quotes.

For a source extent of N bytes, A queried/encountered candidates and J value joins:

- recognition performs O(N + A + J) total work, independent of query order;
- each repeated success or failure uses a fixed-width radix lookup, with no scan
  or allocation;
- storage is O(A + J), including geometric arena and index growth;
- committed decoding additionally costs its output/source token bytes. Asking to
  decode many overlapping successful values necessarily pays for those outputs.

The N term is an upper bound, not an unconditional scan. A single `{.c}` or `{?}`
at either end of a large paragraph uses constant work and storage. Long names or
values are inspected as grammar requires. ASCII scalars bypass UTF-8 decoding;
Unicode names and whitespace retain the same categories.

Class and record vectors grow geometrically from one element. The shared radix
index also grows from one record for every consumer; there is no short-attribute
algorithm, source-size threshold or hash-distribution shortcut.

## Failure and verification

An allocation failure is sticky and aborts the owning parse. An index entry is
published only after its stable record exists. Unpublished entries, partially
built arenas, pending facts and partially decoded values are all released through
the ordinary ownership path. Recognition failure never publishes partial values.

Native tests cover constant sparse storage/work, cumulative requested bytes and
peak live bytes, overlapping lexical runs and shared grammar tails, reverse and
permuted queries, repeated failures, Unicode, quote fallback, escaping, heading
suspension and allocator failure. A reproducible differential experiment compares
all queried ends against the former reverse DP; complete AST dumps and full
parse/free measurements cover the consumers. See
[the experiment report](../reviews/2026-09-13-inline-performance-fixes.md).

## Continuation boundary

These are forward grammar facts, but they are currently scoped to immutable input.
They are not yet an append API or a promise that an EOF result is final. For
`{k='}`, appending a later quote may change an already successful bare-value
fallback. The following work belongs to incremental milestone I52:

- give source extents versioned identities and stable offsets;
- retain the member cursor, pending join chain and lexical cursor when a scan
  reaches the live frontier;
- record positive and negative reads, including the quote search used to select
  bare-value fallback; distinguish an actual blank-line boundary from EOF;
- resume lexical states for escapes, CRLF and quoted values, invalidate dependent
  provisional results, and propagate changes through joins;
- preserve immutable results for retained snapshots, and compare every append
  prefix and editor splice with a fresh one-shot parse.

A copied `data` pointer, a completed container end or a finite lookbehind window
alone cannot establish these dependencies. Source/version ownership must exist
before cross-stage or cross-update facts can safely outlive their current extent.
