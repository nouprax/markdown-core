# Issue 363: fresh measurement and remaining work

PR #372 resolves seven confirmed problems. The controlled same-input lifecycle
ratio falls **1.135× → 1.073×** (Ir **−5.50%**); all **464/464** source gates pass.
See [the final comparison](#final-controlled-comparison) for artifact identity,
remaining-cost analysis and the complete final 43-pair/12-boundary census.

## Measurement identity

The baseline is main `d01676ec17923349220267f0d488576e065c28f1`, after PRs
#362, #365 and #366. It is measured by the fresh main push run
[35682262782](https://github.com/nouprax/markdown-core/actions/runs/35682262782),
stage job `106601649657`, artifact `10676405508`:

- Name: `stage-benchmark-d01676ec17923349220267f0d488576e065c28f1`.
- ZIP SHA-256: `7fcfd54d941e07af2b126e6d7c9bc35e3c8b26f2d98f2a0fd3f56c32f1ab3def`.
- GCC 13.3.0, glibc 2.39, Valgrind 3.22.0, x86-64/generic.
- Corpus: 232 cases at scales 1 and 2; 464 measured documents, 43 proved
  domains and 12 reviewed boundary interventions. The corpus and pairing
  digests are recorded in the generated census below.

This replaces the old issue's 138-document/30-substitution interpretation.
The current reference cohort has 224 documents: 234 have a CommonMark/GFM
reference, but 10 declare unmatched anchor fields. Exclude `block-heading`,
`block-lheading`, `pair-ldirective-common`, `host-setext` and `mixed-commonmark`
at both scales. They remain measured workloads, not same-input comparisons.
Do not compare the old and new aggregate ratios as a controlled speedup.

Reproduce the measurement on Linux with the pinned toolchain and reference
engines, then aggregate its raw profiles without rerunning or changing them:

```sh
node scripts/benchmark-stages.mjs --scale 2 --out build/stage-rerun
node scripts/report-performance.mjs build/stage-rerun build/performance-rerun
```

The census generator writes both Markdown and JSON. JSON includes the exact
cohort, omitted fields, self costs by function/file and inclusive call edges.
"Complete parse" is the `bench_parse_document` lifecycle, including parsing,
the root receipt and document teardown; `main`'s input loading and process
setup are outside that edge. The two stages exclude this teardown work.
It verifies the parse-stage partition and that function/file self partitions
sum to the profile's `totals`. Callgrind `summary` can differ from `totals`;
it is not substituted for the measured cost-line sum.

## Disposition of the previous candidates

| Previous item | Fresh evidence and disposition |
| --- | --- |
| Node lifecycle | The slab model from #362 is in main. Constructor self is 514,294,439 Ir over the current reference cohort. Recycled cells still clear the entire maximum record, even for kinds with no record: #369. Block creation allocates content before writing: #368. Kind conversion also abandons existing cell storage for a separate replacement allocation: #373 (24,159 calls / 5,307,575 inclusive Ir over the full corpus, not this reference cohort). |
| Repeated attach checks | #365 supplies owner proofs, validated attachment and debug/ASan containment assertions; #354 is closed. Public checked mutations remain necessary. This rerun found no new unproved internal attachment bypass or repeated decision to remove. |
| Finish walk | `walk_owned_trees` is 451,879,231 self Ir. ENTER/EXIT events, mutation handling, field roots and word depth are required semantics. One actual duplicated operation remains: absorbed Text completion reinterprets its structure despite the projected finish plan, #370. The consolidation-to-completion edge is 815,344 calls / 39,136,512 inclusive Ir; that whole cost is not removable because completion and observation remain required. |
| Per-line block pipeline | #365's shared line index and static-inline driver advancement address #355. Keep the per-document source-stage regression gate. `S_parse_source`/`open_new_blocks` self totals (842,476,508 / 520,400,102 Ir) do not by themselves identify another redundant scan. |
| Table gates and geometry | #356's reusable geometry and bounded transactions are in main. A prose line can head a simple table; the next line must be inspected. Spaced dash forms can belong to multiline tables. Rejecting these inputs based on benchmark shapes changes grammar. No additional redundant gate was established by this rerun; the high table-file ratio is not a standalone correctness or complexity defect. |
| Properties | #357's line access and failure model are in main. Historical metadata substitutions are reviewed boundaries/reconstructed domains, not full-language isomorphisms. No new repeated ownership operation was confirmed. |
| Inline fixed cost | `start_inlines` / `finish_inlines` self is 109,383,045 / 117,218,728 Ir. Resumable parsing, headings/references and word grammar require state and callbacks. Shared node initialization (#369) and Text completion (#370) are the confirmed removable work; skipping setup based on input shape is unjustified. |
| Autolink | Bare autolinks remain enabled by the dialect and therefore run on CommonMark-shaped text. That is additional semantic work. #359 already removed the fake-node mapping operation. No new redundant mapping was found. |
| strbuf | `add_child_validated → strbuf_grow` has 1,608,560 calls / 406,539,733 inclusive Ir, including allocations and repeated capacity checks. Block construction should acquire storage only when writing, #368. A separate allocator-poisoning reproducer also exposed a real empty-buffer NUL defect, #371. |

These dispositions distinguish required behavior from confirmed defects;
they do not assert that all remaining implementation costs are optimal.
Source-file ratios include different grammar obligations and compiler inlining,
so they cannot prove the cost of one equivalent semantic operation.

## Problems included in this round

1. [#368](https://github.com/nouprax/markdown-core/issues/368): every block uses
   the existing empty-buffer sentinel; the first write acquires storage.
   The streaming line writer retains the former 32-byte initial reservation
   at this ownership transition and reserves the whole first write in one growth;
   bounded value producers keep ordinary strbuf writes.
   The policy is shared by every block kind and physical line. Removing the
   reservation entirely caused extra reallocations: early partial Linux
   profiles showed fences +3.65% and Setext workloads +2.54% source Ir. This
   revision moves the reservation to its correct lifecycle boundary instead.
   A full intermediate rerun then exposed one source-gate regression
   (`split-attributes-link`, +3.64%): reserving 32 and immediately growing for
   the known first line changed allocator work. Reserving the complete write,
   including tab expansion, removes that redundant allocation generally.
2. [#369](https://github.com/nouprax/markdown-core/issues/369): initialize the
   complete node and the active record, not spare cell storage. Fresh/reused
   cells share one constructor; external records retain their own allocation
   and failure transaction. The 202,308,828 Ir attributed to constructor
   `string_fortified.h` is not a predicted saving: required node bytes still
   need initialization.
3. [#370](https://github.com/nouprax/markdown-core/issues/370): absorbed Text
   uses the existing finish-kind projection and the same completion ordering
   as ENTER. Kind completion still precedes observation at the correct word
   depth. The normal event path keeps its locally cached plan/observer.
4. [#371](https://github.com/nouprax/markdown-core/issues/371): successful
   strbuf growth establishes `ptr[size] == 0`, including the sentinel-to-owned
   transition. Allocation failure preserves the old value. An allocator seam
   filling fresh memory with `0xa5` reproduced the old empty buffer's non-NUL
   terminator; this is a buffer-contract defect, not a claim of a reproduced
   public-entry overread.
5. [#373](https://github.com/nouprax/markdown-core/issues/373): kind conversion
   uses the same record capacity/ownership rule as construction. An external
   replacement is reserved before old fields are destroyed. A fitting record
   reuses the cell after destruction, with infallible initialization; node
   identity, links and element-owned state survive. All conversion callers
   were checked: Setext and pipe-table conversion borrow no old record after
   success; HTML comment conversion restores its borrowed literal only on
   rejection/failure, before any destructive commit. Existing cell storage
   preserves the node address just as an external replacement does.
6. [#374](https://github.com/nouprax/markdown-core/issues/374): the new census
   reporter supports valid `--case` artifacts, whose pair registry remains
   complete while the measured cases are a subset. A proof or boundary is
   reported only when both required scale-1 documents were measured. An empty
   reference cohort has no ratio. This fixes the Codex review finding in the
   new reporting tool, rather than attributing it to the baseline parser.
7. [#375](https://github.com/nouprax/markdown-core/issues/375): the same-job
   artifact stores only the remeasured Core profiles under `baseline/`; its
   reference profiles are shared with the current report. Explicit `--baseline`
   selection reads this ownership layout after checking revision, corpus,
   proof, runtime, reference binary and copied-measurement identity. It fails
   on mismatches or missing evidence instead of searching unrelated paths.

Tests cover poisoned cell reuse across zero/inline/external payload kinds,
buffer growth and failed growth, block storage acquisition and release, and
completion-before-observation for absorbed escaped spaces. Conversion tests
cover refused allocation, both record backings, old owned fields and nodes
that outlive their parser pool. Existing lifecycle,
OOM, mutation, complexity and full-AST parity suites remain required.

## Baseline census

Local validation of the implementation: Release, Debug, ASan and UBSan each
passed all 90 CTest entries; all 150 script tests passed. All 464 benchmark
documents produced byte-identical complete AST dumps against main's tree.
The pinned-oracle audits passed 714 CommonMark and 97 GFM inputs, with their
existing declared divergences/projections; the domain/boundary audit passed.
Allocator ownership, attachment order, finish-hook shapes, node integrity,
source positions, reference ordering, source inventories and repository/CI
policy audits passed. C compilation with warnings as errors and the pinned
clang-format 23.1.0 check passed. These checks establish behavior and ownership;
the controlled hosted measurement below establishes the instruction impact.

The following section is generated by `scripts/report-performance.mjs` from
the artifact above. Final results on the same corpus follow this baseline census.

### Generated baseline census

Corpus: `1f863c0b79216e71005af20c5324f7288032665855cfd4f802cf9c8f9850a17e`; pairing: `316c545d69bf07ea7062ba4af10188efec23c0fbc48c7bf26bd19ced27569ad6`.

464 measured documents. Same-input reference cohort: **224 documents**; CommonMark uses cmark, GFM uses cmark-gfm, and every nonempty carries declaration is excluded. The JSON lists every included and excluded document. Proof pairs use scale 1 and are a separate cohort.

Ir is Callgrind's instruction-read count, not elapsed time. Parse-path costs come from the harness call edge. Program self costs below include process startup, input loading, receipts and shared libc leaves. Allocator self is every cost line attributed to malloc.c, including harness allocation. It must not be subtracted from parse-path Ir or described as exact parse-only allocator cost.

| Accounting, same-input cohort | Core Ir | Reference Ir | Ratio |
| --- | ---: | ---: | ---: |
| Source | 3,957,893,591 | 3,032,117,335 | 1.305× |
| AST | 3,749,880,072 | 3,599,972,899 | 1.042× |
| Complete parse | 8,477,614,640 | 7,466,802,885 | 1.135× |
| Outside stages, inside parse | 769,840,977 | 834,712,651 | 0.922× |
| Whole program self | 8,511,255,014 | 7,860,255,331 | 1.083× |
| Allocator program self | 1,113,719,419 | 3,488,997,857 | 0.319× |
| Whole program excluding allocator | 7,397,535,595 | 4,371,257,474 | 1.692× |

### Full Core corpus

| Source Ir | AST Ir | Complete parse Ir | Outside stages Ir |
| ---: | ---: | ---: | ---: |
| 7,976,574,658 | 7,313,869,386 | 16,893,899,025 | 1,603,454,981 |

### Proved domains

| Reference | Pairs | Median Grammar | Median Shape | Median Same-job |
| --- | ---: | ---: | ---: | ---: |
| cmark | 40 | 1.009× | 1.368× | 1.393× |
| cmark-gfm | 3 | 0.871× | 1.048× | 0.830× |

Grammar = A/B; Shape = B/R; Same-job = A/R. Grammar includes recognition and construction; Shape includes the complete two-stage implementation on B. Factor medians need not multiply. Boundary interventions and unproved historical substitutions are absent from this table.

### Source-attributed program self

Inline costs stay with their source header. This is an accounting partition, not proof of semantic equivalence between subsystem implementations.

| Subsystem | Core Ir | Reference Ir | Ratio |
| --- | ---: | ---: | ---: |
| Tree and structural headers | 1,488,352,387 | 453,753,359 | 3.280× |
| Block pipeline | 2,038,031,032 | 1,472,581,800 | 1.384× |
| Tables | 393,449,519 | 76,033,225 | 5.175× |
| Inlines | 1,148,147,782 | 913,162,502 | 1.257× |
| Buffers | 270,209,791 | 303,973,773 | 0.889× |
| Byte classification | 159,982,811 | 87,645,122 | 1.825× |
| UTF-8 | 22,077,610 | 55,613,452 | 0.397× |
| Inline libc string operations | 270,023,411 | 69,317,947 | 3.895× |

### Core function self, same-input cohort

| Function | Ir |
| --- | ---: |
| `S_parse_source` | 842,476,508 |
| `open_new_blocks` | 520,400,102 |
| `markdown_core_node_pool_new` | 514,294,439 |
| `walk_owned_trees` | 451,879,231 |
| `markdown_core_inline_parse_inline` | 419,972,326 |
| `_int_malloc` | 380,078,348 |
| `markdown_core_inline_make_literal` | 269,996,655 |
| `_int_free` | 214,916,086 |
| `S_free_nodes` | 199,342,559 |
| `markdown_core_inline_process_delimiters` | 189,356,608 |
| `markdown_core_text_parse` | 145,832,790 |
| `scan_element_start` | 140,890,755 |
| `markdown_core_inline_manual_scan_link_url` | 139,908,666 |
| `malloc_consolidate` | 137,417,963 |
| `markdown_core_table_try_open` | 132,465,259 |
| `free_node_as` | 131,537,927 |
| `markdown_core_inline_finish_inlines` | 117,218,728 |
| `__memcpy_avx_unaligned_erms` | 115,108,358 |
| `markdown_core_parser_add_child_validated` | 110,990,640 |
| `markdown_core_inline_start_inlines` | 109,383,045 |
| `malloc` | 94,886,072 |
| `free` | 93,233,535 |
| `markdown_core_block_advance_offset` | 88,925,915 |
| `markdown_core_strbuf_put` | 88,633,983 |
| `try_opening_table_block` | 85,848,071 |
| `markdown_core_consolidate_text_step` | 83,444,621 |
| `markdown_core_parser_extend_source_lines` | 82,250,607 |
| `markdown_core_block_parse_list_marker` | 80,749,117 |
| `markdown_core_node_can_contain_type` | 79,442,204 |
| `markdown_core_isspace` | 73,622,696 |
| `postprocess_text` | 71,061,388 |
| `match` | 69,687,151 |
| `try_paragraph` | 66,581,446 |
| `scan_delimiter` | 64,154,441 |
| `realloc` | 63,619,499 |

### All proved pairs

| Case | Reference | A Ir | B Ir | R Ir | Grammar | Shape | Same-job |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `proof-grid-cell-dialect` | cmark | 20,640,437 | 10,244,849 | 6,966,587 | 2.015× | 1.471× | 2.963× |
| `proof-leaf-promotion-dialect` | cmark | 33,721,757 | 20,498,910 | 14,336,794 | 1.645× | 1.430× | 2.352× |
| `proof-anonymous-container-dialect` | cmark | 26,307,186 | 19,708,850 | 13,135,098 | 1.335× | 1.500× | 2.003× |
| `proof-named-container-dialect` | cmark | 24,167,155 | 19,065,264 | 12,699,260 | 1.268× | 1.501× | 1.903× |
| `proof-loose-definition-dialect` | cmark | 39,652,531 | 31,353,588 | 21,316,365 | 1.265× | 1.471× | 1.860× |
| `proof-leaf-fence-dialect` | cmark | 17,324,407 | 13,694,071 | 9,558,140 | 1.265× | 1.433× | 1.813× |
| `proof-opaque-formula-dialect` | cmark | 26,890,012 | 22,098,309 | 15,916,791 | 1.217× | 1.388× | 1.689× |
| `proof-opaque-display-dialect` | cmark | 24,711,572 | 20,590,181 | 14,908,073 | 1.200× | 1.381× | 1.658× |
| `proof-leaf-formula-dialect` | cmark | 20,861,092 | 18,823,537 | 13,143,194 | 1.108× | 1.432× | 1.587× |
| `proof-cite-normal-dialect` | cmark | 28,514,999 | 23,977,539 | 18,177,028 | 1.189× | 1.319× | 1.569× |
| `proof-leaf-comment-dialect` | cmark | 20,476,376 | 18,823,537 | 13,143,194 | 1.088× | 1.432× | 1.558× |
| `proof-roman-list-dialect` | cmark | 30,198,486 | 28,826,916 | 19,778,433 | 1.048× | 1.457× | 1.527× |
| `proof-upper-roman-list-dialect` | cmark | 30,180,766 | 28,826,916 | 19,778,433 | 1.047× | 1.457× | 1.526× |
| `proof-upper-list-dialect` | cmark | 28,551,054 | 28,069,630 | 19,253,775 | 1.017× | 1.458× | 1.483× |
| `proof-alpha-list-dialect` | cmark | 29,652,779 | 29,616,359 | 20,325,245 | 1.001× | 1.457× | 1.459× |
| `proof-enclosed-default-list-dialect` | cmark | 28,068,054 | 28,069,630 | 19,253,775 | 1.000× | 1.458× | 1.458× |
| `proof-decimal-list-dialect` | cmark | 29,616,359 | 29,616,359 | 20,325,245 | 1.000× | 1.457× | 1.457× |
| `proof-inline-directive-dialect` | cmark | 25,085,994 | 23,730,743 | 17,219,274 | 1.057× | 1.378× | 1.457× |
| `proof-default-list-dialect` | cmark | 29,447,006 | 29,616,359 | 20,325,245 | 0.994× | 1.457× | 1.449× |
| `proof-empty-directive-dialect` | cmark | 26,263,233 | 25,105,770 | 18,479,170 | 1.046× | 1.359× | 1.421× |
| `proof-cite-author-dialect` | cmark | 26,668,388 | 25,737,552 | 19,527,261 | 1.036× | 1.318× | 1.366× |
| `proof-record-span-dialect` | cmark | 18,537,951 | 17,653,476 | 13,592,042 | 1.050× | 1.299× | 1.364× |
| `proof-class-span-dialect` | cmark | 23,542,617 | 23,086,953 | 17,304,991 | 1.020× | 1.334× | 1.360× |
| `proof-cite-suppress-dialect` | cmark | 25,566,668 | 24,826,372 | 18,813,179 | 1.030× | 1.320× | 1.359× |
| `proof-opaque-comment-dialect` | cmark | 19,238,584 | 20,590,181 | 14,908,073 | 0.934× | 1.381× | 1.290× |
| `proof-run-super-dialect` | cmark | 26,165,748 | 26,265,188 | 21,933,676 | 0.996× | 1.197× | 1.193× |
| `proof-run-insertion-dialect` | cmark | 24,530,526 | 24,551,676 | 20,592,039 | 0.999× | 1.192× | 1.191× |
| `proof-run-mark-dialect` | cmark | 24,530,526 | 24,551,676 | 20,592,039 | 0.999× | 1.192× | 1.191× |
| `proof-run-strike-dialect` | cmark | 24,486,111 | 24,551,676 | 20,592,039 | 0.997× | 1.192× | 1.189× |
| `proof-run-sub-dialect` | cmark | 26,030,148 | 26,265,188 | 21,933,676 | 0.991× | 1.197× | 1.187× |
| `proof-leaf-directive-dialect` | cmark | 23,293,697 | 27,349,899 | 19,837,872 | 0.852× | 1.379× | 1.174× |
| `proof-cross-local-dialect` | cmark | 18,824,132 | 21,983,265 | 16,497,285 | 0.856× | 1.333× | 1.141× |
| `proof-cross-embed-absent-dialect` | cmark | 18,979,443 | 22,640,017 | 16,935,107 | 0.838× | 1.337× | 1.121× |
| `proof-cross-embed-empty-dialect` | cmark | 19,076,987 | 22,751,226 | 17,198,477 | 0.839× | 1.323× | 1.109× |
| `proof-cross-anchor-dialect` | cmark | 14,636,604 | 17,190,485 | 13,366,953 | 0.851× | 1.286× | 1.095× |
| `proof-cross-link-absent-dialect` | cmark | 19,034,002 | 23,194,222 | 17,407,873 | 0.821× | 1.332× | 1.093× |
| `pair-insertion-dialect` | cmark | 24,511,226 | 24,554,986 | 22,674,104 | 0.998× | 1.083× | 1.081× |
| `proof-cross-embed-dialect` | cmark | 15,106,418 | 17,893,375 | 13,985,180 | 0.844× | 1.279× | 1.080× |
| `proof-cross-link-empty-dialect` | cmark | 19,056,563 | 23,293,635 | 17,667,156 | 0.818× | 1.318× | 1.079× |
| `proof-cross-link-dialect` | cmark | 14,997,998 | 18,201,405 | 14,284,698 | 0.824× | 1.274× | 1.050× |
| `proof-task-value-dialect` | cmark-gfm | 29,573,468 | 29,573,468 | 28,220,079 | 1.000× | 1.048× | 1.048× |
| `proof-specimen-graph-dialect` | cmark-gfm | 21,198,978 | 26,927,420 | 25,546,068 | 0.787× | 1.054× | 0.830× |
| `proof-simple-matrix-dialect` | cmark-gfm | 28,248,241 | 32,449,769 | 40,351,066 | 0.871× | 0.804× | 0.700× |

### Reviewed boundaries

These are whole-document interventions, including recognition, construction and byte changes. Delta is neither an isolated feature price nor a Same-job quotient; it can be negative.

| Original | Without | Full Ir | Without Ir | Delta Ir | Delta / unit | Full/without bytes |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| pair-anchor-dialect | boundary-anchor-without | 13,171,978 | 9,760,176 | 3,411,802 | 2966.78 | 65590/50600 |
| pair-citegroup-dialect | boundary-citegroup-without | 25,910,098 | 20,697,647 | 5,212,451 | 3768.95 | 65547/51717 |
| pair-embed-dialect | boundary-embed-without | 8,524,634 | 8,289,482 | 235,152 | 543.08 | 65589/61259 |
| pair-metadataempty-dialect | boundary-metadataempty-without | 2,468,825 | 15,963 | 2,452,862 | 904.78 | 65570/6 |
| pair-metadata-dialect | boundary-metadata-without | 3,075,005 | 15,963 | 3,059,042 | 780.17 | 65650/6 |
| pair-tcaption-dialect | boundary-tcaption-without | 40,053,006 | 30,040,239 | 10,012,767 | 6309.24 | 65544/41262 |
| pair-specimenstart-dialect | boundary-specimenstart-without | 20,131,696 | 20,117,756 | 13,940 | 12.13 | 65571/64422 |
| pair-callout-dialect | boundary-callout-without | 13,287,738 | 13,025,082 | 262,656 | 366.84 | 65542/46926 |
| pair-headless-dialect | boundary-headless-without | 28,740,409 | 13,790,630 | 14,949,779 | 5245.54 | 65619/65573 |
| pair-sparsegrid-dialect | boundary-sparsegrid-without | 23,010,461 | 4,682,182 | 18,328,279 | 71594.84 | 65536/21504 |
| pair-caption-dialect | boundary-caption-without | 32,629,575 | 25,791,120 | 6,838,455 | 4205.69 | 65556/34146 |
| pair-deflist-dialect | boundary-deflist-without | 41,570,814 | 21,821,700 | 19,749,114 | 24748.26 | 65574/59988 |

## Final controlled comparison

The final parser code is `f8a1c368f27d6696349a359c60b4d3837ae068f8`, measured by
[run 35687536827](https://github.com/nouprax/markdown-core/actions/runs/35687536827),
job `106617594784`. CI, CodeQL, Release Dry Run and Attribute Benchmark all succeeded.
The subsequent #375 reporter change and report documentation do not alter this parser code.

- Artifact: `10677377286`, `stage-benchmark-8ebf910dab9fa5f0e2c232b463b63caf6d760278`.
- ZIP SHA-256: `904f9f65bfa4a324dcdac8e980564583593725b4fcb71435c860e7d3742fbdac`.
- Event base: `d01676ec17923349220267f0d488576e065c28f1`; the artifact name identifies the PR merge commit.
- The corpus, proofs, toolchain and reference binaries are unchanged. Every document SHA and every baseline stage/lifecycle Ir matches the fresh main rerun.
- This comparison uses the artifact's own baseline. Whole-program baseline self differs from the independent main run by 500 Ir over the reference cohort (process/harness work); stage and lifecycle costs are identical.

```sh
node scripts/report-performance.mjs build/final-stage build/final-performance
node scripts/report-performance.mjs build/final-stage build/final-baseline-performance --baseline
```

### Same-input cohort: controlled before/after

| Metric | Base Core Ir | Final Core Ir | Change | Base/reference | Final/reference |
| --- | ---: | ---: | ---: | ---: | ---: |
| Source | 3,957,893,591 | 3,717,354,927 | -6.08% | 1.305× | 1.226× |
| AST | 3,749,880,072 | 3,718,992,812 | -0.82% | 1.042× | 1.033× |
| Parse lifecycle, including teardown | 8,477,614,640 | 8,011,444,981 | -5.50% | 1.135× | 1.073× |
| Outside stages, inside lifecycle | 769,840,977 | 575,097,242 | -25.30% | 0.922× | 0.689× |
| Whole-program self | 8,511,254,514 | 8,045,061,219 | -5.48% | 1.083× | 1.024× |
| Allocator program self | 1,113,719,419 | 736,073,599 | -33.91% | 0.319× | 0.211× |
| Whole program excluding allocator self | 7,397,535,095 | 7,308,987,620 | -1.20% | 1.692× | 1.672× |

Across all 464 Core documents, source Ir falls **5.14%**, AST Ir **0.64%**,
and lifecycle Ir **4.64%**. These are instruction counts, not wall-clock speedups.

### Proved-domain medians

| Reference | Pairs | Grammar, before → after | Shape, before → after | Same-job, before → after |
| --- | ---: | ---: | ---: | ---: |
| cmark | 40 | 1.009× → 1.010× | 1.368× → 1.336× | 1.393× → 1.348× |
| cmark-gfm | 3 | 0.871× → 0.878× | 1.048× → 0.975× | 0.830× → 0.813× |

Both A and B two-stage Ir decrease in **all 43 proved pairs**. Grammar can rise
when B improves more than A; a larger A/B alone is not an absolute regression.
The highest remaining proved Same-job ratio is grid-cell at **2.922×**
(Grammar **2.085×**, Shape **1.401×**), followed by leaf promotion at **2.285×**.
The prior-candidate audit above found no new redundant geometry, ownership or
completion operation behind these remaining ratios. They remain diagnostic
priorities, not evidence that necessary grammar recognition may be skipped.

### Regression and mechanism checks

- Source gate: **464/464** pass the unchanged 2% per-document limit. The maximum increase is **0.782%** (`block-heading`, scale 1).
- No lifecycle case increases by more than 2%; the largest increase is **0.737%** (`pair-metadata-common`, scale 2). This is not a claim that every workload improves.
- The intermediate failing `split-attributes-link` scale-1 source case now improves **6.617%** against base.
- Block constructor → `strbuf_grow`: **2,849,071 → 0 calls** across the full corpus. Storage is acquired by writers; this does not claim that all later growth disappears.
- Node constructor self: **946,683,983 → 898,723,383 Ir**. Header-attributed inline memset work is part of function self, not an additional saving to sum.
- Absorbed-Text completion: **939,261 calls** before and after; inclusive edge **46,705,134 → 29,798,436 Ir**. Both completion and observation callbacks are retained. The shared static-inline helper has **zero out-of-line calls**, and the absorbed-Text callback performs no structure-descriptor lookup.
- Kind conversion → allocator: **24,159 → 0 calls** for the measured parser conversions. External replacement allocation remains covered by the public mutation OOM tests.
- A local allocator-seam census independently finds **6,596,956 → 4,950,324 calls** over all 464 inputs, no document with an increased call count, and no live allocation after document release. This is an auxiliary native-build count, not Linux Ir or process RSS.

All seven confirmed problems (#368–#371, #373–#375) are implemented in PR #372.
The remaining nonzero ratios do not establish a new correctness, complexity or
redundant-work defect. The twelve boundaries below are remeasured interventions;
their signed differences are not isolated feature prices or isomorphism claims.

### Generated final census


Corpus: `1f863c0b79216e71005af20c5324f7288032665855cfd4f802cf9c8f9850a17e`; pairing: `316c545d69bf07ea7062ba4af10188efec23c0fbc48c7bf26bd19ced27569ad6`.

464 measured documents. Same-input reference cohort: **224 documents**; CommonMark uses cmark, GFM uses cmark-gfm, and every nonempty carries declaration is excluded. The JSON lists every included and excluded document. Proof pairs use scale 1 and are a separate cohort.

Ir is Callgrind's instruction-read count, not elapsed time. Parse-path costs come from the harness call edge. Complete parse means the bench_parse_document lifecycle: parsing, the root receipt and document teardown; main's input loading and process setup are outside that edge. Program self costs below include process startup, input loading, receipts and shared libc leaves. Allocator self is every cost line attributed to malloc.c, including harness allocation. It must not be subtracted from parse-path Ir or described as exact parse-only allocator cost.

| Accounting, same-input cohort | Core Ir | Reference Ir | Ratio |
| --- | ---: | ---: | ---: |
| Source | 3,717,354,927 | 3,032,117,335 | 1.226× |
| AST | 3,718,992,812 | 3,599,972,899 | 1.033× |
| Complete parse | 8,011,444,981 | 7,466,802,885 | 1.073× |
| Outside stages, inside parse | 575,097,242 | 834,712,651 | 0.689× |
| Whole program self | 8,045,061,219 | 7,860,255,331 | 1.024× |
| Allocator program self | 736,073,599 | 3,488,997,857 | 0.211× |
| Whole program excluding allocator | 7,308,987,620 | 4,371,257,474 | 1.672× |

### Full Core corpus

| Source Ir | AST Ir | Complete parse Ir | Outside stages Ir |
| ---: | ---: | ---: | ---: |
| 7,566,349,484 | 7,267,322,881 | 16,109,422,438 | 1,275,750,073 |

### Proved domains

| Reference | Pairs | Median Grammar | Median Shape | Median Same-job |
| --- | ---: | ---: | ---: | ---: |
| cmark | 40 | 1.010× | 1.336× | 1.348× |
| cmark-gfm | 3 | 0.878× | 0.975× | 0.813× |

Grammar = A/B; Shape = B/R; Same-job = A/R. Grammar includes recognition and construction; Shape includes the complete two-stage implementation on B. Factor medians need not multiply. Boundary interventions and unproved historical substitutions are absent from this table.

### Source-attributed program self

Inline costs stay with their source header. This is an accounting partition, not proof of semantic equivalence between subsystem implementations.

| Subsystem | Core Ir | Reference Ir | Ratio |
| --- | ---: | ---: | ---: |
| Tree and structural headers | 1,474,138,830 | 453,753,359 | 3.249× |
| Block pipeline | 2,047,246,640 | 1,472,581,800 | 1.390× |
| Tables | 393,449,519 | 76,033,225 | 5.175× |
| Inlines | 1,148,147,782 | 913,162,502 | 1.257× |
| Buffers | 223,521,881 | 303,973,773 | 0.735× |
| Byte classification | 159,982,811 | 87,645,122 | 1.825× |
| UTF-8 | 22,077,610 | 55,613,452 | 0.397× |
| Inline libc string operations | 245,458,594 | 69,317,947 | 3.541× |

### Core function self, same-input cohort

| Function | Ir |
| --- | ---: |
| `S_parse_source` | 842,476,508 |
| `open_new_blocks` | 520,400,102 |
| `markdown_core_node_pool_new` | 489,744,542 |
| `walk_owned_trees` | 451,879,231 |
| `markdown_core_inline_parse_inline` | 419,972,326 |
| `markdown_core_inline_make_literal` | 269,996,655 |
| `_int_malloc` | 248,741,814 |
| `S_free_nodes` | 197,481,137 |
| `markdown_core_inline_process_delimiters` | 189,356,608 |
| `markdown_core_text_parse` | 145,832,790 |
| `_int_free` | 144,063,876 |
| `scan_element_start` | 140,890,755 |
| `markdown_core_inline_manual_scan_link_url` | 139,908,666 |
| `markdown_core_table_try_open` | 132,465,259 |
| `free_node_as` | 131,537,927 |
| `markdown_core_inline_finish_inlines` | 117,218,728 |
| `__memcpy_avx_unaligned_erms` | 115,340,148 |
| `markdown_core_inline_start_inlines` | 109,383,045 |
| `markdown_core_parser_add_child_validated` | 106,164,960 |
| `markdown_core_strbuf_put` | 90,875,369 |
| `markdown_core_block_advance_offset` | 88,925,915 |
| `malloc_consolidate` | 86,985,632 |
| `try_opening_table_block` | 85,848,071 |
| `markdown_core_consolidate_text_step` | 83,444,621 |
| `markdown_core_parser_extend_source_lines` | 82,250,607 |
| `markdown_core_block_parse_list_marker` | 80,749,117 |
| `markdown_core_node_can_contain_type` | 78,778,838 |
| `markdown_core_isspace` | 73,622,696 |
| `postprocess_text` | 71,061,388 |
| `match` | 69,687,151 |
| `try_paragraph` | 66,581,446 |
| `markdown_core_block_add_line` | 64,761,258 |
| `free` | 64,381,499 |
| `scan_delimiter` | 64,154,441 |
| `calloc` | 60,453,064 |

### All proved pairs

| Case | Reference | A Ir | B Ir | R Ir | Grammar | Shape | Same-job |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `proof-grid-cell-dialect` | cmark | 20,355,204 | 9,760,975 | 6,966,587 | 2.085× | 1.401× | 2.922× |
| `proof-leaf-promotion-dialect` | cmark | 32,755,301 | 19,811,586 | 14,336,794 | 1.653× | 1.382× | 2.285× |
| `proof-anonymous-container-dialect` | cmark | 25,166,719 | 18,567,438 | 13,135,098 | 1.355× | 1.414× | 1.916× |
| `proof-named-container-dialect` | cmark | 23,063,306 | 17,961,190 | 12,699,260 | 1.284× | 1.414× | 1.816× |
| `proof-loose-definition-dialect` | cmark | 37,447,377 | 29,867,204 | 21,316,365 | 1.254× | 1.401× | 1.757× |
| `proof-leaf-fence-dialect` | cmark | 16,637,546 | 13,240,402 | 9,558,140 | 1.257× | 1.385× | 1.741× |
| `proof-opaque-formula-dialect` | cmark | 26,268,284 | 21,531,383 | 15,916,791 | 1.220× | 1.353× | 1.650× |
| `proof-opaque-display-dialect` | cmark | 24,125,137 | 20,055,304 | 14,908,073 | 1.203× | 1.345× | 1.618× |
| `proof-leaf-formula-dialect` | cmark | 20,232,063 | 18,195,528 | 13,143,194 | 1.112× | 1.384× | 1.539× |
| `proof-cite-normal-dialect` | cmark | 27,886,562 | 23,381,281 | 18,177,028 | 1.193× | 1.286× | 1.534× |
| `proof-leaf-comment-dialect` | cmark | 19,846,343 | 18,195,528 | 13,143,194 | 1.091× | 1.384× | 1.510× |
| `proof-roman-list-dialect` | cmark | 28,424,446 | 27,053,117 | 19,778,433 | 1.051× | 1.368× | 1.437× |
| `proof-upper-roman-list-dialect` | cmark | 28,406,726 | 27,053,117 | 19,778,433 | 1.050× | 1.368× | 1.436× |
| `proof-inline-directive-dialect` | cmark | 24,556,944 | 23,223,708 | 17,219,274 | 1.057× | 1.349× | 1.426× |
| `proof-upper-list-dialect` | cmark | 26,824,942 | 26,343,471 | 19,253,775 | 1.018× | 1.368× | 1.393× |
| `proof-empty-directive-dialect` | cmark | 25,659,220 | 24,528,916 | 18,479,170 | 1.046× | 1.327× | 1.389× |
| `proof-alpha-list-dialect` | cmark | 27,829,043 | 27,792,623 | 20,325,245 | 1.001× | 1.367× | 1.369× |
| `proof-enclosed-default-list-dialect` | cmark | 26,341,942 | 26,343,471 | 19,253,775 | 1.000× | 1.368× | 1.368× |
| `proof-decimal-list-dialect` | cmark | 27,792,623 | 27,792,623 | 20,325,245 | 1.000× | 1.367× | 1.367× |
| `proof-default-list-dialect` | cmark | 27,623,270 | 27,792,623 | 20,325,245 | 0.994× | 1.367× | 1.359× |
| `proof-record-span-dialect` | cmark | 18,171,036 | 17,296,457 | 13,592,042 | 1.051× | 1.273× | 1.337× |
| `proof-cite-author-dialect` | cmark | 26,008,932 | 25,097,236 | 19,527,261 | 1.036× | 1.285× | 1.332× |
| `proof-class-span-dialect` | cmark | 23,009,541 | 22,561,157 | 17,304,991 | 1.020× | 1.304× | 1.330× |
| `proof-cite-suppress-dialect` | cmark | 24,930,435 | 24,208,733 | 18,813,179 | 1.030× | 1.287× | 1.325× |
| `proof-opaque-comment-dialect` | cmark | 18,697,585 | 20,055,304 | 14,908,073 | 0.932× | 1.345× | 1.254× |
| `proof-run-super-dialect` | cmark | 25,525,993 | 25,625,433 | 21,933,676 | 0.996× | 1.168× | 1.164× |
| `proof-run-insertion-dialect` | cmark | 23,930,172 | 23,951,322 | 20,592,039 | 0.999× | 1.163× | 1.162× |
| `proof-run-mark-dialect` | cmark | 23,930,172 | 23,951,322 | 20,592,039 | 0.999× | 1.163× | 1.162× |
| `proof-run-strike-dialect` | cmark | 23,885,757 | 23,951,322 | 20,592,039 | 0.997× | 1.163× | 1.160× |
| `proof-run-sub-dialect` | cmark | 25,390,393 | 25,625,433 | 21,933,676 | 0.991× | 1.168× | 1.158× |
| `proof-cross-local-dialect` | cmark | 18,339,086 | 21,487,382 | 16,497,285 | 0.853× | 1.302× | 1.112× |
| `proof-leaf-directive-dialect` | cmark | 21,972,698 | 26,700,664 | 19,837,872 | 0.823× | 1.346× | 1.108× |
| `proof-cross-embed-absent-dialect` | cmark | 18,460,618 | 22,129,475 | 16,935,107 | 0.834× | 1.307× | 1.090× |
| `proof-cross-embed-empty-dialect` | cmark | 18,572,457 | 22,254,843 | 17,198,477 | 0.835× | 1.294× | 1.080× |
| `proof-cross-anchor-dialect` | cmark | 14,279,447 | 16,823,604 | 13,366,953 | 0.849× | 1.259× | 1.068× |
| `pair-insertion-dialect` | cmark | 24,151,063 | 24,194,823 | 22,674,104 | 0.998× | 1.067× | 1.065× |
| `proof-cross-link-absent-dialect` | cmark | 18,520,958 | 22,668,411 | 17,407,873 | 0.817× | 1.302× | 1.064× |
| `proof-cross-embed-dialect` | cmark | 14,725,896 | 17,521,440 | 13,985,180 | 0.840× | 1.253× | 1.053× |
| `proof-cross-link-empty-dialect` | cmark | 18,557,640 | 22,782,459 | 17,667,156 | 0.815× | 1.290× | 1.050× |
| `proof-cross-link-dialect` | cmark | 14,624,850 | 17,821,064 | 14,284,698 | 0.821× | 1.248× | 1.024× |
| `proof-task-value-dialect` | cmark-gfm | 27,507,980 | 27,507,980 | 28,220,079 | 1.000× | 0.975× | 0.975× |
| `proof-specimen-graph-dialect` | cmark-gfm | 20,767,228 | 26,488,287 | 25,546,068 | 0.784× | 1.037× | 0.813× |
| `proof-simple-matrix-dialect` | cmark-gfm | 27,569,784 | 31,401,094 | 40,351,066 | 0.878× | 0.778× | 0.683× |

### Reviewed boundaries

These are whole-document interventions, including recognition, construction and byte changes. Delta is neither an isolated feature price nor a Same-job quotient; it can be negative.

| Original | Without | Full Ir | Without Ir | Delta Ir | Delta / unit | Full/without bytes |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| pair-anchor-dialect | boundary-anchor-without | 12,865,436 | 9,453,976 | 3,411,460 | 2966.49 | 65590/50600 |
| pair-citegroup-dialect | boundary-citegroup-without | 25,812,486 | 20,636,567 | 5,175,919 | 3742.53 | 65547/51717 |
| pair-embed-dialect | boundary-embed-without | 8,384,547 | 8,148,601 | 235,946 | 544.91 | 65589/61259 |
| pair-metadataempty-dialect | boundary-metadataempty-without | 2,468,825 | 15,979 | 2,452,846 | 904.78 | 65570/6 |
| pair-metadata-dialect | boundary-metadata-without | 3,075,007 | 15,979 | 3,059,028 | 780.17 | 65650/6 |
| pair-tcaption-dialect | boundary-tcaption-without | 38,695,954 | 28,706,101 | 9,989,853 | 6294.80 | 65544/41262 |
| pair-specimenstart-dialect | boundary-specimenstart-without | 19,811,149 | 19,797,097 | 14,052 | 12.23 | 65571/64422 |
| pair-callout-dialect | boundary-callout-without | 12,706,432 | 12,497,474 | 208,958 | 291.84 | 65542/46926 |
| pair-headless-dialect | boundary-headless-without | 28,603,749 | 13,706,684 | 14,897,065 | 5227.04 | 65619/65573 |
| pair-sparsegrid-dialect | boundary-sparsegrid-without | 22,871,972 | 4,613,353 | 18,258,619 | 71322.73 | 65536/21504 |
| pair-caption-dialect | boundary-caption-without | 31,814,805 | 24,848,557 | 6,966,248 | 4284.29 | 65556/34146 |
| pair-deflist-dialect | boundary-deflist-without | 40,102,472 | 21,826,441 | 18,276,031 | 22902.29 | 65574/59988 |
