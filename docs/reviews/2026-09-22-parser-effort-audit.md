# Parse-effort re-audit of all structural pairs

> Historical report. The scenario suite and its commands were retired after PR #388.
> Reproduction requires [the original implementation](https://github.com/nouprax/markdown-core/tree/bd0b2a8ed9d7e3842f564915cbc7e2c6b6b7be8f).
> Current comparisons use the [grammar-certified corpus](../architecture/benchmark-grammar-corpus.md).


**43 structural proofs; 0 certificates of equal full-parser optimal effort.**
The previous AST/ownership proofs establish a commuting result mapping. They
do not establish a cost-preserving correspondence of admissible parsers. This
is a correction of the benchmark's proof target, not another owner mismatch.

The [cost model](https://github.com/nouprax/markdown-core/blob/bd0b2a8ed9d7e3842f564915cbc7e2c6b6b7be8f/docs/architecture/benchmark-parser-effort.md) specifies input,
control, memory, storage and output work, the complete correctness/failure
domain, and the two algorithm transformations sufficient to prove equal optima.
A cost-preserving trace of two particular parsers is a weaker result. The current
proofs supply neither that result nor the stronger theorem. Unproved does not
mean that unequal optima or impossibility has been proved.

## Measurement provenance

This is a reinterpretation of [run 35703046590](https://github.com/nouprax/markdown-core/actions/runs/35703046590),
code `756fd72ae6c9b80a89457901737b7c7e2ff0221e`, now merged by #381.
Artifact `benchmark-stages-0018715e7f707d294ffcec7f98d8327a93ccdeaa-1`
(ID 10684160757), ZIP SHA256
`2a8209626a04e68e9e2257fefbb7bcc5514dba79cb9af26c5862ad8d8469fb34`.

Original corpus: `f53208346cd8dbd602fc6889476b52e382ae85ad9c4dd92353824c3eba51bc7e`.
Original measurement pairing identity: `58a81400281294bc868bde84b5afd6f9037d11f0ec2e6b4f5b34b619b9a43a0e`.
Current interpretation: `parser-effort-v1`,
`f8c5975512e0a837bfa3e91599f55cd0898f4739196365d8c7d2cde47fd41b8d`.

No parser instructions were remeasured or altered for this audit. Mechanical
checks compare every A/B/R value and both same-input cohort aggregates with the
previous report; all are unchanged. Only interpretation and eligibility change.
The 464/464 Source gates and the same-input before/after improvements in the
[preceding report](2026-09-22-owner-preserving-performance.md) remain valid.

## Per-pair adjudication

All rows have status **unproved** for equal full-parser optimal effort. The
bytes and A/R values below are x1 diagnostics, not lower bounds or percentages
of removable overhead. An equal byte count is not a certificate. In particular,
the decimal-list identity is a same-input control, and the arbitrary-depth
insertion proof remains a structural theorem on its restricted sublanguage.

| Structural proof | A / B bytes | Descriptive A/R | Missing effort obligation |
| --- | ---: | ---: | --- |
| insertion-strong-v1 | 65640 / 65640 | 1.022× | Recursive isolated runs have matching token extents and trees. No bidirectional cost-preserving simulation covers full-grammar flanking, residue rules, contextual rejection and source-position obligations. |
| run-insertion-v2 | 65565 / 65565 | 1.136× | Equal-width isolated markers are a candidate local correspondence, not a cost theorem. Fixed payloads exclude interacting runs, escapes and context; the shared *** terminator also prevents a global marker-alphabet bijection for the strong substitutions. |
| run-mark-v2 | 65565 / 65565 | 1.136× | Equal-width isolated markers are a candidate local correspondence, not a cost theorem. Fixed payloads exclude interacting runs, escapes and context; the shared *** terminator also prevents a global marker-alphabet bijection for the strong substitutions. |
| run-strike-v2 | 65565 / 65565 | 1.133× | Equal-width isolated markers are a candidate local correspondence, not a cost theorem. Fixed payloads exclude interacting runs, escapes and context; the shared *** terminator also prevents a global marker-alphabet bijection for the strong substitutions. |
| run-super-v2 | 65540 / 65540 | 1.137× | Equal-width isolated markers are a candidate local correspondence, not a cost theorem. Fixed payloads exclude interacting runs, escapes and context; the shared *** terminator also prevents a global marker-alphabet bijection for the strong substitutions. |
| run-sub-v2 | 65540 / 65540 | 1.131× | Equal-width isolated markers are a candidate local correspondence, not a cost theorem. Fixed payloads exclude interacting runs, escapes and context; the shared *** terminator also prevents a global marker-alphabet bijection for the strong substitutions. |
| opaque-comment-v2 | 65536 / 61440 | 1.237× | Opaque output leaves do not equate closer recognition, delimiter-run lengths, escape/flanking decisions, or code-span whitespace normalization. |
| opaque-formula-v2 | 65550 / 65550 | 1.362× | Opaque output leaves do not equate closer recognition, delimiter-run lengths, escape/flanking decisions, or code-span whitespace normalization. |
| opaque-display-v2 | 65536 / 61440 | 1.338× | Opaque output leaves do not equate closer recognition, delimiter-run lengths, escape/flanking decisions, or code-span whitespace normalization. |
| leaf-comment-v2 | 65544 / 71006 | 1.510× | Different fence/promotion decisions and physical-line extents remain; the reversible terminal-LF field encoding does not prove equal trimming, normalization or recognition cost. |
| leaf-formula-v2 | 65544 / 71006 | 1.490× | Different fence/promotion decisions and physical-line extents remain; the reversible terminal-LF field encoding does not prove equal trimming, normalization or recognition cost. |
| leaf-fence-v2 | 65538 / 51636 | 1.680× | Different fence/promotion decisions and physical-line extents remain; the reversible terminal-LF field encoding does not prove equal trimming, normalization or recognition cost. |
| leaf-promotion-v2 | 65538 / 77454 | 1.800× | Different fence/promotion decisions and physical-line extents remain; the reversible terminal-LF field encoding does not prove equal trimming, normalization or recognition cost. |
| task-value-v2 | 65544 / 65544 | 0.963× | The singleton marker-value correspondence preserves ownership, but does not map the complete task-marker language, field representation and contextual decisions with equal cost. |
| record-span-v2 | 65562 / 66900 | 1.312× | Attribute/class members and link destinations/titles have different delimiters, decoding, termination and field-construction obligations despite the reversible field encoding. |
| cross-link-v2 | 65565 / 69936 | 1.011× | Cross path/anchor/label fields are encoded as URL/title fields. Recognizing and normalizing those grammars, bracket context and null/empty distinctions have no cost-preserving simulation. |
| cross-embed-v2 | 65550 / 69825 | 1.041× | Cross path/anchor/label fields are encoded as URL/title fields. Recognizing and normalizing those grammars, bracket context and null/empty distinctions have no cost-preserving simulation. |
| inline-directive-v2 | 65555 / 89904 | 1.088× | DirectiveLabel ownership is preserved, but nested image/link destinations, labels and link activation do not correspond to directive recognition step for step. |
| leaf-directive-v2 | 65546 / 70588 | 1.093× | DirectiveLabel ownership is preserved, but nested image/link destinations, labels and link activation do not correspond to directive recognition step for step. |
| anonymous-container-v2 | 65540 / 45200 | 1.839× | Fenced containers have open/close, name/attribute and line-boundary decisions; a quote has prefix continuation. Equal owned paragraphs do not remove those obligations. |
| alpha-list-v2 | 65556 / 65556 | 1.349× | Alphabetic, Roman or implicit markers encode values differently from decimal markers, with distinct continuation/disambiguation rules. Two fixed items do not prove equal conversion or recognition cost. |
| loose-definition-v2 | 65538 / 73482 | 1.538× | Term/body recognition and caption precedence differ from list/quote continuation. The extra reference owner repairs the output graph, not the recognition obligations. |
| grid-cell-v2 | 65549 / 27258 | 2.634× | Grid borders, scalar columns, rectangle/span validation and cell source mapping differ from list/quote prefixes. Equal output owners do not make these recognition problems isomorphic. |
| simple-matrix-v2 | 65576 / 66747 | 0.684× | Positional column discovery and pipe delimiter/escape recognition differ even with equal cell values and alignment fields. |
| class-span-v2 | 65538 / 65538 | 1.316× | Attribute/class members and link destinations/titles have different delimiters, decoding, termination and field-construction obligations despite the reversible field encoding. |
| cite-author-v2 | 65556 / 87408 | 1.301× | Citation-key/mode recognition and an explicit URI autolink have different validation and derived-text obligations; fixed key encoding establishes values, not equal effort. |
| cite-suppress-v2 | 65548 / 84276 | 1.294× | Citation-key/mode recognition and an explicit URI autolink have different validation and derived-text obligations; fixed key encoding establishes values, not equal effort. |
| cite-normal-v2 | 65540 / 81360 | 1.517× | Citation-key/mode recognition and an explicit URI autolink have different validation and derived-text obligations; fixed key encoding establishes values, not equal effort. |
| cross-link-absent-v2 | 65538 / 67524 | 1.050× | Cross path/anchor/label fields are encoded as URL/title fields. Recognizing and normalizing those grammars, bracket context and null/empty distinctions have no cost-preserving simulation. |
| cross-embed-absent-v2 | 65552 / 67480 | 1.076× | Cross path/anchor/label fields are encoded as URL/title fields. Recognizing and normalizing those grammars, bracket context and null/empty distinctions have no cost-preserving simulation. |
| cross-link-empty-v2 | 65552 / 71336 | 1.037× | Cross path/anchor/label fields are encoded as URL/title fields. Recognizing and normalizing those grammars, bracket context and null/empty distinctions have no cost-preserving simulation. |
| cross-embed-empty-v2 | 65555 / 71174 | 1.067× | Cross path/anchor/label fields are encoded as URL/title fields. Recognizing and normalizing those grammars, bracket context and null/empty distinctions have no cost-preserving simulation. |
| cross-anchor-v2 | 65565 / 66960 | 1.056× | Cross path/anchor/label fields are encoded as URL/title fields. Recognizing and normalizing those grammars, bracket context and null/empty distinctions have no cost-preserving simulation. |
| cross-local-v2 | 65555 / 65555 | 1.098× | Cross path/anchor/label fields are encoded as URL/title fields. Recognizing and normalizing those grammars, bracket context and null/empty distinctions have no cost-preserving simulation. |
| named-container-v2 | 65550 / 43700 | 1.798× | Fenced containers have open/close, name/attribute and line-boundary decisions; a quote has prefix continuation. Equal owned paragraphs do not remove those obligations. |
| upper-list-v2 | 65550 / 62100 | 1.373× | Alphabetic, Roman or implicit markers encode values differently from decimal markers, with distinct continuation/disambiguation rules. Two fixed items do not prove equal conversion or recognition cost. |
| roman-list-v2 | 65564 / 63792 | 1.417× | Alphabetic, Roman or implicit markers encode values differently from decimal markers, with distinct continuation/disambiguation rules. Two fixed items do not prove equal conversion or recognition cost. |
| upper-roman-list-v2 | 65564 / 63792 | 1.416× | Alphabetic, Roman or implicit markers encode values differently from decimal markers, with distinct continuation/disambiguation rules. Two fixed items do not prove equal conversion or recognition cost. |
| default-list-v2 | 65556 / 65556 | 1.339× | Alphabetic, Roman or implicit markers encode values differently from decimal markers, with distinct continuation/disambiguation rules. Two fixed items do not prove equal conversion or recognition cost. |
| enclosed-default-list-v2 | 65550 / 62100 | 1.348× | Alphabetic, Roman or implicit markers encode values differently from decimal markers, with distinct continuation/disambiguation rules. Two fixed items do not prove equal conversion or recognition cost. |
| decimal-list-v2 | 65556 / 65556 | 1.347× | The two measured source strings are identical, so A/B is a control. Core and the reference still support different complete parser contracts; identity of this input does not prove equal full-parser optima. |
| empty-directive-v2 | 65550 / 93955 | 1.077× | DirectiveLabel ownership is preserved, but nested image/link destinations, labels and link activation do not correspond to directive recognition step for step. |
| specimen-graph-v2 | 65583 / 66624 | 0.798× | Fresh fixed labels align definition/call edges, but no simulation covers full definition recognition, symbol lookup, hoisting, ordinals, resolution failures and source coordinates. |

## Consequences for profiling

Grid's 2.634× A/R is no longer a claim of 163.4% excess work above an equally
hard recognition problem. Definitions, containers and promotion likewise stay
useful diagnostic workloads, but a high quotient alone does not establish a
defect. Same-input before/after and concrete repeated-work counters can still
justify repairs; #381's scanner/cache, caption admission and ownership-transfer
results do not depend on an equal-optimal-effort assumption.

All 30 legacy witnesses were rechecked, and all 43 structural domains passed the
three-parser tree audit at both scales and their declared extremes. The 12
boundary interventions retain their existing, explicitly non-additive meaning.
The old formal-pair median and Same-job interpretation are withdrawn for the
whole cohort, not just the four pairs whose owners were fixed in #380.

## Reproduce

```sh
node scripts/report-performance.mjs ARTIFACT_DIR build/effort-census
node scripts/audit-corpus-pairs.mjs
node --test scripts/tests/*.test.mjs
```

The census JSON retains the original measurement digest and records the new
interpretation digest separately. Both report paths publish the effort gap for
each structural control and omit the equal-effort median. A future certificate
must satisfy the stronger theorem; a manifest flag or another matching AST
fixture cannot confer eligibility.
