# Ownership-preserving performance rerun

The six fixes reduce complete-parse Ir by **1.04%** on the 224-document
same-input cohort and **1.85%** across all 464 Core inputs. Corrected grid-cell
x1 improves **2.693× → 2.634×**, and loose-definition **1.666× → 1.538×**
against their reference alternatives. All **464/464 Source gates pass** at
the unchanged 2% limit. Same-input Source Ir increases 0.14%, while AST Ir
decreases 2.37%; the aggregate improvement does not imply every stage improved.

This rerun follows the structural audit in PR #380 and the six parser fixes in
PR #381. It uses the corrected v2 corpus for both revisions. The old v1
`grid-cell`, `loose-definition`, `inline-directive` and `empty-directive`
comparisons erased a semantic owner and are withdrawn. Changes to those old
reference inputs are not parser speedups.

The paired domain proof preserves ordered children, semantic owners, literals
and declared fields, modulo the explicitly documented grammar-role mapping.
It does not prove that recognizing two different concrete spellings requires
the same instructions. Grammar = Core(A)/Core(B), Shape = Core(B)/reference(B),
and Same = Core(A)/reference(B). Both stages are included in these quotients;
none is a pure scanner or node-constructor microbenchmark.

## Fixes and their invariants

| Issue | Redundant operation | Replacement and preserved boundary |
| --- | --- | --- |
| #376 | Build and discard three Text nodes for a recognized opaque formula | Direct leaf construction for fixed owners; dynamic containment remains deferred in its original decision order, and both paths share the literal/scope constructor |
| #377 | Allocate/copy a second formula payload and literal on promotion | Reserve the destination first, acquire borrowed storage if necessary, then move ownership; owned trim retains the allocation base and NUL terminator |
| #378 | Discard directive attribute recognition before decoding | One memo survives recognition and construction, and is disposed/accounted once on every outcome |
| #379 | Broadcast each word/affix boundary to every delimiter rule and residue | Shared monotone boundary floors combined with rule-specific failed-search floors |
| #382 | Rescan the same complete grid border in several topology passes | One immutable whole-line grammar fact per query; partial cell intervals still receive independent exact recognition |
| #383 | Replay table lookahead for a caption successor that cannot start any table | Shared raw-source necessary-condition admission at candidate entry points; no change to caption/definition precedence or supported table grammars |

The grid change has no one-column shortcut. Recognition work is bounded by the
whole-line extents plus actual interior cell intervals, with tests for 1/3/17
columns, narrow/wide cells, rows, UTF-8, prefixes, alignment, partial walls and
mixed markers. The caption gate reasons about physical lines and dash-run
counts: stripping a container prefix cannot add or split a dash run. A positive
admission still requires ordinary grammar recognition. CR, CRLF, LF, EOF and
one-column pipe tables remain covered.

## Attribution limits

Grid topology, scalar/source mapping and parsing block contents inside cells
remain real work. Definition/caption disambiguation is required by precedence;
only impossible-candidate replay is removed. Named containers still recognize
names and fences and store their fields. Shared source processing supports a
larger grammar and source-position contract than cmark.

Tree/Table source-file quotients are accounting partitions. Header inlining,
allocator attribution and the cmark cohort's absence of table recognition make
them unsuitable as ratios for one equivalent semantic operation. Inclusive
call edges overlap with their descendants; they must not be added together to
predict savings. The before/after stage totals below are the controlled effect
of all six changes, including their bookkeeping costs.

## Validation

Release, Debug, ASan and UBSan each passed 90 CTest entries. All 179 script tests,
warning-as-error C compilation, ownership/parser audits, staged repository audit
and post-commit clean audit passed. All 43 proof domains passed the three-party
AST audit. The pinned CommonMark (714) and GFM (97) comparisons passed within
the repository's declared projection/divergence contracts.

Exact AST and source-position parity passed on 460 benchmark documents,
6,000 deterministic formula/delimiter/bracket/field combinations and 720
table/caption/container/newline compositions. The four extreme-depth benchmark
documents use pathological tests rather than quadratic pretty-printed dumps.
OOM tests cover payload transfers, owned/borrowed literals, directive cleanup
and the existing table transaction workspace.

