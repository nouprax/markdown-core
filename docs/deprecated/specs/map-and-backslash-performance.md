# Map and consecutive-backslash performance design

Status: awaiting review

Date: 2026-07-14

Scope: C parser core, directive attribute normalization, complexity gate

## Decision

This performance fix addresses two independent problems exposed by the same
end-to-end tests:

1. Replace the reference/footnote map inherited from cmark-gfm, which performs
   a full `qsort` on first lookup and then `bsearch`, with a shared byte-key
   open-addressing hash index. Directive attribute deduplication reuses it.
2. When no extension handles `\\`, decode consecutive escaped-backslash pairs
   into one Text node, avoiding tens of millions of temporary nodes that would
   later be consolidated at parser finish.

Neither change alters observable semantics. Maps preserve first-definition-wins
for references/footnotes and first-position/last-value-wins for directives.
Backslash changes preserve CommonMark escape results, source scope, and
extension-dispatch precedence.

## Background and boundaries

### Map origins and consumers

`markdown_core_map` came from cmark-gfm in the 2026-07-11 baseline; it was not
introduced for directives or formulas. Its direct consumers before and after
the change are:

- reference definitions;
- footnote definitions.

Formulas do not use it. Directives do not adopt `markdown_core_map` semantics
such as reference-label normalization or expansion budgets; they reuse only the
general `markdown_core_key_index` extracted in this change.

The two layers are intentional:

- `markdown_core_key_index`: a byte-key indexing primitive that owns neither
  keys nor values;
- `markdown_core_map`: reference/footnote label normalization, duplicate-definition
  rules, and expansion accounting on top of the index.

This is not public C API. Types and functions are declared in core-private
`map.h`, excluded from installed headers and platform bindings.

### Non-goals

- Provide a general container library or arbitrary-type dictionary.
- Change reference-label Unicode case folding, edge-whitespace trimming, or
  internal-whitespace folding.
- Define output order through hash iteration order.
- Restore directive HTML-style `#id`/`.class` shortcuts or special id/class
  merging semantics.
- Change extensions' priority in handling backslash special characters.

## 1. General byte-key index

### Before the change

The inherited implementation stores definitions in a singly linked list. On
first lookup it:

1. allocates a pointer array for every entry;
2. runs `qsort` by normalized label and source age;
3. compacts duplicate labels in place;
4. uses `bsearch` for each lookup.

Preparation is therefore O(n log n). Earlier directive deduplication likewise
built and sorted a pointer array for every attribute. The old path measured
4.442 normalized slowdown in the 4 KiB → 128 MiB duplicate-attribute endpoint
test.

### Data structure

`markdown_core_key_index` is an open-addressing table with power-of-two capacity:

```c
typedef struct markdown_core_key_index_slot {
    uint64_t hash;
    const unsigned char *key;
    bufsize_t key_len;
    void *value;
} markdown_core_key_index_slot;
```

Constraints:

- Minimum capacity is 16.
- Capacity is always a power of two, so bucket selection and wrapping use masks.
- Load factor never exceeds 0.5.
- Linear probing examines at most 64 slots per operation.
- Probe exhaustion during insertion first triggers one transactional doubling
  and retry; only another failure is reported to the caller.
- Hashing uses byte-by-byte 64-bit FNV-1a followed by avalanche mixing; zero
  hashes map to 1.
- Keys and values are borrowed pointers whose validity for the index lifetime
  is the caller's responsibility.
- Equality compares hash, length, and bytes; equal hashes do not imply equal keys.

The goal is expected O(1) ordinary lookup/insertion with an explicit work bound
for adverse collisions, not cryptographic hashing for hostile inputs.

### Growth and failure atomicity

Insertion doubles capacity when load would exceed 0.5. Growth allocates a new
table and fully rehashes before replacing the old table. Allocation, overflow,
or probe-limit failures therefore never leave a partially migrated index.

Index APIs return 0 for initialization, insertion, or growth failure. They do
not suppress errors and continue using an incomplete table.

### Collisions and fallback

The 64-probe limit prevents constructed collisions from degrading linear
probing into unbounded O(n²). Two cluster types receive different treatment:

- Crowding with distinct hashes, such as unlucky ordinary input at low load:
  insertion performs one transactional doubling after probe exhaustion. The
  additional mask bit disperses these clusters and insertion can succeed.
- Identical hashes from constructed input: doubling does not change placement,
  so the retry still fails. The entire index is released and the map falls back
  to inherited pointer `qsort`/`bsearch`.

Thus:

- ordinary input: expected O(n) preparation and expected O(1) lookup;
- probe/allocation fallback: O(n log n) preparation and O(log n) lookup;
- no mixed state using a complete hash for some entries and an incomplete one
  for the rest.

