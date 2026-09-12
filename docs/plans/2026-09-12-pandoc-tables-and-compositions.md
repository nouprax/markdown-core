# P11 implementation and P12 evidence

P11a–P11d and local P12 implementation/evidence are complete for the selected
feature inventory. The 2026-09-12 scope clarification confirms that acceptance
is based on the requested features and normative dialect, not full Pandoc
compatibility. The [landing checklist](2026-09-04-canonical-vnext-landing-plan.md)
now states that boundary explicitly. Supported-host CI and release remain
separate delivery work.

- [x] Implement captions and all three additional table forms across every binding.
- [x] Preserve sparse ownership with mapped block inputs and bounded geometry.
- [x] Add canonical, source-position, OOM and counted-work evidence.
- [x] Add adjacency, nesting, opacity and deterministic mutation probes.
- [x] Document the public syntax and remaining oracle differences.
- [x] Verify selected-feature completion without expanding scope to full Pandoc compatibility.
- [x] Reduce candidate copying, allocation and repeated scanning; measure the shared parser path.
- [x] Verify simple-table body interruption review against the pinned oracle and clarify the dialect.

## Delivered table behavior

All four source forms emit the same Table model on C, Swift, Kotlin and ES.
TableCaption is an independent owned field, visited before head/body/foot rows.
The existing row chain and its counts continue to describe rows alone. The
canonical inventory covers 39 Markup kinds, 73 fields and 36 shared cases.

Captions use the pinned Pandoc 3.11 ownership rule: in `A / table 1 / B /
table 2`, table 1 owns A and table 2 owns B. Without A, table 1 owns B. A
second caption with no eligible table remains ordinary paragraph content.
The producer and definition-term precedence query share one candidate grammar.
Captured headers retain the core's heading, code, quote, list and definition
marker precedence. Malformed candidates consume no source.

Simple cells retain inline content and null relative widths. Multiline cells
remove their common alignment margin, retain relative indentation, and parse
ordinary blocks in both headers and bodies. Grid cells remove one shared border
padding space. Every block cell enqueues a mapped input in the original parser;
the queue drains before document-wide completion and inline parsing. References,
headings and definitions use document-owned registries ordered by original line
and column. Nesting does not recurse into a new document parser or repair scopes
after parsing.

Grid recognition joins source regions across missing walls. A wall present on
only some content lines does not divide the eventual cell. A connected region
can acquire its remaining rectangle through a later row; validation occurs when
it leaves the active frontier. Surviving roots compact at every row, so temporary
union state has at most twice the column count, independent of row count or
span area. Closed components are the final candidate cells, ordered by their
source anchors using the same fixed-pass radix operation as deferred definitions
and headings. Geometry storage is O(columns + rows + cells), in addition to
captured source and provenance maps. Boundary work is O(source × α(columns))
with fixed-pass ordering O(cells); there is no rows-by-columns allocation.

Only the starting row owns a spanning cell. Explicit fully covered rows retain
`cells=[]`; authored empty cells retain `content=[]`. Consumers recover occupied
coordinates and choose layout. Multiline source rectangles have overlapping
linear bounding intervals, and a rowspan scope extends below its owning row;
these intentional scope findings are individually recorded in the position
ledger. All endpoints remain checked against the original source.

## Verification

The C API suite checks counted size doubling for valid and malformed boundaries,
wide grids, repeated failed suffix queries and fully covered rows. It measures
the frontier bound directly. Nested mapped-input tests reach 128 grid levels;
shared references and same-line duplicate headings verify document ownership and
source order. Strict allocation-failure tests exercise captures, queues, fields,
spans and nested cells. The allocation-free iterative field destructor also
retains its 4096-level ownership test.

Validation includes native correctness and conformance; ASan API, strict OOM and
table/definition fixtures; Swift tests and a separate package consumer; Kotlin
JVM and macOS Native correctness/conformance and ABI; ES Node, packed-package
consumer/types, conformance and headless Chrome; source-position, containment,
canonical, source-list, public-surface, projection and test-topology audits.
The existing CommonMark, GFM, remark, Obsidian and insertion oracle suites are
also run against the implementation. Fixed-seed CommonMark, GFM and remark
recombinations use an independent CommonMark AST to keep implicit heading
references, custom task markers and definition-list envelopes in their owning
extension oracles; escaped markers and opaque blocks retain base coverage.
Tool bootstraps are explicit, pinned setup
operations; ordinary tests do not fetch dependencies.

Pandoc canaries assert native JSON before projection: 48 caption arrangements
across all forms and markers, plus 11 sparse/merged-grid cases. Product assertions
check the same ownership and geometry. The corpus has 92 cases, 58 agreements,
34 exact documented differences, and zero missing-feature gaps. A new exact
remark conflict records the normative `:badge[short]` caption prefix after an
uncaptained table; neither projected tree discards its content.

The six simple-table body witnesses compare heading, quote and fence markers
with and without a preceding blank line. Pandoc 3.11 retains the unseparated
markers inside inline cells and opens independent blocks after a blank, agreeing
with the parser in all six cases. The dialect now scopes shared block-start
interruption to pipe rows and caption paragraphs; simple rows retain their own
termination grammar. API tests also cover LF, CR, CRLF and unterminated EOF.

