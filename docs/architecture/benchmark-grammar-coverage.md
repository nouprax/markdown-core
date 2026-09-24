# Grammar benchmark feature acceptance

The benchmark's deliverable is a generated corpus with mathematical grammar
certificates. [T1–T7](benchmark-grammar-corpus.md) establish equivalence on each
declared language. [The machine ledger](../../packages/markdown-core/benchmarks/grammar-coverage.json)
binds coverage to every syntax specification, its sections, the registered C
elements. Native AST similarity is never the proof. Parser correctness remains
the responsibility of the existing parity and regression pipelines.

“All features” has the following executable acceptance conditions:

1. Every page in `docs/specs/dialect/` has generated, certified benchmark inputs.
   Every registered element has exactly one specification owner. No feature is
   admitted merely because a node of its kind appeared incidentally elsewhere.
2. Shared CommonMark/GFM syntax uses identical source and the identity proof.
   An extension counterpart retains all variable fields of its declared grammar,
   with both inverse laws and complete source consumption.
3. A boundary retains its complete host and an exhaustive byte partition, names
   the unmatched reference productions, and supplies a positive proof for the
   complete local field grammar. A local ratio never certifies a host ratio.
4. Every specification section has an explicit disposition in
   `scripts/benchmark/sections.mjs`: source-grammar sections name their
   certificates, while context-only sections explain why they introduce no new
   source production. Missing/renamed sections, stale links, nonexistent or empty
   certificate lists fail before the ledger can be regenerated. Every certificate
   must have a section link. Source digests additionally expose changed rules. Correctness fixtures and native output assertions
   remain in parity/regression; changing them does not change this proof identity.
5. The generated source, grammar, normal form, proof identity and
   Callgrind reports travel together. Filtering a benchmark selects both sides
   and, for boundaries, both complete hosts. Missing halves fail reporting.
6. Every generated input enumerates the full Cartesian product of finite grammar
   fields, including all 26 canonical ordinal alternatives and all three callout
   states. Requested unit counts are lower bounds; they cannot truncate this
   coverage. Callgrind measures these generated documents.

There are **30 specification features, 136 sections, 32 registered elements,
194 certificates and 424 generated documents**, each measured once.
Of the 136 sections, **132 link to grammar certificates and four are explicitly
context-only** (automatic-anchor output, anchor ownership, and two navigation
introductions). Cross-feature links reuse the same certificate; a documentation
heading does not demand another benchmark. Mixed sections link their source
productions and explicitly keep output-coordinate rules in parity/regression.
Of these, **182 certify whole declared languages and 12 certify local boundary
languages**. This is feature coverage, not a theorem that the union of these
restricted grammars equals every possible document of the unrestricted dialect.
In particular a boundary's residual is not proved impossible to match against
all imaginable reference grammars. The ledger exposes that distinction.