Retaining `qsort` deliberately isolates failures; it is not the ordinary-input
path. The other `qsort` in `blocks.c`, sorting footnotes by source index, controls
final output order and is unrelated to lookup or duplicate normalization.

Allocation failure follows one engine-wide contract: **every `calloc`/`realloc`
failure, including `strbuf` growth and its 2 GiB limit, degrades gracefully
without abort or undefined behavior, and any loss is reported**. Three layers
implement it:

- `markdown_core_strbuf` carries sticky `oom` state. Failed growth preserves
  existing content, later writes become no-ops, and `detach` reports loss with
  NULL. A valid empty string still returns owned "", so NULL is unambiguous.
- Parser and map sticky `oom` bits aggregate structural loss: failed node/block/
  delimiter construction, contaminated line buffers, lost definitions or
  attributes, and allocations during extension opening/postprocessing. Paths
  where allocation failure could be mistaken for valid input shapes, such as
  list markers, table rows, and directive scans, distinguish the failures and
  set the bit only for allocation failure.
- `markdown_core_parser_finish` frees the syntax tree and returns NULL if any
  `oom` bit is set; truncated documents cannot masquerade as success. Injected
  failure may still succeed only through lossless fallback, such as map pointer
  sorting or capacity-sampling fallback, producing output identical to the
  uninjected control.

Public APIs report failures directly: `parser_new`/`map_new`/`iter_new` return
NULL; `markdown_core_consolidate_text_nodes`, `markdown_core_node_own`, and
setters return int. If copying fails, `markdown_core_node_own` clears the chunk
rather than leaving a borrowed pointer.

fallback_runner's `oom_sweep` protects the contract. It fails the kth allocation
in turn across every allocation point of a corpus covering all features, then
asserts that parsing returns either NULL or an AST identical to the control.
The suite also runs under ASan/UBSan.

### Duplicate reference and footnote semantics

The definitions list is newest-first, but Markdown requires the first authored
definition for a reference label to win. Index construction walks newest to
oldest and permits replacement for equal keys. The final slot therefore holds
the oldest definition, which appears first in source.

The map still performs the original expansion-limit accounting after each
lookup. Hash indexing does not bypass `max_ref_size` / `ref_size` protection.

Footnote lookup uses the same rule. Source-index output order requires explicit
collection and sorting, never reliance on hash-slot order.

Initial index capacity reuses directive sampling below. Above 1024 definitions,
a sample determines the starting capacity. Duplicate-heavy lists start from the
sample's unique count and grow amortized, rather than preallocating a slot for
every source occurrence.

## 2. Directive attribute deduplication

Directive attributes are ordinary key/value metadata. `id` and `class` behave
like other keys; HTML-style `#id` and `.class` shortcuts are rejected.

The duplicate-key contract is:

- preserve the first occurrence's position;
- use the last occurrence's value;
- emit only one key in final JSON.

Implementation takes two passes. The first indexes the first attribute for each
key. The second scans source order again, swaps later values into the first
attribute, and marks duplicates inactive. Output neither depends on hash
iteration order nor needs another sort.

If index initialization or insertion fails, directives fall back to pointer
sorting. Fallback explicitly moves the last value to the first attribute and
disables other duplicates, preserving identical semantics across both paths.

### Initial-capacity sampling

Inputs dominated by unique keys benefit from preallocating by total attribute
count; duplicate-heavy inputs should not waste space on millions of source
occurrences. When count exceeds 1024, sample at most 1024 keys:

- if the sample's unique ratio exceeds 0.5, initialize from total count;
- otherwise, initialize from sample unique count and grow incrementally as needed.

Sampling affects only initial capacity, not the final key set or duplicate
semantics. If the sampling index fails, initialize from total count, keeping
sampling an optimization hint rather than a correctness prerequisite.

## 3. Consecutive-backslash batching

### Triggering path

Malformed/unclosed directives expose the issue, for example:

```markdown
:x{key="\\\\\\\\\\\\...
```

Without a closing quote/brace, the directive scanner declines the fragment and
ordinary CommonMark inline parsing continues. For consecutive `\\`, the old
`handle_backslash` decodes one pair of source bytes to one literal backslash and
creates one Text node per call. Parser finish merges adjacent Text nodes, so
output is correct, but large inputs retain temporary nodes, allocations, and
linked-list work proportional to the number of pairs before consolidation.

The 128 MiB case once measured 9.850 normalized slowdown remotely and took
about 2.49 seconds locally.

### New path

Batching runs only when all conditions hold:

1. the current character is `\\`;
2. the next character is also an escapable `\\`;
3. no registered extension handles the `\\` special character;
4. the consecutive run contains at least two backslash pairs.

