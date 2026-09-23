# Grammar-equivalent parse benchmarks

The parse benchmark uses one corpus: [grammar-corpus.json](grammar-corpus.json).
Its 194 certificates cover all 30 syntax-guide features and 32 registered
elements: 182 complete declared-language pairs and 12 local boundary pairs.
The [mathematical proofs](../../../docs/architecture/benchmark-grammar-corpus.md)
and [feature ledger](../../../docs/architecture/benchmark-grammar-coverage.md)
define their exact domains. Shared CommonMark/GFM features use identical input;
extensions use reversible source-grammar transformations or local boundaries.

The deterministic generator emits 412 documents, each measured once.
It varies independent fields, widths, repetition and recursion. Every finite
lexical alternative is exercised in the generated input. The catalog includes two
reviewable examples per certificate. There is no separate scenario manifest,
AST-pair census, ownership model or legacy measurement mode.

## Run and inspect

```sh
# Generate the exact documents and proof metadata without native parsers.
pnpm benchmark --corpus-only

# Linux with Callgrind and the pinned references.
scripts/init-environment.sh --install oracle-cmark oracle-cmark-gfm
pnpm benchmark

# Measure a whole family, including the selected input's counterpart/hosts.
pnpm benchmark --case insertion-strong-paired-dialect

# Remeasure a full base revision with the current corpus and harness.
pnpm benchmark --baseline-ref FULL_40_CHARACTER_COMMIT_SHA
```

The driver uses one fixed corpus. Each family starts with 12 derivation units,
expanded as needed to enumerate its finite grammar alternatives. `--out DIR`
changes the output directory and `--quiet` suppresses per-document progress. A selected boundary includes its complete hosts as well
as both local inputs. Unknown names fail before builds begin.

Outputs default to `build/benchmark-grammar`:

- `stages.md` and `stages.json`: every certified comparison, separate stages,
  instruction/data-reference counts and complete toolchain provenance.
- `corpus/`: generated documents, byte digests, concrete grammars, normal forms,
  derivations and lossless boundary partitions.
- `callgrind/`: raw profiles for each measured document and applicable engine.
- `baseline/`: the same inputs measured against the selected Core revision.

CI measures this corpus once, uploads structured reports for its trusted PR
publisher, and archives reports, inputs and raw profiles. The PR comment shows
Core/base source regressions and A/B, B/R and A/R for each certificate.
Build trees and reference checkouts are not uploaded.

## Reading a comparison

A is Core on the dialect encoding; B is Core on the common encoding; R is the
pinned cmark or cmark-gfm on the common encoding. A/B includes the complete
syntax/frame change. B/R measures implementations on the same common input.
A/R is the combined comparison. Byte counts remain visible because fixed
frame transformations need not preserve source length.

`paired-*` rows cover their complete declared languages. `boundary-*` rows
cover only the certified local grammar in its explicit entry envelope.
`host-*` rows record Core's complete document cost, including unmatched work;
no reference is run on those hosts and their cost is not a certified ratio.
Local and host costs are not additive. Grammar equivalence does not assert
equal native ASTs, output work, or globally optimal instruction counts.

The source-language proof checks run in
`node --test scripts/tests/grammar-corpus.test.mjs`. Native parser correctness
belongs to parity and regression. The common runner retains its historical
nonempty input/root-child receipt guard; it neither validates feature hits nor
establishes grammar equivalence.

## The two stages

| Stage | Core entry | Reference entry |
| --- | --- | --- |
| `source_to_buffer` | `markdown_core_parse_document_with_setup → S_parse_source` | `bench_parse_document → cmark_parser_feed` |
| `buffer_to_ast` | `markdown_core_parse_document_with_setup → S_finish_parse` | `bench_parse_document → cmark_parser_finish` |

Ratios use the sum of these two stages. Parser setup and release remain visible
as `outsideStagesIr` beside the full `parsePathIr`; they are not silently
amortized into parsing. Raw profiles preserve hot functions and stage edges.
Callgrind calling contexts separate source reading from nested source work
entered during AST construction.

Ir counts executed instructions, not elapsed time. It does not price cache
misses or dependency stalls. Data references and the pinned cache geometry
provide additional diagnostics. Counts depend on the compiler, libraries and
build options; measurements with different provenance are not interchangeable.

## Build and measurement provenance

The benchmark preset defines the compiler and release options. The driver
builds every engine, validates actual compile commands and stage symbols, and
records compiler programs, code-generation target, effective flags, static
reference pins, loaded libraries, libc dispatch and runner digests. Build
stamps invalidate incompatible caches. Core and references use separate build
trees, and output directories cannot overlap those trees.

Measurements run from a dedicated directory in an explicitly constructed
environment. Compiler probes must agree on repeated reads. Response-file
flags are rejected because their contents would otherwise escape the report's
identity. `corpus.digest` binds the exact selected input bytes; grammar identity
also binds the source-language definitions, proofs and coverage declarations.
Schema 5 identifies each workload by its case name and byte digest; it has no
scale dimension or separate pairing registry.

With `--baseline-ref`, only engine source comes from the base revision. The
current harness, corpus, preset and reference binaries are used on both sides.
Each document's `source_to_buffer` Ir must be at most 1.02 times its baseline.
AST improvements and aggregate medians cannot hide a source-stage regression.
This gate does not replace inspection of complete parse cost and allocation
behavior.

## The attribute grammar, against lexbor

`pnpm benchmark:attributes` remains an independent comparison of Core's
attribute-list grammar with the pinned lexbor HTML attribute tokenizer. Their
authored spellings differ; the workload and recovered attribute values are
defined by `scripts/benchmark-attributes.mjs`. It builds both implementations
with recorded flags and measures scanning, decoding and release on the harness
entry edge. Its reports and raw profiles live in `build/benchmark-attributes`.

This operation-level comparison is not pooled into document-parser ratios.
It has its own provenance and input receipts, and remains a separate CI job.
