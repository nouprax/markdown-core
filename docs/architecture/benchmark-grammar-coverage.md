# Grammar benchmark feature acceptance

The benchmark's deliverable is a generated corpus with mathematical grammar
certificates. [T1–T7](benchmark-grammar-corpus.md) establish equivalence on each
declared language. [The machine ledger](../../packages/markdown-core/benchmarks/grammar-coverage.json)
binds coverage to every syntax specification, its sections, the registered C
elements and complete correctness-fixture inputs. Native AST similarity is
never the proof.

“All features” has the following executable acceptance conditions:

1. Every page in `docs/specs/dialect/` has generated, certified benchmark inputs.
   Every registered element has exactly one specification owner. No feature is
   admitted merely because a node of its kind appeared incidentally elsewhere.
2. Shared CommonMark/GFM syntax uses identical source and the identity proof.
   An extension counterpart retains all variable fields of its declared grammar,
   with both inverse laws, complete consumption and native conformance checks.
3. A boundary retains its complete host and an exhaustive byte partition, names
   the unmatched reference productions, and supplies a positive proof for the
   complete local field grammar. A local ratio never certifies a host ratio.
4. The ledger binds every specification section and all source fixtures for that
   feature by digest. A new specification, element, section, changed rule or
   changed fixture fails CI until its disposition is reviewed. The fixture
   registry includes rejection, limits and composition cases; those tests remain
   correctness evidence and are not relabelled as mathematical proofs or as
   separately measured benchmark documents.
5. The generated source, grammar, normal form, proof identity, native checks and
   Callgrind reports travel together. Filtering a benchmark selects both sides
   and, for boundaries, both complete hosts. Missing halves fail reporting.

There are **30 specification features, 136 sections, 32 registered elements,
185 certificates and 788 generated documents** at the default two scales.
Of these, **173 certify whole declared languages and 12 certify local boundary
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
| Highlights | Paired runs and rejected openers. |
| Strikethrough | Same-input GFM pairs, cross-syntax pairs and rejected openers. |
| Super/subscript | Both markers, formatted content, empty superscript, escaped spaces and raw-space rejection. |
| Comments | Inline/block percent forms, empty comments, inline/block HTML comments and fallback. |
| Formulas | Dollar, backtick-dollar, doubled-backslash parentheses/brackets, blocks, formula fences, promotion and fallback. |
| Bracketed spans | Empty, nested, record/class forms and rejected suffixes. |
| Links/images | Direct links/images, all reference forms, duplicate and unresolved references, angle/bare autolinks; numeric dimensions have an explicit boundary. |
| Cross links/embeds | Absent/empty/raw labels, path/local/heading/block targets, embed forms and fallback; numeric dimensions remain a boundary. |
| Anchors | Explicit IDs, last/empty ID, Unicode/duplicate/empty automatic bases and implicit references. |
| Block IDs | Paragraph, list-item and standalone container declarations. |
| Footnotes | GFM references, nested inline notes, cycles, duplicate/unused definitions, repeated calls and fallback. The GFM retention difference is checked and reported. |
| Citations | Author/suppressed/normal modes, groups, independent prefix/key/suffix and rejected keys. |
| Specimens | Definition/call equality, explicit nine-digit resets, anonymous/duplicate definitions and group reset suppression have constructive whole-language counterparts. |
| Lists | Three bullets, decimal delimiters, tight/loose/nested/empty items; alpha/Roman canonical finite domains and default markers have constructive proofs. |
| Tasks | Standard states, custom/Unicode markers and heading bodies. |
| Definition lists | Tight/loose, multiple/empty definitions and multiple blocks. |
| Quotes/callouts | Same-input plain/nested/lazy quotes; variable type and all three collapse states have invertible counterparts. |
| Tables | Same-input pipe/alignment/ragged/escaped-pipe forms, both captions; simple, headless, multiline, grid, spans, sparse rows and footer use geometry boundaries. |
| Directives | Inline label/attribute/both, Unicode names, leaf forms, named/nameless/unbraced/nested containers and fallback. |
| Properties | All ten fields, text/number/bool/null/list distinctions, BOM, invalid-before-valid/duplicates, unknown fields and literal indentation in complete boundary hosts. |
| Attributes | Quoted/unquoted/empty/bare values, ordered duplicates, escapes/entities/newline, IDs/classes and every documented owner category. |
| Precedence | Nested block owners, literal islands, bracket/suffix and rejected-prefix cases; the complete conflicts fixtures remain bound to this review. |

The precise restricted productions, including all punctuation, are in
`grammar-corpus.json`; a table cell above does not broaden a certificate's domain.
For example the Roman substitution is explicitly 1..26, and generated Word fields
do not claim the full unrestricted Unicode key language. Boundary hosts retain
the additional work rather than hiding it in a CommonMark paragraph ratio.

The whole document cost still includes native allocation, construction and
post-processing. Grammar equivalence alone does not establish equality of those
implementations' output work, constant factors, or global optimal instruction
counts. That is why reports retain A/B, B/R, A/R, byte counts and residual host
costs instead of calling every quotient removable parser overhead.

After reviewing a grammar or specification change, regenerate both ledgers:

```sh
node --input-type=module <<'JS'
import fs from 'node:fs';
import {buildGrammarCorpus, grammarCatalog} from './scripts/lib/grammar-corpus.mjs';
import {featureCoverage} from './scripts/lib/grammar-coverage.mjs';
const corpus = buildGrammarCorpus({units:2, scale:1});
for (const [name, value] of [
  ['grammar-corpus', grammarCatalog()],
  ['grammar-coverage', featureCoverage(process.cwd(), corpus)]
]) fs.writeFileSync(`packages/markdown-core/benchmarks/${name}.json`, JSON.stringify(value, null, 4) + '\n');
JS
pnpm exec prettier --write packages/markdown-core/benchmarks/grammar-{corpus,coverage}.json
pnpm audit:corpus-pairs
```

Regenerating a digest does not discharge a changed rule's proof obligation. Review
the actual grammar, source examples and native assertions together. CI's snapshot
comparison makes the changed evidence visible rather than inferring mathematical
coverage from fixture counts or output node counts.