| Feature | Generated grammar coverage |
| --- | --- |
| Basics | Paragraphs, blanks, tabs, Unicode, LF/CR/CRLF, literal punctuation, escapes and named/decimal/hex entities. |
| Headings | All six ATX levels, closing hashes, both Setext levels, multiline and inline content. Same input; generated anchors are extra native output work. |
| Emphasis | Asterisk and underscore, strong, nested formatting and flanking in the declared domains. |
| Line breaks | Soft breaks and both hard-break spellings. |
| Thematic breaks | Three marker families and spaced markers. |
| Code | Code spans, backtick runs, padding/newlines, both fence characters, info and indented blocks. |
| HTML | Inline tags/attributes, raw blocks, processing instructions, declarations, CDATA, block/complete tags; comments have their own families. |
| Insertions | Recursive insertion/strong substitution, runs, protected code and rejected openers. |
| Highlights | Paired runs, formatted bodies and rejected openers. |
| Strikethrough | Same-input GFM pairs, cross-syntax pairs and rejected openers. |
| Super/subscript | Both markers, formatted content, empty superscript, escaped spaces and raw-space rejection. |
| Comments | Inline/block percent forms, empty comments, inline/block HTML comments and fallback. |
| Formulas | Dollar, backtick-dollar, doubled-backslash parentheses/brackets, blocks, formula fences, promotion and fallback. |
| Bracketed spans | Empty, nested, record/class forms and rejected suffixes. |
| Links/images | Direct links/images, all reference forms, duplicate and unresolved references, angle/bare autolinks; numeric dimensions have an explicit boundary. |
| Cross links/embeds | Absent/empty/raw labels, path/local/heading/block targets, embed forms, escaped pipes within tables and fallback; numeric dimensions remain a boundary. |
| Anchors | Explicit IDs, last/empty ID, Unicode/duplicate/empty automatic bases and implicit references. |
| Block IDs | Paragraph, list-item, list/quote/table container declarations and escaped markers. |
| Footnotes | GFM references and multi-block definitions, nested inline notes, cycles, duplicate/unused definitions, repeated calls and fallback. The GFM retention difference is documented for cost interpretation. |
| Citations | Author/suppressed/normal modes, groups, independent prefix/key/suffix and rejected keys. |
| Specimens | Definition/call equality, explicit nine-digit resets, anonymous/duplicate definitions and group reset suppression have constructive whole-language counterparts. |
| Lists | Three bullets, decimal delimiters, tight/loose/nested/empty items; alpha/Roman canonical finite domains and default markers have constructive proofs. |
| Tasks | Standard states, custom/Unicode markers, nested ordered tasks, heading bodies and missing-separator fallback. |
| Definition lists | Tight/loose, multiple/empty definitions and multiple blocks. |
| Quotes/callouts | Same-input plain/nested/lazy quotes; variable type and all three collapse states have invertible counterparts. |
| Tables | Same-input pipe/alignment/ragged/escaped-pipe forms, both captions; simple, headless, multiline, grid, spans, sparse rows and footer use geometry boundaries. |
| Directives | Inline label/attribute/both, Unicode names, leaf forms, named/nameless/unbraced/nested containers and fallback. |
| Properties | All ten fields, text/number/bool/null/list distinctions, BOM, invalid-before-valid/duplicates, unknown fields and literal indentation in complete boundary hosts. |
| Attributes | Quoted/unquoted/empty/bare values, ordered duplicates, escapes/entities/newline, IDs/classes and every documented owner category. |
| Precedence | Nested block owners, literal islands, bracket/suffix and rejected-prefix cases; the declared source productions cover precedence interactions. |

The precise restricted productions, including all punctuation, are in
`grammar-corpus.json`; a table cell above does not broaden a certificate's domain.
For example the Roman substitution is explicitly 1..26, and generated Word fields
do not claim the full unrestricted Unicode key language. Boundary hosts retain
the additional work rather than hiding it in a CommonMark paragraph ratio.

The pipelines have separate acceptance rules. The grammar checker recognizes
source bytes against the declared languages and verifies the inverse laws. The
benchmark records measured instructions, input receipts and artifact provenance.
Parity and regression test native parsing results. A native node census, output
field comparison or fixture hash is not a benchmark admission criterion.

The whole document cost still includes native allocation, construction and
post-processing. Grammar equivalence alone does not establish equality of those
implementations' output work, constant factors, or global optimal instruction
counts. That is why reports retain A/B, B/R, A/R, byte counts and residual host
costs instead of calling every quotient removable parser overhead.

After reviewing a grammar or specification change, regenerate both ledgers:

```sh
node --input-type=module <<'JS'
import fs from 'node:fs';
import {buildGrammarCorpus, grammarCatalog} from './scripts/benchmark/corpus.mjs';
import {featureCoverage} from './scripts/benchmark/coverage.mjs';
const corpus = buildGrammarCorpus({units:2});
for (const [name, value] of [
  ['grammar-corpus', grammarCatalog()],
  ['grammar-coverage', featureCoverage(process.cwd(), corpus)]
]) fs.writeFileSync(`packages/markdown-core/benchmarks/${name}.json`, JSON.stringify(value, null, 4) + '\n');
JS
pnpm exec prettier --write packages/markdown-core/benchmarks/grammar-{corpus,coverage}.json
node --test scripts/benchmark/tests/corpus.test.mjs
```

Regenerating a digest does not discharge a changed rule's proof obligation. Review
the actual grammar, source examples and transformation proof together. CI's snapshot
comparison makes the changed evidence visible rather than inferring mathematical
coverage from fixture counts or output node counts.