## Candidate performance follow-up

Table queries borrow input slices and reuse one parser-owned line workspace.
Readonly bounded scanners remove the write-and-restore sentinel requirement
that previously forced captured-line copies. Cached dash-run facts serve all
candidate grammars; interval and column geometry allocation waits until it is
needed. Invalid separators reject before header precedence work, and a failed
full-boundary multiline candidate is not parsed twice. Dash runs and horizontal
grid boundaries use generated re2c scanners. Both scanner families regenerate
exactly with the pinned re2c version.

Counted-work tests cover ordinary paragraphs and malformed Unicode headers as
well as table shapes. They enforce bounded workspace growth, zero column
geometry for rejected candidates and linear separator scans. Exact-allocation
scanner tests cover every truncated prefix and invisible out-of-slice suffixes
under ASan. Strict OOM checks retain the parser ownership contract.

Local Release measurements compare `c7f57b88` with this follow-up using the same
compiler, `-O3 -DNDEBUG`, shared public library and benchmark runner source.
The ordinary-document benchmark uses seven alternating rounds, five warmups
and 31 repetitions per round; medians improve from 3.723 ms to 3.537 ms (5.0%).
The main-base revision `5f5a516c` measures 3.306 ms on the same run, leaving a
7.0% overhead for the full feature change.

The `tables` workload adds pipe/caption, simple, multiline, grid and rejected
candidate inputs. Three alternating rounds with two warmups and nine repetitions
per round give the following medians; these timings are diagnostic evidence,
while the counted-work assertions gate complexity.

| Workload | Before (ms) | After (ms) | Time reduction |
| --- | ---: | ---: | ---: |
| Pipe with caption | 3.421 | 3.129 | 8.5% |
| Simple | 2.934 | 2.676 | 8.8% |
| Multiline | 6.250 | 5.381 | 13.9% |
| Grid | 4.728 | 4.630 | 2.1% |
| Rejected candidate | 1.915 | 1.601 | 16.4% |

The representative corpus, extension workload and doubled adversarial link and
emphasis workloads were also compared. All correctness, conformance, external
oracle and source-position gates pass with the follow-up.

## Composition evidence

`node scripts/check-pandoc-compositions.mjs` writes
`build/pandoc-compositions.json`. It runs 306 ordered pairs of the 18 reviewed
feature witnesses, 18 blockquote wrappers, 18 grid-cell wrappers, 18 opaque code
wrappers, and 128 deterministic mutations. Each record contains the authored
input, reader, seed IDs, both complete projected trees and their SHA-256 digests.
The fixed mutation reader enables exactly the selected extension inventory.
Unknown constructors, parse errors and oracle-envelope changes fail the run.
No seed's single-case difference waiver is inherited by a composition.

The current 488-case diagnostic has 233 exact agreements and 255 differences.
Every opaque code wrapper agrees. Among the 360 adjacency/nesting/opacity cases,
34 example-list adjacency cases and two nested example-list cases retain the
existing consumer-model difference; the other 110 differences concern authored
relative widths. Mutation inputs also expose inherited whitespace/escape rules,
script matching, citations and document-owned values. These are raw diagnostic
results, not 255 accepted waivers. `--require-agreement` fails while any remain.
The composition unit tests verify complete pair coverage, stable ordering,
nesting, opacity, reader control, repeatable mutation seeds and fail-closed
comparison behavior.

## Acceptance scope and retained differences

The selected inventory contains 18 public feature entries. Both the corpus
readers and the composition inventory restrict extension flags to the selected
features and their declared dependencies. The former request to eliminate all
non-representation differences was an acceptance error, not an additional user
requirement. Closure requires implemented syntax, fields, ownership, fallback,
precedence and complexity under the existing modules, with binding and inherited
language regression evidence. A matching oracle tree is useful evidence but is
not the product definition.

The normative relative width is the interior source width divided by the sum
of those widths. For grid interiors 15, 15 and 20, the result is 0.3, 0.3 and
0.4. Pandoc reports 16/72, 16/72 and 21/72, incorporating borders and its default
page width. The implementation preserves the existing normative contract and
records both original values in the three former table-gap cases. It does not
silently normalize the oracle or erase widths.

The other 31 existing entries also include deliberate syntax differences:
nonempty scripts and delimiter matching, global anchor reservation and Unicode
rules, reference adjacency, citation recovery/affixes, list-start restrictions,
fence closure, definition compactness and document-owned example lists. Their
exact cases and explanations remain in
[`deltas.json`](../../specs/oracles/pandoc/deltas.json). The
[34-case review list](../../specs/oracles/pandoc/differences.md) distinguishes source
recognition, field semantics, model/information and reader-configuration
differences. Some entries are historically called `projection` even though
their own explanation describes a semantic difference; that label does not
make them representation-only equivalences.

These are 34 differing inputs, not 34 unimplemented features or 34 additional
Pandoc extensions. The fixed corpus has zero remaining feature gaps; the
independent product fixtures and binding/complexity checks establish completion
under the selected modules. New or changed corpus differences still fail the
exact registry. Composition diagnostics retain both trees without granting
blanket waivers. No change to parser ownership, consumer numbering, grid
coordinate expansion or layout is required to close P12 under this scope.
