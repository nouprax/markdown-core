# Performance and architecture review — 2026-09-13

Review baseline: `3e6ad875`, macOS arm64. The review covered the C parser,
element recognition and commit, AST ownership, postprocessing, source mapping,
and the projection and lifecycle models of Swift, Kotlin JVM/Native, and
ECMAScript/WASM. Sections 1–8 record findings before the fixes; the final section
records the fixes and their validation.

**Initial verdict: changes required. Two P1 and six P2 issues confirmed, plus two
design observations.** Although all existing tests passed, new stress tests
combining input dimensions exposed deterministic quadratic work and stack
overflow during Swift destruction. The evidence does not support claims of
"no performance problems" or "all lifecycles are safe." All eight findings have
GitHub issues; their resolution is recorded below.

The confirmed scope contract takes precedence over the earlier documentation:
input is assumed to be UTF-8, and scope contains editor start and end coordinates
aligned with cmark. There is no input validation, coordinate-unit conversion, or
range repair. Scope is not a slice range into a parsed string and cannot be used
to infer an arbitrary node's literal. It must not become UTF-16, grapheme, or
uniformly half-open coordinates.

## 1. [P1] Ancestor checks in general node attachment enter the parser hot path and produce Θ(n²) work

Locations: [node.c:88](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/core/node.c#L88),
[autolink.c:710](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/autolink.c#L710).

`S_can_contain` walks every ancestor of the parent on each attachment to prevent
cycles. This is necessary for arbitrary reparenting, but the parser also takes
this path when inserting newly created, unpublished Link and Text nodes into an
existing deep tree. Nesting depth D multiplies the number W of new leaf-level
nodes, producing Θ(D×W) work. Existing benchmarks that varied depth or width
alone did not cover this combination.

Reproduction input: `"![" * n + "a@b.co " * n + "](u)" * n`. The control preserves
the structure and replaces the email with equal-length `abcdef ` text.

| n | Input bytes | Ancestor visits | Release parse + free, median |
| ---: | ---: | ---: | ---: |
| 512 | 6,656 | 527,872 | 1.862 ms |
| 1,024 | 13,312 | 2,104,320 | 6.812 ms |
| 2,048 | 26,624 | 8,402,944 | 25.942 ms |
| 4,096 | 53,248 | 33,583,104 | 217.969 ms |
| 8,192 | 106,496 | 134,275,072 | 982.437 ms |

The counts match `2n² + 7n` exactly; the control takes `2n` visits and 2.324 ms at
n=8,192. Span nesting reproduces the issue too. Counts came from a separate copy
of `node.c` with only a loop counter added; production source was unchanged.
Timings came from the uninstrumented Release library, using the median of three
runs per case. The complexity finding rests on deterministic operation counts,
not timing ratios.

Required direction: give the commit of newly created, unpublished nodes an
explicit ownership invariant that permits constant-time attachment, while
retaining cycle checks for arbitrary reparenting. Audit every
`append/insert/replace` caller, including link, citation, table, formula, and
footnote. Do not reintroduce a global safety switch, skip checks by depth, or add
an autolink-only algorithm. Regression gates must vary D and W independently and
together.

## 2. [P1] Swift value trees overflow the stack during normal ARC destruction

Locations: [List.swift:45](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/swift-markdown-core/Sources/MarkdownCore/Markup/List.swift#L45),
[Document.swift:38](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/swift-markdown-core/Sources/MarkdownCore/Document.swift#L38),
[MarkdownCoreSuites.swift:312](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/swift-markdown-core/Tests/MarkdownCoreTests/MarkdownCoreSuites.swift#L312).

`TreeBuilder` and the visitor traverse iteratively, but the final value tree
owns the next level through arrays and existentials. When the last owner
releases the root, ARC still recursively releases its descendants. The existing
deep-tree test explicitly retains the next node before releasing the previous
one at each level to avoid that destruction path, so it does not cover an
ordinary consumer's lifecycle.

An independent consumer linked optimized Swift/C libraries built from the
reviewed source, parsed `"- " * depth + "leaf\n"`, and used `withExtendedLifetime`
to ensure that printing and flushing happened before release:

| Depth | Parse completed | Normal release | Control: `_exit(0)` immediately after parsing |
| ---: | :---: | :---: | :---: |
| 1,000 | Yes | Passed | Passed |
| 10,000 | Yes | Passed | Passed |
| 30,000 | Yes | SIGSEGV | Passed |
| 65,536 | Yes | SIGSEGV | Passed |

Host LLDB captured `EXC_BAD_ACCESS` during release, with repeated
`_swift_release_dealloc`, `swift_arrayDestroy`,
`_ContiguousArrayStorage.__deallocating_deinit`, and generated destruction
frames. The failure occurs while destroying the array ownership chain, not
during C parsing or Swift construction. The triggering depth depends on the
thread stack and build conditions; 30,000 is not a reliable safety limit.

Required direction: internal ownership must bound the call stack during
construction, traversal, complete-root release, and final release of a retained
subtree, while preserving immutable value semantics and transfers across
isolation boundaries. The fix needs normal-release tests through real
consumers. Requiring manual level-by-level release, increasing the thread stack,
adding depth thresholds, or introducing a global cleanup queue does not resolve
the architectural requirement.

## 3. [P2] Directive probes allocate attributes before full recognition, then discard and reparse them

Locations: [directive.c:257](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/directive.c#L257),
[directive.c:537](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/directive.c#L537),
[directive.c:590](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/directive.c#L590).

`scan_directive_block` parses attributes and constructs classes, records, and
strings before checking that only whitespace follows. `probe_directive_block`
frees the result whether recognition succeeds or fails. The actual open step
then repeats the parse.

Independent allocator counts: one probe of the invalid
`::: {.a k=1} junk\n` makes **7 allocations totaling 550 bytes**. The valid
`::: {.a k=1}\n` makes **7 allocations totaling 510 bytes**, but the probe still
discards every attribute. The class-word form also allocates before rejecting
invalid trailing content.

For 8,192 invalid directive paragraphs, the totals are 131,185 allocations and
17,045,224 bytes. An equal-length control that changes only the opener to `;;;`
takes 41,071 allocations and 7,100,136 bytes: 90,114 fewer allocations. Attribute
work occurs in a local attribute parser while the probe's
`parser.attribute_work` remains 0, so the existing work counter misses this
path.

Required direction: recognition should retain borrowed name, label, and
attribute boundaries, then materialize semantic fields once after validating
the complete candidate. Reuse the attribute recognizer instead of adding
another grammar. All recognition work used to establish complexity must feed
the same metric. The problem is semantic allocation before a successful commit;
a valid block does not have to wait for its closing fence before it can be
created.

## 4. [P2] Pipe-table continuation constructs and destroys row data, then constructs it again

Locations: [table.c:607](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/table.c#L607),
[table.c:486](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/table.c#L486).

`matches` calls `row_from_string` to allocate a row and cells solely to return a
boolean, then frees them all. `try_opening_table_row` parses the same row again;
it also attaches an AST TableRow before later checks can fail and delete it.
For `| a | b |\n`, continuation detection alone makes **3 allocations totaling
144 bytes**, all discarded.

Header recognition also passes the entire paragraph to `row_from_string`,
constructing and clearing cells for each preceding line before retaining only
the selected header. This establishes redundant linear work and allocations,
not quadratic complexity.

Required direction: row recognition should provide borrowed boundary facts;
actual cells should be constructed when the header/body is selected and
committed. Temporary parse results should not become a persistent cache that
must stay synchronized with the AST. Preliminary checks for a valid
continuation usually establish its syntax, so not every TableRow allocation is
an unclosed speculative candidate.

## 5. [P2] Autolink copies the entire text even without a match and creates empty Text nodes only to delete them

Locations: [autolink.c:563](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/autolink.c#L563),
[autolink.c:712](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/autolink.c#L712),
[autolink.c:747](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/autolink.c#L747).

`postprocess_text` moves out the owned literal and substitutes a borrowed slice
before confirming any email match. Even without an `@`, it allocates a copy of
the literal at the end and frees the original buffer. For 90,112 bytes of plain
text, the autolink stage requests 90,208 bytes in total: 96 bytes of iteration
and consolidation overhead plus a copy of the entire text. Invalid email
candidates behave the same way.

Every split unconditionally creates a trailing Text node; when `post_len == 0`,
it is deleted immediately. Empty prefixes also undergo ownership conversion
before deletion. The outer parser has already consolidated Text nodes, but the
first postprocessor, autolink, repeats consolidation with no intervening tree
mutation.

Required direction: scan borrowed text until the first confirmed match, then
transfer ownership of the original buffer. Commit only nonempty fragments and
place consolidation at one consistent lifecycle boundary. Preserve source
mapping and complete OOM failure behavior without choosing algorithms by input
length or match count.

## 6. [P2] Fixed-dialect registration rebuilds several heap lists on every parse

Locations: [blocks.c:95](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/core/blocks.c#L95),
[core-elements.c:41](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/core-elements.c#L41).

Every parse attaches the same 32 static descriptors and rebuilds `elements`,
`inline_elements`, `inline_lifecycle_elements`, `block_elements`, and
`block_alternatives`. Appends repeatedly traverse to the list tail, and inline
registration sorts again. This is O(E²) in the element count E, which is
currently fixed; it is not quadratic in input length.

Empty input takes 90 allocations totaling 10,048 bytes, with 76 allocations
before reading any input. Those 76 include parser/root initialization and
cannot all be attributed to registration lists. Nevertheless, repeatedly
allocating and freeing static membership and order adds unnecessary fixed cost
and failure boundaries when parsing on each editor change.

Required direction: represent fixed order and classification with one immutable,
contiguous registration layout. Necessary test injection should use that same
model. Do not replace the lists with a process-wide lazy cache, locks, or
separate production and test mechanisms.

## 7. [P2] Autolink changes semantics by domain segment count to hide an earlier complexity problem

Location: [autolink.c:245](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/autolink.c#L245).

When either of the last two domain segments contains `_`, the code rejects it
only if `np <= 10`; with more segments, it accepts the domain. The comment
explains that this avoids quadratic behavior and assumes normal URLs will not
have that many segments.

Reproduced: `www.a.a.a.a.a.a.a.a.a._b` becomes Text, while adding a segment to
produce `www.a.a.a.a.a.a.a.a.a.a._b` makes it a Link. This is a semantic branch
based on input cardinality and violates the repository's requirement for one
general algorithm.

Required direction: separate domain rejection rules from scan complexity.
Provide reusable scan progress or an index for candidate suffixes under a fixed
dialect rule. Simply deleting the threshold must not restore the old quadratic
behavior. This behavior has inherited compatibility implications, so the fix
must update the specification, compatibility baseline, and long failed-candidate
tests rather than silently changing the grammar.

## 8. [P2] Scope implementation, specification, and audit invariants disagree

Locations: [canonical-ast.md:68](https://github.com/nouprax/markdown-core/blob/3e6ad875/docs/specs/canonical-ast.md#L68),
[base.md:76](https://github.com/nouprax/markdown-core/blob/3e6ad875/docs/specs/dialect/base.md#L76),
[audit-position-places.mjs:80](https://github.com/nouprax/markdown-core/blob/3e6ad875/scripts/audit-position-places.mjs#L80).

**The implementation follows the confirmed coordinate-unit and pass-through
contract.** The [C facade](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/ast.c#L305)
copies native start and end coordinates directly. Swift, the ES/WASM wire, and
Kotlin JVM/Native projections do not convert them to language-specific string
indices. Swift's Scope comment explicitly warns against substring extraction.
Internal source marks map stripped prefixes, decoded content, and joined input
fragments back to original editor coordinates. This is necessary parser source
mapping, not a change to the public scope unit.

For `> é &amp; 🚀\n> next\n`, the first Text literal is `é & 🚀`, but its scope
remains `1:3..1:15`; the second line's Text has scope `2:3..2:6`. These Text
coordinates match the locally pinned cmark 0.31.2. Literal length cannot
determine scope.

Remaining inconsistencies:

- The canonical specification describes start and end as the first and last
  bytes, calls them inclusive, and explains them as slices. The base
  specification also suggests using scope when consumers need newline bytes.
  This leads callers to treat public coordinates as a string range.
- A truly zero-byte input has Document scope `1:1..0:0`, matching the pinned
  cmark version. Documentation instead says `1:1..1:0`, which belongs to other
  inputs such as a document containing only a newline.
- The position-place audit rejects line 0 and end-before-start uniformly, so it
  cannot represent cmark's unchanged empty-document value. The corpus does not
  include truly zero-byte input; passing 15,071 coordinate checks therefore
  does not establish this boundary.
- Swift's Position comment says lines always start at 1 and does not describe
  the native empty-document sentinel.

Required direction: align documentation and tests with the confirmed cmark
coordinate pass-through contract. Record the actual sentinels without repairing
native coordinates or normalizing them to half-open ranges. Compare exact
values for empty input, blank lines, LF/CRLF, non-ASCII text, tabs, entities,
escapes, nested containers, reference occurrences, and tables. A table cell
spanning rows may cover multiple structural fragments; its scope must not be
changed to satisfy string-range containment.

## Two design observations to retain

**Formula postprocessing creates a new object, copies the literal, and frees
the old CodeBlock.** [formula.c:664](https://github.com/nouprax/markdown-core/blob/3e6ad875/packages/markdown-core/elements/formula.c#L664)
retains only scope and the trimmed literal. Fenced `formula {#eq .wide k=1}`
produces `anchor=null attributes={}`. However,
[formulas.md:227](https://github.com/nouprax/markdown-core/blob/3e6ad875/docs/specs/dialect/formulas.md#L227)
explicitly specifies discarding the other fields, so this cannot be classified
as an implementation deviation from the current specification. It remains a
semantic conversion and allocation cost worth examining: establish whether
common attributes should also be discarded, then use a consistent commit or
conversion mechanism once the semantics are settled, without an additional
repair pass.

**The attribute recognizer allocates its index for the entire input.** For a
1 MiB input starting with `{}`, `markdown_core_attributes_end` returns end=2,
but first requests 8,388,616 bytes and performs 1,048,577 units of index work.
Complexity remains linear, so this example alone does not establish an
incorrect algorithm. Sparse attributes do have a substantial auxiliary-space
cost. Further evaluation should cover total failed-candidate work and peak
memory across the whole grammar, without special cases for short attributes or
length thresholds.

## Architecture and ownership assessment

- The boundary of one native parse followed by immutable binding projections
  is sound. Shared reference resources and occurrence-specific scopes or
  attributes have different semantics and must not be conflated to reduce
  object counts. Source mapping and canonical scope also serve different
  purposes and are not duplicate state.
- Unified C construction, transactional kind conversion, and iterative release
  are invariants to retain. The Swift failure shows that iterative construction
  and traversal alone do not establish a complete lifecycle.
- The main redundancies are fixed-descriptor registration, repeated probe/open
  materialization, postprocessing borrow/copy/delete cycles, and repeated
  consolidation. They do not require additional synchronized caches.
- The reviewed parser paths provide no reason to add synchronization across
  parses. Parser state remains local. JVM class loading and initialization have
  a lifecycle basis for one-time synchronization; synchronous ES WASM calls and
  instance ownership likewise cannot simply be dismissed as removable overhead.
- Not all state created at an opener should disappear. Delimiter/bracket stacks
  for closure matching, fallbacks that ultimately remain plain text, and shared
  linear scan indices are necessary. The concern is allocating final objects or
  fields before their semantics are established, then undoing them on rejection.
  Directive and table probes demonstrably do this; autolink also materializes
  fragments already known to be empty.
- Address the two P1 issues first, unify ownership and data models from
  recognition to commit, then remove static registration and redundant
  postprocessing costs. Scope documentation and audits can be aligned
  independently, without a coordinate-conversion layer.

## Validation scope and evidence

| Validation | Result |
| --- | --- |
| C Release, excluding benchmarks | 88/88 passed |
| C ASan, complete suite | 88/88 passed |
| C UBSan, complete suite | 88/88 passed |
| C TSan, concurrency tests | 2/2 passed |
| Repository benchmarks | 8/8 completed; these do not gate the new inputs above |
| Swift | 37 tests / 10 suites passed; the independent optimized release probe failed |
| ECMAScript Node + WASM built from the reviewed source | 41/41 passed |
| ECMAScript conformance | 40/40 passed |
| Kotlin JVM | 42 tests + 8 conformance passed |
| Kotlin Native macOS arm64 | 31 tests + 8 conformance passed |
| Source-list / AST projection / element-parser boundaries | Passed |
| Position places | 15,071 nodes, 0 findings; the empty-input model was still missing |
| Scope containment | 16,206 relations, 30 registered differences |
| Inline cmark positions | 71 cases, 43 registered differences |

The registered containment and inline differences are not automatically 30/43
new bugs. They include dialect differences and table coordinates that are not
string ranges. This review did not revise ledgers to hide new findings.

The complete platform matrix, Android/iOS devices or simulators, browser matrix,
and full external AST parity campaign were not run. Timings are local
diagnostic data. Allocation bytes count cumulative requests, including realloc
target capacities; **they are not peak live memory or RSS**. Passing sanitizers
or existing tests does not prove performance or absence of leaks for all inputs.

The timings, initial allocator counts, and LLDB backtraces above came from
untracked local diagnostic artifacts. They are historical observations from
this review. The repository does not provide those raw artifacts or commands
to rerun those measurements; these numbers are not independently reproducible
benchmark baselines.

Tracked tests protect the fixed invariants. These paths and commands work in
other checkouts:

- [C API tests](../../packages/markdown-core/tests/api/main.c):
  `deep_inline_construction` varies depth and width independently and checks the
  complete tree. `speculative_probe_allocations` checks recognition allocations
  and buffer ownership when no match is found. `autolink_domain_linear_work`
  checks domain semantics and deterministic scan-work bounds. Run
  `pnpm test:c-host` from the repository root.
- [Parser boundary tests](../../scripts/tests/parser-boundaries.test.mjs):
  restrict arbitrary reparenting calls in parser construction. Run
  `pnpm check:element-inventory`.
- [Swift ownership tests](../../packages/swift-markdown-core/Tests/MarkdownCoreTests/OwnershipTests.swift):
  cover normal release of complete trees and independently retained subtrees.
  Run `pnpm test:swift-macos`.
- See the [environment guide](../toolchains.md) for toolchains and the
  [repository README](../../README.md) for test entry points. These regressions
  verify semantics, ownership, and work bounds; they do not promise to reproduce
  a particular machine's timings or RSS measurements.

## Fixes

Implementation and regression coverage for all eight issues are complete. The
fixing PR links the issues for closure on merge.

| Issue | Fix |
| --- | --- |
| [#232](https://github.com/nouprax/markdown-core/issues/232) | One constant-time splice handles parser transfers of independently owned subtrees and checked arbitrary reparenting. Public reparenting retains cycle checks. All parser callers migrated, and the former inline-only splice was removed. |
| [#233](https://github.com/nouprax/markdown-core/issues/233) | Immutable flat Swift records and indexed relations replace recursive ARC subtree ownership. Tests no longer bypass destruction by releasing one level at a time. |
| [#234](https://github.com/nouprax/markdown-core/issues/234) | Directive probes borrow attribute ranges and materialize attributes only after recognizing the complete opener. They share the attribute recognizer and aggregate its work counters. |
| [#235](https://github.com/nouprax/markdown-core/issues/235) | Pipe rows use one borrowed iterator for recognition and commit. Temporary row/cell heap arrays are removed. Continuation checks, mismatched headers, and preceding-line recognition do not materialize geometry. |
| [#236](https://github.com/nouprax/markdown-core/issues/236) | A failed email scan retains the original Text buffer. Only nonempty fragments are created, and duplicate consolidation is removed. |
| [#237](https://github.com/nouprax/markdown-core/issues/237) | The fixed descriptor table is borrowed directly, removing five lists rebuilt on every parse. Test extensions use the same contiguous registry, with no global cache or lock. |
| [#238](https://github.com/nouprax/markdown-core/issues/238) | The 10-segment semantic threshold is removed. A shared monotonic rejection boundary keeps failed-suffix scanning linear while permitting valid candidates after an underscore. The specification records the change to inherited behavior. |
| [#239](https://github.com/nouprax/markdown-core/issues/239) | C/Swift/ES/Kotlin comments, specifications, and audits share one contract. Tests cover truly zero-byte input and UTF-8 boundaries. Production coordinates remain unchanged. |

Swift child relations change from arrays to read-only
`MarkupCollection<Element>`; grouped definition relations use
`MarkupGroups<Element>`. Both conform to `RandomAccessCollection`; callers that
need arrays use `Array(...)`. Collections reside directly in the owning node's
`Fields`, and groups do not consume separate records. Retaining a container
subtree keeps the entire immutable Swift store alive until the last owner
releases it. There are no native handles, caches, synchronization, or cleanup
queues. The Document-only metadata payload is stored indirectly once so its
size does not determine every ordinary node's stride. See
[Swift storage](../architecture/swift-storage.md).
`MarkupStore` centralizes field queries and projection. All 29 node types refer
to fields through `@Stored` rather than individually storing indices and
checking the enum. Location information resides in the shared reference
implementation; views remain 16 bytes.

### Fix evidence

- At n=8,192, the combined deep/wide Embedded + email case fell from
  982.437 ms to 3.838 ms; Span took 4.622 ms. Regressions vary depth and width
  independently and check the complete tree. A source-boundary audit prevents
  parser calls to arbitrary reparenting APIs, while existing tests continue to
  protect public cycle checks.
- Empty-input C allocations fell from 90 to 19, and allocations before reading
  input fell from 76 to 6. Directive probes may allocate only the shared
  recognizer index; class-word probes allocate nothing. Pipe continuation and
  column-count mismatches allocate nothing. Autolink without a match allocates
  only one 48-byte iterator; tests also assert that the original buffer address
  is unchanged.
- Swift regressions for normal release and independently retained subtrees pass
  at 30,000/65,536 levels. Weak references confirm that the store is reclaimed.
  An independent optimized consumer also prints `released` normally.
- Optimized Swift measurements before and after the change, using the same C
  implementation, covered empty documents, wide documents, and deep lists. No
  stable timing regression was observed for wide documents. A 4,000-level list
  fell from roughly 4.0–4.8 ms to 2.3–3.0 ms. Peak RSS for the host diagnostic
  workload fell from 42,270,720 bytes to 36,519,936 bytes. Timings and RSS are
  workload-specific observations, not guarantees for all inputs.
- Domain tests cover both sides of the former threshold, underscores in the
  last/second-to-last/third-to-last segment, URL/www entry points, and
  16–8,192 overlapping candidates. Deterministic scan work stays within 3×
  input bytes.
- C Release passed 88 correctness tests plus 8 benchmarks; ASan/UBSan each
  passed 88 tests, and TSan passed 2 concurrency tests. Swift passed 42 tests /
  10 suites plus 2 independent consumer tests. Definition grouping and ownership
  tests also passed in Release; 8,192 bodies no longer add collection records.
  ES passed 41 correctness / 40 conformance tests; Kotlin JVM passed 42+8 and
  Native passed 31+8.
- Position places reported no new differences across 15,072 cases, now including
  truly zero-byte input. Containment across 16,206 relations and inline
  positions across 71 cases still match their original ledgers.

Existing compatibility ledgers were not changed to conceal regressions. The two
design observations retain their original boundaries: discarding Formula fields
is current specified behavior, and the attribute index has linear auxiliary
space cost. This work does not additionally change those semantics or algorithms.
The complete mobile-platform and browser matrices were not run.

A follow-up review corrected the transaction boundary of public reparenting:
the containment predicate runs once, before unlinking, and commit uses the
shared non-failing splice. `attachment_containment` covers five mutation
operations, same-parent and cross-parent moves, and preservation of both trees
on rejection. The regression failed before the fix.

After `alignment` was renamed to `flow`, three Pandoc table records needed
updated projection digests on both sides. Each was updated only after verifying
that restoring the field name alone reproduced all six original digests.
Input, relative column widths, content, structure, and the reasons for the
registered differences are unchanged. No difference waiver was added or relaxed.