Mutation checks independently disabled the grid fact cache and bypassed the
caption candidate admission gate. Each mutation failed its corresponding work
invariant; restoring the implementation passed the API suite again. These
checks establish that the new tests detect the former repeated work, not just
that the final output remains correct.

## Controlled measurement identity

Code commit `756fd72ae6c9b80a89457901737b7c7e2ff0221e`; baseline `75c2f1a0f2e79dc937084139f75f6e3dd0436856`.
[Run 35703046590](https://github.com/nouprax/markdown-core/actions/runs/35703046590) measured the PR merge tree `0018715e7f707d294ffcec7f98d8327a93ccdeaa`.
Artifact `benchmark-stages-0018715e7f707d294ffcec7f98d8327a93ccdeaa-1` (ID 10684160757); ZIP SHA256
`2a8209626a04e68e9e2257fefbb7bcc5514dba79cb9af26c5862ad8d8469fb34`. Both revisions use this job's exact same corpus,
harness, compiler, runtime libraries and reference-engine binaries. The census
reader checks those identities before combining the shared reference profiles
with the separately measured baseline Core profiles.

gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0; ldd (Ubuntu GLIBC 2.39-0ubuntu8.9) 2.39; valgrind-3.22.0.
Target: `march=x86-64 mtune=generic`; flags: `-O3 -DNDEBUG -g -fno-inline-functions-called-once -fvisibility=hidden`.

All **464/464** Source gates pass at the unchanged 2% per-document limit.
Largest Source ratio: **1.012180×** on `pair-comment-common` x1
(2,569,865 → 2,601,166 Ir). AST savings cannot offset this gate.

## Aggregate effect

| Cohort / accounting | Baseline Core Ir | PR Core Ir | Change | Baseline / reference | PR / reference |
| --- | ---: | ---: | ---: | ---: | ---: |
| Same input (224), Source | 3,724,667,866 | 3,729,960,391 | 0.14% | 1.226× | 1.228× |
| Same input (224), AST | 3,758,390,376 | 3,669,246,848 | -2.37% | 1.035× | 1.011× |
| Same input (224), Complete parse | 8,064,915,080 | 7,981,064,077 | -1.04% | 1.074× | 1.063× |
| All 464 Core inputs, Source | 7,573,662,423 | 7,550,526,352 | -0.31% | — | — |
| All 464 Core inputs, AST | 7,306,720,445 | 7,033,551,297 | -3.74% | — | — |
| All 464 Core inputs, Complete parse | 16,162,892,537 | 15,863,228,494 | -1.85% | — | — |

## Audited domains at both scales

These are two-stage Ir (Source + AST), with equal production-unit counts within
each pair. Both A and B absolute costs are shown: a Grammar quotient can rise
when B improves more than A. The four corrected ownership pairs are first.

| Pair | Scale | A before → after Ir | A change | B before → after Ir | R Ir | Grammar before → after | Shape before → after | Same before → after |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| grid-cell | x1 | 20,355,204 → 19,904,810 | -2.21% | 10,671,321 → 10,521,402 | 7,557,930 | 1.907× → 1.892× | 1.412× → 1.392× | 2.693× → 2.634× |
| grid-cell | x2 | 40,129,656 → 39,228,856 | -2.24% | 21,343,607 → 21,043,769 | 15,114,969 | 1.880× → 1.864× | 1.412× → 1.392× | 2.655× → 2.595× |
| loose-definition | x1 | 37,447,377 → 34,576,661 | -7.67% | 31,652,154 → 31,177,500 | 22,476,578 | 1.183× → 1.109× | 1.408× → 1.387× | 1.666× → 1.538× |
| loose-definition | x2 | 74,573,754 → 68,984,778 | -7.49% | 63,115,591 → 62,166,283 | 44,952,280 | 1.182× → 1.110× | 1.404× → 1.383× | 1.659× → 1.535× |
| inline-directive | x1 | 24,556,944 → 24,090,567 | -1.90% | 29,169,741 → 28,465,493 | 22,144,505 | 0.842× → 0.846× | 1.317× → 1.285× | 1.109× → 1.088× |
| inline-directive | x2 | 49,110,892 → 48,178,387 | -1.90% | 58,318,470 → 56,910,350 | 44,370,670 | 0.842× → 0.847× | 1.314× → 1.283× | 1.107× → 1.086× |
| empty-directive | x1 | 25,659,220 → 25,392,650 | -1.04% | 31,464,106 → 31,197,536 | 23,575,232 | 0.816× → 0.814× | 1.335× → 1.323× | 1.088× → 1.077× |
| empty-directive | x2 | 50,935,352 → 50,402,212 | -1.05% | 62,735,190 → 62,202,050 | 47,197,088 | 0.812× → 0.810× | 1.329× → 1.318× | 1.079× → 1.068× |
| leaf-promotion | x1 | 32,755,301 → 25,813,228 | -21.19% | 19,811,586 → 19,808,607 | 14,336,794 | 1.653× → 1.303× | 1.382× → 1.382× | 2.285× → 1.800× |
| leaf-promotion | x2 | 65,311,654 → 51,425,375 | -21.26% | 39,234,826 → 39,228,868 | 28,672,364 | 1.665× → 1.311× | 1.368× → 1.368× | 2.278× → 1.794× |
| leaf-fence | x1 | 16,637,546 → 16,055,290 | -3.50% | 13,240,402 → 13,238,416 | 9,558,140 | 1.257× → 1.213× | 1.385× → 1.385× | 1.741× → 1.680× |
| leaf-fence | x2 | 33,510,254 → 32,205,482 | -3.89% | 26,282,981 → 26,279,009 | 19,115,263 | 1.275× → 1.226× | 1.375× → 1.375× | 1.753× → 1.685× |
| leaf-formula | x1 | 20,232,063 → 19,579,400 | -3.23% | 18,195,528 → 18,192,797 | 13,143,194 | 1.112× → 1.076× | 1.384× → 1.384× | 1.539× → 1.490× |
| leaf-formula | x2 | 40,093,741 → 38,788,220 | -3.26% | 35,999,984 → 35,994,522 | 26,285,481 | 1.114× → 1.078× | 1.370× → 1.369× | 1.525× → 1.476× |
| opaque-formula | x1 | 26,268,284 → 21,685,563 | -17.45% | 21,531,383 → 21,264,813 | 15,916,791 | 1.220× → 1.020× | 1.353× → 1.336× | 1.650× → 1.362× |
| opaque-formula | x2 | 52,351,513 → 43,186,869 | -17.51% | 42,876,880 → 42,343,740 | 31,835,888 | 1.221× → 1.020× | 1.347× → 1.330× | 1.644× → 1.357× |
| opaque-display | x1 | 24,125,137 → 19,948,653 | -17.31% | 20,055,304 → 19,805,448 | 14,908,073 | 1.203× → 1.007× | 1.345× → 1.329× | 1.618× → 1.338× |
| opaque-display | x2 | 48,255,551 → 39,903,075 | -17.31% | 40,116,761 → 39,617,049 | 29,852,829 | 1.203× → 1.007× | 1.344× → 1.327× | 1.616× → 1.337× |
| anonymous-container | x1 | 25,166,719 → 24,155,672 | -4.02% | 18,567,438 → 18,291,718 | 13,135,098 | 1.355× → 1.321× | 1.414× → 1.393× | 1.916× → 1.839× |
| anonymous-container | x2 | 49,953,874 → 47,933,003 | -4.05% | 36,938,161 → 36,386,721 | 26,269,058 | 1.352× → 1.317× | 1.406× → 1.385× | 1.902× → 1.825× |
| named-container | x1 | 23,063,306 → 22,829,511 | -1.01% | 17,961,190 → 17,694,620 | 12,699,260 | 1.284× → 1.290× | 1.414× → 1.393× | 1.816× → 1.798× |
| named-container | x2 | 45,757,947 → 45,290,357 | -1.02% | 35,725,662 → 35,192,522 | 25,397,389 | 1.281× → 1.287× | 1.407× → 1.386× | 1.802× → 1.783× |

## Confirmation of the newly paired workloads

On corrected grid-cell x1, `scan_table_horizontal` self Ir changes from
**711,304 → 201,190**.
The total A saving in the table above also includes cache lookup and layout
costs; it is not the isolated scanner delta. The cached line remains 96 bytes
on LP64, the same size as the baseline line; the lexical byte result and end
offset use former alignment space.

On corrected loose-definition x1, the caption probe remains
**1,986 calls**, preserving its precedence obligation.
Its inclusive cost changes **6,228,177 → 3,460,988 Ir**.
The table source's lookahead calls change **3,972 → 1,986**.
These nested edges explain the mechanism; they are not additive savings.

The admitted-candidate recognizer consumes the opener's existing admission
proof. The shared admission predicate remains inline in the source step,
avoiding both revalidation and out-of-line transport of source geometry.

The residual grid x1 Grammar ratio is **1.892×** and Shape is **1.392×**.
The admitted grid recognizer still costs 7,903,753 inclusive Ir across 649
tables, and the cell-content source parsing edge costs 3,661,778 Ir across
649 calls. These are distinct grammar/topology and owned-content obligations,
not a repeat of the whole-border scan removed by #382. They identify where a
future design audit should focus; the proof does not establish that this
remaining implementation is instruction-minimal.

The residual loose-definition x1 Grammar ratio is **1.109×** and Shape is
**1.387×**. The remaining 1,986 caption queries preserve precedence, while
the ordinary source and inline pipelines still construct all term/body owners.
The final counters confirm elimination of the audited extra lookahead, rather
than elimination of the caption decision itself. Further improvement needs a
new redundant-work or data-layout argument, not a target inferred from A/R.

## Reproduce the census from the artifact

```sh
node scripts/report-performance.mjs ARTIFACT_DIR build/performance
node scripts/report-performance.mjs ARTIFACT_DIR build/performance-baseline --baseline
```

For a fresh controlled run on the pinned Linux environment:

```sh
node scripts/benchmark-stages.mjs --scale 2 --baseline-ref 75c2f1a0f2e79dc937084139f75f6e3dd0436856
```

## Complete final census

Corpus: `f53208346cd8dbd602fc6889476b52e382ae85ad9c4dd92353824c3eba51bc7e`; pairing: `58a81400281294bc868bde84b5afd6f9037d11f0ec2e6b4f5b34b619b9a43a0e`.

464 measured documents. Same-input reference cohort: **224 documents**; CommonMark uses cmark, GFM uses cmark-gfm, and every nonempty carries declaration is excluded. The JSON lists every included and excluded document. Proof pairs use scale 1 and are a separate cohort.

Ir is Callgrind's instruction-read count, not elapsed time. Parse-path costs come from the harness call edge. Complete parse means the bench_parse_document lifecycle: parsing, the root receipt and document teardown; main's input loading and process setup are outside that edge. Program self costs below include process startup, input loading, receipts and shared libc leaves. Allocator self is every cost line attributed to malloc.c, including harness allocation. It must not be subtracted from parse-path Ir or described as exact parse-only allocator cost.

| Accounting, same-input cohort | Core Ir | Reference Ir | Ratio |
| --- | ---: | ---: | ---: |
| Source | 3,729,960,391 | 3,037,817,394 | 1.228× |
| AST | 3,669,246,848 | 3,629,670,025 | 1.011× |
| Complete parse | 7,981,064,077 | 7,506,207,183 | 1.063× |
| Outside stages, inside parse | 581,856,838 | 838,719,764 | 0.694× |
| Whole program self | 8,014,682,169 | 7,902,259,813 | 1.014× |
| Allocator program self | 748,701,885 | 3,510,204,020 | 0.213× |
| Whole program excluding allocator | 7,265,980,284 | 4,392,055,793 | 1.654× |

### Full Core corpus

| Source Ir | AST Ir | Complete parse Ir | Outside stages Ir |
| ---: | ---: | ---: | ---: |
| 7,550,526,352 | 7,033,551,297 | 15,863,228,494 | 1,279,150,845 |

### Proved domains

| Reference | Pairs | Median Grammar | Median Shape | Median Same-job |
| --- | ---: | ---: | ---: | ---: |
| cmark | 40 | 1.000× | 1.304× | 1.307× |
| cmark-gfm | 3 | 0.879× | 0.963× | 0.798× |

Grammar = A/B; Shape = B/R; Same-job = A/R. Grammar includes recognition and construction; Shape includes the complete two-stage implementation on B. Factor medians need not multiply. Boundary interventions and unproved historical substitutions are absent from this table.

### Source-attributed program self

Inline costs stay with their source header. This is an accounting partition, not proof of semantic equivalence between subsystem implementations.

| Subsystem | Core Ir | Reference Ir | Ratio |
| --- | ---: | ---: | ---: |
| Tree and structural headers | 1,483,513,243 | 456,954,744 | 3.247× |
| Block pipeline | 2,052,938,564 | 1,476,409,529 | 1.390× |
| Tables | 398,784,256 | 76,033,225 | 5.245× |
| Inlines | 1,065,564,339 | 919,528,981 | 1.159× |
| Buffers | 225,680,139 | 305,840,119 | 0.738× |
| Byte classification | 160,758,975 | 88,326,810 | 1.820× |
| UTF-8 | 22,077,610 | 55,613,452 | 0.397× |
| Inline libc string operations | 246,938,253 | 69,505,635 | 3.553× |

### Core function self, same-input cohort

| Function | Ir |
| --- | ---: |
| `S_parse_source` | 843,403,773 |
| `open_new_blocks` | 521,443,562 |
| `markdown_core_node_pool_new` | 493,291,982 |
| `walk_owned_trees` | 454,842,388 |
| `markdown_core_inline_parse_inline` | 422,638,213 |
| `markdown_core_inline_make_literal` | 271,396,550 |
| `_int_malloc` | 253,092,621 |
| `S_free_nodes` | 198,994,741 |
| `_int_free` | 146,806,561 |
| `markdown_core_text_parse` | 145,832,790 |
| `markdown_core_inline_manual_scan_link_url` | 141,710,270 |
| `scan_element_start` | 141,325,530 |
| `markdown_core_table_try_open` | 138,190,495 |
| `free_node_as` | 132,772,632 |
| `markdown_core_inline_finish_inlines` | 117,681,302 |
| `__memcpy_avx_unaligned_erms` | 115,552,235 |
| `markdown_core_inline_start_inlines` | 109,383,045 |
| `markdown_core_parser_add_child_validated` | 106,686,690 |
| `markdown_core_inline_process_delimiters` | 101,882,953 |
| `markdown_core_strbuf_put` | 91,630,095 |
| `markdown_core_block_advance_offset` | 89,774,549 |
| `malloc_consolidate` | 88,389,552 |
| `try_opening_table_block` | 85,848,071 |
| `markdown_core_consolidate_text_step` | 83,444,621 |
| `markdown_core_parser_extend_source_lines` | 82,320,699 |
| `markdown_core_block_parse_list_marker` | 80,749,117 |
| `markdown_core_node_can_contain_type` | 79,444,418 |
| `markdown_core_isspace` | 74,304,384 |
| `postprocess_text` | 70,690,600 |
| `match` | 70,344,493 |
| `try_paragraph` | 66,819,607 |
| `free` | 65,516,409 |
| `markdown_core_block_add_line` | 64,761,258 |
| `scan_delimiter` | 64,154,441 |
| `calloc` | 62,181,619 |

### All proved pairs

| Case | Reference | A Ir | B Ir | R Ir | Grammar | Shape | Same-job |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `proof-grid-cell-dialect` | cmark | 19,904,810 | 10,521,402 | 7,557,930 | 1.892× | 1.392× | 2.634× |
| `proof-anonymous-container-dialect` | cmark | 24,155,672 | 18,291,718 | 13,135,098 | 1.321× | 1.393× | 1.839× |
| `proof-leaf-promotion-dialect` | cmark | 25,813,228 | 19,808,607 | 14,336,794 | 1.303× | 1.382× | 1.800× |
| `proof-named-container-dialect` | cmark | 22,829,511 | 17,694,620 | 12,699,260 | 1.290× | 1.393× | 1.798× |
| `proof-leaf-fence-dialect` | cmark | 16,055,290 | 13,238,416 | 9,558,140 | 1.213× | 1.385× | 1.680× |
| `proof-loose-definition-dialect` | cmark | 34,576,661 | 31,177,500 | 22,476,578 | 1.109× | 1.387× | 1.538× |
| `proof-cite-normal-dialect` | cmark | 27,579,202 | 23,105,561 | 18,177,028 | 1.194× | 1.271× | 1.517× |
| `proof-leaf-comment-dialect` | cmark | 19,846,343 | 18,192,797 | 13,143,194 | 1.091× | 1.384× | 1.510× |
| `proof-leaf-formula-dialect` | cmark | 19,579,400 | 18,192,797 | 13,143,194 | 1.076× | 1.384× | 1.490× |
| `proof-roman-list-dialect` | cmark | 28,018,658 | 26,647,329 | 19,778,433 | 1.051× | 1.347× | 1.417× |
| `proof-upper-roman-list-dialect` | cmark | 28,000,938 | 26,647,329 | 19,778,433 | 1.051× | 1.347× | 1.416× |
| `proof-upper-list-dialect` | cmark | 26,431,642 | 25,948,446 | 19,253,775 | 1.019× | 1.348× | 1.373× |
| `proof-opaque-formula-dialect` | cmark | 21,685,563 | 21,264,813 | 15,916,791 | 1.020× | 1.336× | 1.362× |
| `proof-alpha-list-dialect` | cmark | 27,412,034 | 27,375,614 | 20,325,245 | 1.001× | 1.347× | 1.349× |
| `proof-enclosed-default-list-dialect` | cmark | 25,948,642 | 25,948,446 | 19,253,775 | 1.000× | 1.348× | 1.348× |
| `proof-decimal-list-dialect` | cmark | 27,375,614 | 27,375,614 | 20,325,245 | 1.000× | 1.347× | 1.347× |
| `proof-default-list-dialect` | cmark | 27,206,261 | 27,375,614 | 20,325,245 | 0.994× | 1.347× | 1.339× |
| `proof-opaque-display-dialect` | cmark | 19,948,653 | 19,805,448 | 14,908,073 | 1.007× | 1.329× | 1.338× |
| `proof-class-span-dialect` | cmark | 22,767,249 | 22,318,865 | 17,304,991 | 1.020× | 1.290× | 1.316× |
| `proof-record-span-dialect` | cmark | 17,837,874 | 16,963,295 | 13,592,042 | 1.052× | 1.248× | 1.312× |
| `proof-cite-author-dialect` | cmark | 25,401,932 | 24,801,020 | 19,527,261 | 1.024× | 1.270× | 1.301× |
| `proof-cite-suppress-dialect` | cmark | 24,345,185 | 23,923,131 | 18,813,179 | 1.018× | 1.272× | 1.294× |
| `proof-opaque-comment-dialect` | cmark | 18,447,729 | 19,805,448 | 14,908,073 | 0.931× | 1.329× | 1.237× |
| `proof-run-super-dialect` | cmark | 24,940,653 | 25,040,093 | 21,933,676 | 0.996× | 1.142× | 1.137× |
| `proof-run-insertion-dialect` | cmark | 23,382,387 | 23,403,537 | 20,592,039 | 0.999× | 1.137× | 1.136× |
| `proof-run-mark-dialect` | cmark | 23,382,387 | 23,403,537 | 20,592,039 | 0.999× | 1.137× | 1.136× |
| `proof-run-strike-dialect` | cmark | 23,337,972 | 23,403,537 | 20,592,039 | 0.997× | 1.137× | 1.133× |
| `proof-run-sub-dialect` | cmark | 24,805,053 | 25,040,093 | 21,933,676 | 0.991× | 1.142× | 1.131× |
| `proof-cross-local-dialect` | cmark | 18,110,580 | 21,258,876 | 16,497,285 | 0.852× | 1.289× | 1.098× |
| `proof-leaf-directive-dialect` | cmark | 21,680,262 | 26,072,935 | 19,837,872 | 0.832× | 1.314× | 1.093× |
| `proof-inline-directive-dialect` | cmark | 24,090,567 | 28,465,493 | 22,144,505 | 0.846× | 1.285× | 1.088× |
| `proof-empty-directive-dialect` | cmark | 25,392,650 | 31,197,536 | 23,575,232 | 0.814× | 1.323× | 1.077× |
| `proof-cross-embed-absent-dialect` | cmark | 18,225,402 | 21,894,259 | 16,935,107 | 0.832× | 1.293× | 1.076× |
| `proof-cross-embed-empty-dialect` | cmark | 18,343,951 | 22,026,337 | 17,198,477 | 0.833× | 1.281× | 1.067× |
| `proof-cross-anchor-dialect` | cmark | 14,109,257 | 16,653,414 | 13,366,953 | 0.847× | 1.246× | 1.056× |
| `proof-cross-link-absent-dialect` | cmark | 18,278,666 | 22,426,119 | 17,407,873 | 0.815× | 1.288× | 1.050× |
| `proof-cross-embed-dialect` | cmark | 14,552,046 | 17,347,590 | 13,985,180 | 0.839× | 1.240× | 1.041× |
| `proof-cross-link-empty-dialect` | cmark | 18,322,424 | 22,547,243 | 17,667,156 | 0.813× | 1.276× | 1.037× |
| `pair-insertion-dialect` | cmark | 23,172,480 | 23,216,240 | 22,674,104 | 0.998× | 1.024× | 1.022× |
| `proof-cross-link-dialect` | cmark | 14,447,096 | 17,643,310 | 14,284,698 | 0.819× | 1.235× | 1.011× |
| `proof-task-value-dialect` | cmark-gfm | 27,188,453 | 27,188,453 | 28,220,079 | 1.000× | 0.963× | 0.963× |
| `proof-specimen-graph-dialect` | cmark-gfm | 20,379,976 | 26,240,529 | 25,546,068 | 0.777× | 1.027× | 0.798× |
| `proof-simple-matrix-dialect` | cmark-gfm | 27,582,665 | 31,385,871 | 40,351,066 | 0.879× | 0.778× | 0.684× |

### Reviewed boundaries

These are whole-document interventions, including recognition, construction and byte changes. Delta is neither an isolated feature price nor a Same-job quotient; it can be negative.

| Original | Without | Full Ir | Without Ir | Delta Ir | Delta / unit | Full/without bytes |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| pair-anchor-dialect | boundary-anchor-without | 12,730,886 | 9,313,676 | 3,417,210 | 2971.49 | 65590/50600 |
| pair-citegroup-dialect | boundary-citegroup-without | 25,250,988 | 20,429,117 | 4,821,871 | 3486.53 | 65547/51717 |
| pair-embed-dialect | boundary-embed-without | 8,331,721 | 8,095,775 | 235,946 | 544.91 | 65589/61259 |
| pair-metadataempty-dialect | boundary-metadataempty-without | 2,468,825 | 15,979 | 2,452,846 | 904.78 | 65570/6 |
| pair-metadata-dialect | boundary-metadata-without | 3,075,007 | 15,979 | 3,059,028 | 780.17 | 65650/6 |
| pair-tcaption-dialect | boundary-tcaption-without | 38,540,428 | 28,744,189 | 9,796,239 | 6172.80 | 65544/41262 |
| pair-specimenstart-dialect | boundary-specimenstart-without | 19,383,721 | 19,369,669 | 14,052 | 12.23 | 65571/64422 |
| pair-callout-dialect | boundary-callout-without | 12,433,636 | 12,235,418 | 198,218 | 276.84 | 65542/46926 |
| pair-headless-dialect | boundary-headless-without | 27,879,560 | 13,706,585 | 14,172,975 | 4972.97 | 65619/65573 |
| pair-sparsegrid-dialect | boundary-sparsegrid-without | 22,491,568 | 4,588,521 | 17,903,047 | 69933.78 | 65536/21504 |
| pair-caption-dialect | boundary-caption-without | 31,821,308 | 24,877,825 | 6,943,483 | 4270.28 | 65556/34146 |
| pair-deflist-dialect | boundary-deflist-without | 36,610,266 | 21,447,281 | 15,162,985 | 19001.23 | 65574/59988 |
