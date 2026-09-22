# Issue 363: fresh measurement and remaining work

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
It verifies the parse-stage partition and that function/file self partitions
sum to the profile's `totals`. Callgrind `summary` can differ from `totals`;
it is not substituted for the measured cost-line sum.

## Disposition of the previous candidates

| Previous item | Fresh evidence and disposition |
| --- | --- |
| Node lifecycle | The slab model from #362 is in main. Constructor self is 514,294,439 Ir over the current reference cohort. Recycled cells still clear the entire maximum record, even for kinds with no record: confirmed as #369. Block creation also allocates a content buffer before any content exists: #368. |
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
   No kind, input size or benchmark-cardinality branch is introduced.
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

Tests cover poisoned cell reuse across zero/inline/external payload kinds,
buffer growth and failed growth, block storage acquisition and release, and
completion-before-observation for absorbed escaped spaces. Existing lifecycle,
OOM, mutation, complexity and full-AST parity suites remain required.

## Baseline census

Local validation of the implementation: Release, Debug, ASan and UBSan each
passed all 90 CTest entries; all 147 script tests passed. All 464 benchmark
documents produced byte-identical complete AST dumps against main's tree.
The pinned-oracle audits passed 714 CommonMark and 97 GFM inputs, with their
existing declared divergences/projections; the domain/boundary audit passed.
Allocator ownership, attachment order, finish-hook shapes, node integrity,
source positions, reference ordering, source inventories and repository/CI
policy audits passed. C compilation with warnings as errors and the pinned
clang-format 23.1.0 check passed. These checks establish behavior and ownership;
the hosted before/after measurement below must establish performance.

The following section is generated by `scripts/report-performance.mjs` from
the artifact above. Fix results will be recorded separately on the same corpus.

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