The implementation scans the complete pair run once, allocates a buffer of the
final size, fills it with `run_bytes / 2` literal backslashes, and creates one
Text node. Single pairs, odd tails, line endings, non-punctuation, and allocation
failure retain the original per-item path.

### Semantic equivalence

For a consecutive run of `2k` backslashes, the old path creates k adjacent Text
nodes, each with one literal backslash. Parser finish merges them into one
length-k Text node. The new path creates that final node directly.

Preserved boundaries:

- decoded literal bytes are identical;
- node scope still covers the full consumed source run;
- the character after an even pair run goes through the next inline dispatch;
- the final backslash of an odd run still becomes an escape, hard break, or
  literal according to the next character;
- extension hooks run before the core handler, and registering a `\\` extension
  explicitly disables batching;
- formula delimiter/escape rules are not redefined.

This optimization belongs in general inline core because the cost occurs in
ordinary Markdown parsing after directive fallback. Putting it in the directive
scanner would miss other inputs producing the same backslash run.

## 4. Complexity validation

The complexity runner measures each case at 4 KiB and 128 MiB endpoints, a
32768-fold input range. Its decision value is:

```text
normalized slowdown = large_time / small_time / 32768
```

Eight input classes are covered:

| Category | Case |
| --- | --- |
| directive scanner | valid long quoted value |
| directive/backslash | valid consecutive backslashes |
| scanner fallback | unclosed long quoted value |
| core backslash fallback | unclosed backslash value |
| directive map | many unique attributes |
| directive map | many duplicate attributes |
| inherited map | many unique references |
| inherited map | many duplicate references |

Latest local normalized slowdowns are 0.663–1.164. The 128 MiB
unclosed-backslash case fell from about 2.49 seconds to 0.189 seconds, measuring
0.979× after the fix.

The wall-clock threshold is 4.0, rather than treating 2.0 as a mathematical
discriminator for n log n. Parsing 128 MiB creates millions of objects and
crosses allocator/cache regimes absent from a 4 KiB sample. Remote macOS
measurements of the expected-linear unique-attribute hash path reached
2.753–3.318. The 4.0 threshold remains below the measured old sort path's 4.442
and rejects the old backslash path's 9.850.

The timing gate detects real end-to-end degradation but does not independently
prove algorithmic complexity. Structural guarantees come from:

- 0.5 load factor;
- the hard 64-probe limit and one transactional growth retry on exhaustion;
- transactional growth;
- deterministic fallback_runner regressions: injected allocators force pointer
  sorting for item-by-item comparisons against hashing, separately for reference
  maps and directive attributes; constructed clusters verify exhaustion triggers
  growth rather than immediate failure; allocation failure on both paths yields
  a lookup miss and permits later recovery;
- fallback_runner `oom_sweep`, injecting failure at every allocation point of
  the full-feature corpus and asserting "NULL or byte-for-byte identical" to
  protect graceful degradation;
- a 50,000-entry legacy-hash collision regression constructed against the
  inherited 32-bit hash. Under the current 64-bit hash this is equivalent to a
  random-key smoke test, not evidence of collision handling;
- unique/duplicate map endpoint tests;
- CommonMark correctness, directive fixtures, and sanitizer suites.

## 5. Review checklist

Reviewers should verify:

- keys/values outlive `markdown_core_key_index`;
- capacity arithmetic, doubling, and allocation sizes fully check overflow;
- failed growth leaves the old index unchanged;
- probe exhaustion triggers exactly one growth and retry before complete
  fallback, and fallback/hash duplicate semantics agree;
- newest-first list traversal preserves reference first-definition-wins;
- directives retain first-position/last-value-wins;
- no output incorrectly depends on hash-slot order;
- the backslash fast path runs only without an extension owner;
- odd/even runs, single pairs, allocation failures, and source scopes preserve
  prior behavior;
- performance gates continue covering ordinary, duplicate, malformed, and
  collision inputs.

## 6. Related implementation and tests

- `packages/markdown-core/core/map.c` / `map.h`: shared index and inherited-map adapter;
- `packages/markdown-core/core/references.c`: reference entry ownership;
- `packages/markdown-core/core/footnotes.c`, `blocks.c`: footnote lookup and explicit output sorting;
- `packages/markdown-core/extensions/directive.c`: attribute normalization and capacity sampling;
- `packages/markdown-core/core/inlines.c`: consecutive-backslash pair fast path;
- `packages/markdown-core/tests/runners/complexity_runner.c`: 4 KiB → 128 MiB endpoint gate;
- `packages/markdown-core/tests/runners/pathological_runner.c`: legacy-hash collision regression;
- `packages/markdown-core/tests/runners/fallback_runner.c`: allocator-injected sorted-fallback comparisons, constructed-cluster growth retries, and OOM fallback regressions.
