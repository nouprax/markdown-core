# A corpus certified by grammar equivalence

The deliverable is the corpus, its generative grammars, and the proofs below.
[The checked-in corpus catalog](../../packages/markdown-core/benchmarks/grammar-corpus.json)
contains all 194 certificates, their concrete grammars, normal forms and two
complete examples per family. The run emits 848 documents, each measured once:
in each of [two alphabets](#alphabets), 182 whole declared-language pairs, 12
local boundary pairs with complete hosts, and 12 controls for
[rejection certificates](#rejection-certificates). The [feature acceptance ledger](benchmark-grammar-coverage.md) covers all
30 syntax-guide features, 136 sections and 32 registered elements. Its explicit
section mapping names certificates for 132 source-language sections and gives
reviewed context-only dispositions for four sections. Certificates are registered
directly from their source grammars and specification obligations.

CommonMark/GFM features use the same input on both sides. Extensions use proved
syntax translations or explicit local boundaries. These are grammar proofs;
AST ownership is not an admission criterion.
## What equivalence means here

Different punctuation gives different literal languages: `++a++` and `**a**`
are not the same string. We prove equivalence **under an explicitly specified
syntax translation**, rather than pretend their literal byte languages are equal.
For each certificate, let `G_A`, `G_R` be its displayed concrete grammars, `N`
its normal-form grammar, and `d_A`, `d_R` their decoding maps. The obligation is

```
d_A : L(G_A) <-> L(N)       d_R : L(G_R) <-> L(N)
T = d_R^-1 o d_A           T^-1 = d_A^-1 o d_R
```

Each arrow is a bijection on the **entire declared language**, not merely on
sample inputs. Equality of the two normal forms plus the inverse laws proves
`T(L(G_A)) = L(G_R)`. Translating a derivation preserves every variable field,
its type, multiplicity, order within that field, repetition count, and recursive
structure. A labelled product may permute whole fields; its declaration records
that permutation. Fixed punctuation is part of the concrete encodings, not an
unreported variable. Neither native node ownership nor identical native ASTs
is a hypothesis.

The permitted constructions are typed lexical nonterminals, fixed terminal
encodings, labelled products, repetition, and recursive delimiter substitution.
There is no arbitrary enumeration of strings, parser-output matching, or
instance-specific lookup table. The proof does not identify the *unrestricted*
Markdown Core and CommonMark languages. Their published constructs contain
alternatives deliberately excluded by these grammars. For example, a Word is
spelled with the 26 lowercase ASCII letters or 26 chosen UTF-8 letters, not with
any letter at all. A field whose own grammar admits only ASCII letters is an
AsciiWord, and UnicodeWord is a separate grammar; neither is silently treated as
a Word. Finite marker/state languages and repeated bindings have the separate T7
proof below.

These certificates establish the grammar equivalence requested of the benchmark.
They do not assert equal instruction counts, equal byte lengths for all frames,
or an optimality theorem for the native implementations. Those are different
claims. Reports preserve both byte counts and the actual native measurements;
a fixed frame rewrite must not be described as free at runtime.

A B/R ratio compares two implementations of the same parsing work. Grammar
equivalence alone does not make it one. The reference must also implement every
construct the certificate is built around. An extension certificate meets this
because its extension syntax appears only in A, while B and R parse the same
CommonMark translation. A shared (T6) certificate meets it because both parsers
implement the shared construct. Some checks Core makes on shared input belong to
its own language, for example looking for a block identifier at the end of every
paragraph. They count in B/R. A certificate built around a construct only Core
implements does not meet the premise. It is never reported as a reference
comparison; see [Rejection certificates](#rejection-certificates).

## Alphabets

Every certificate is generated twice, once per alphabet, with the same grammar,
the same fields and the same schedule. ASCII documents spell every Word with
`[a-z]`; they are byte for byte the documents of the single-alphabet corpus
before it, so their measurements stay comparable with earlier reports. UTF-8
documents spell every Word with 26 letters of two, three and four bytes in turn:
Cyrillic `а`–`и`, CJK `一`–`丈` and CJK Extension B `𠀀`–`𠀇`. Each is a Unicode
letter that is neither punctuation nor white space and folds to itself, so every
syntax decision -- flanking, label matching, anchors -- is the same in both
alphabets and only the work done per character differs. A UTF-8 document's name
carries `-utf8` after its certificate's id.

A letter keeps its position: the UTF-8 twin of an ASCII document spells the
same Word with the letter at the same index of its alphabet. So respelling a
UTF-8 document letter by letter gives its ASCII twin exactly, except in a field
bounded in bytes, which holds fewer UTF-8 letters at its limit; the tests check
this for every document. Grid geometry counts characters, not bytes.

Some positions admit only ASCII letters in their own grammar: a block
identifier (`#id#`), a callout type (`[!type]`) and an email address's local
part. Those fields are AsciiWord and keep `[a-z]` in both alphabets. A
certificate whose every field is ASCII-only or finite has the same document in
both.

The attribute benchmark against lexbor uses the same two alphabets. Its lists
are measured once with ASCII names and values and once with UTF-8 ones; syntax,
digits, spaces and character references are the same bytes in both, and white
space stays ASCII because HTML splits a class run only on ASCII white space.

## Common grammar and unique recognition (T1)

`SP` is one ASCII space and `LF` one newline. The definitions are:

```ebnf
Word   = Letter+ ;
Letter = [a-z] | [а-и] | [一-丈] | [𠀀-𠀇] ;
AsciiWord = [a-z]+ ;
AttributeKey = Word except 'id' and 'class' ;
UnicodeWord = (Letter | 'é' | '字')+ ;
Phrase = Word (SP Word)* ;
Empty  = epsilon ;
Body   = Word | Word SP (Atom SP)* Word ;
Atom   = Word | OPEN Body CLOSE ;
```

A field annotated `source-bytes <= n` restricts that lexical language on **both**
sides. Link reference labels are bounded by 1,000 source bytes. GFM footnote
calls count the authored caret in that limit, so their ASCII key domain is at
most 999 bytes. A specimen or heading key paired with one of these labels takes
the same bound; an unbounded Word would not prove a valid native counterpart.
Repeated occurrences retain the same bound. The decoder rejects the first byte
beyond it, and the generated schedule reaches each bound.
These are syntax limits, distinct from an implementation's allocation failure.

`Body` has a Word at both ends. Its nested atoms have a space before and after
them. `OPEN` and `CLOSE` are the same fixed punctuation token (`**` normally,
`++` in the recursive insertion side). Words contain neither punctuation nor
spaces. Thus maximal Word scanning is unique; a space separates atoms, and a
punctuation token after a space starts a nested atom. Immediately after the
last Word without an intervening space, that token ends the enclosing atom.
The end of the outer Body is supplied by its enclosing production. Induction
on nesting depth establishes a unique derivation and both decode/encode inverse
laws. The one-Word alternative and the multi-atom alternative are disjoint by
presence of a space. Phrase is similarly uniquely split at spaces; Empty has
one derivation.

The independent `recognizeBody` consumes bytes, checks both Word endpoints and
EOF, and reconstructs this derivation. It does not trust the generator's tree.
`renderBody` is its inverse on all finite Body derivations. Finite tests exercise
these laws and reject malformed strings; the induction, not the tests, extends
the result to arbitrary Word length, number of siblings and finite depth.

## Contextual delimiter equivalence (T2)

Nine certificates use:

```ebnf
Document  = Paragraph+ ;
Paragraph = 'probe ' OPEN Payload CLOSE ' end' LF LF ;
```

| Certificate | Payload | A token | R token |
| --- | --- | --- | --- |
| insertion-strong | Body | `++` | `**` |
| run-insertion | Phrase | `++` | `**` |
| run-mark | Phrase | `==` | `**` |
| run-strike | Phrase | `~~` | `**` |
| run-super | Word | `^` | `*` |
| run-sub | Word | `~` | `*` |
| opaque-comment | Phrase | `%%` | two backticks |
| opaque-formula | Phrase | `$` | one backtick |
| opaque-display | Phrase | `$$` | two backticks |

Expand Paragraph, identify corresponding OPEN/CLOSE terminal roles, and apply
T1 to Payload. Replacing a role's token preserves its position and width. For
Body apply that substitution recursively. By induction it has an inverse, and
paragraph concatenation extends the bijection to every Document. This is a
contextual grammar substitution, not a global replacement of `+` everywhere in
Markdown. The grammar excludes adjacent delimiter runs, escapes, punctuation
in words and intraword delimiters. All openers follow a space and precede a
letter; all closers follow a letter and precede a space. Nested delimiters have
the same property. Opaque phrases contain no backticks, newlines or boundary
spaces; code-span newline/padding rules therefore introduce no alternate value.

## Labelled product equivalence (T3)

The original product families below and the additional feature productions in
`scripts/benchmark/features.mjs` are complete concrete grammar pairs over their
declared domains. Identical productions use T6; products with repeated bindings
or finite lexical substitutions use T7.
Their exact productions (including all whitespace and punctuation) are in the
catalog's `grammars` entries and in `productGrammar`. In the table `B=Body`,
`P=Phrase`, `W=Word`, `K=AttributeKey`, and `E=Empty`.

| Certificate | Normal-form Unit fields | Concrete frame substitution |
| --- | --- | --- |
| leaf-comment | literal:P | comment fence / code fence |
| leaf-formula | literal:P | formula fence / code fence |
| leaf-fence | literal:P | formula info fence / plain code fence |
| leaf-promotion | literal:P | standalone formula / code fence |
| record-span | body:B, key:K, value:W | attribute envelope / link destination and title |
| class-span | value:W | class envelope / link destination |
| cross-link | label:P, target:W | cross-link envelope / link destination and title |
| cross-embed | label:P, target:W | embedded cross envelope / image destination and title |
| cross-link-absent | target:W | absent cross label / destination only |
| cross-embed-absent | target:W | absent embedded label / image destination only |
| cross-link-empty | label:E, target:W | empty cross label / empty title |
| cross-embed-empty | label:E, target:W | empty embedded label / empty image title |
| cross-anchor | anchor:W, target:W | cross target and anchor / destination components |
| cross-local | anchor:W | local cross anchor / local destination component |
| cite-author | key:W | author citation / autolink |
| cite-suppress | key:W | suppressed citation / autolink |
| cite-normal | key:W | bracketed citation / autolink |
| inline-directive | body:B, key:W | directive label / link label and destination |
| empty-directive | body:E, key:W | empty directive / empty link label |
| leaf-directive | body:B, key:W | directive block / link paragraph |
| anonymous-container | body:B, tail:B | anonymous fence / quoted paragraphs |
| named-container | body:B, key:W, tail:B | named fence / quoted name and paragraphs |
| loose-definition | body:B, literal:P | definition term and body / loose list paragraphs |
| anchor | body:B, key:W | quote and anchor declaration / quote and reference declaration |

For either side, write a production as
`Unit = c0 F1 c1 ... Fk ck`, where each `ci` is an exact literal and each
`Fi` is a distinct labelled nonterminal. Its normalization is the product of
those labelled nonterminals, ordered by label. The grammar checker verifies
that the two label/type sets are identical and contain no repeated labels.
For the independent-field T3 case, inline administrative productions, apply the fixed frame encoding, and apply
the recorded field permutation. The result on both sides is literally the
same labelled product grammar. `Document = Unit+` on both sides.

Here is why normalization is invertible, rather than arbitrary erasure of
punctuation. A Word ends at a nonletter; Phrase contains only letters and single
spaces; Body contains only letters, spaces and balanced `**` tokens. Every
field in the catalog is terminated by punctuation outside that field's
alphabet or by LF. There are no adjacent undelimited fields. Empty fields are
statically declared. Consequently each frame has a unique field segmentation;
T1 supplies unique decoding inside each field. The initial literal or initial
field followed by its unique terminator fixes the unit start; the exact final
literal fixes its end. Repeating this argument gives unique Document splitting,
including frames with internal blank lines. Encoding inserts the same literals
and field positions, recovering every byte. Hence `encode(decode(x))=x`, and
independence of the labelled field domains gives the other inverse law.

This also proves the translations with a different field order, such as
`::key[body]` versus `[body](/key)`: decoding gives the same **labelled** product,
without equating a parser's block and inline ownership. Cases with an absent
label and those with an empty label have separate certificates and normal
forms; absence is never silently turned into an empty variable.

The standalone source recognizer compiles these literal/nonterminal productions,
then uses T1 recognizers on every captured field and requires complete input
consumption. It compares both decoded Documents without executing a native parser.
Native output correctness belongs to the parity and regression suites.

## List frame equivalence (T4)

`task-value` and `decimal-list` use a common Body grammar and:

```ebnf
Document = Item (LF* Item)* LF+ ;
Item     = Prefix Body LF ;
```

For task-value the Prefix terminals are `- [~] ` and `- [x] ` respectively.
Normalizing the contextual marker yields the same Item production; both task
states are one fixed alternative in their respective grammars. Other task
states are outside this pair. For decimal-list both Prefix productions are
`Decimal '. '`, with `Decimal = [1-9][0-9]{0,8}`; translation is identity.
The decimal spelling is retained as a string, including all its digits.
Bodies contain no LF, so line/item boundaries and all blank lines are unique.
T1 and induction on item count give the two inverse laws. The recognizer must
retain blank-line multiplicities: ignoring them would prove only a quotient
of this grammar, not its language bijection.

## Identity grammar (T6)

For shared syntax `G_A = G_R` literally: every production, lexical nonterminal,
context guard and terminal is the same. Set `T = id`. Then both inverse laws and
`T(L(G_A)) = L(G_R)` follow immediately, for all derivations of that grammar.
No alternate heading, HTML, hard-break, entity or escape spelling is necessary.
The catalog marks these certificates `identity: true`, the generator emits equal
bytes, and the checker compares the productions and independently decodes both.

Native output differences affect performance interpretation without changing
source grammar identity. A quote's node name, automatic heading anchors, source
positions and emitted definition retention are output-model questions. For
example `footnote-retention` uses the identical reference-definition grammar,
while Core retains authored definitions and GFM emits used definitions. The
report documents that distinction. Correctness of those outputs is owned by
parity/regression, not by an AST assertion in the grammar corpus.

## Finite lexical substitution and repeated bindings (T7)

A finite lexical grammar is an explicit injective table `e : D -> Tokens`. The
catalog records **every** token, not just the generated examples. All encodings
for one type have the same domain. For Ordinal, `D = {1,...,26}`; the five tables
are decimal, lower/upper ASCII letters, and canonical lower/upper Roman spellings.
For State, `D = {plain,closed,open}`; encodings are the callout suffixes
`epsilon,-,+` and the link-title words `plain,closed,open`. Distinctness of all
entries proves bijectivity onto each finite lexical language. Compose their
inverses with T3's frame substitution. Token length need not be equal.

In the four ordered-list productions, an initial `a)`, `(A)`, `i.` or `I)` fixes
the source variant; the reference begins with `1.`. The second marker is the
variable ordinal. Thus `i` inside an established alpha list does not accidentally
select a fresh Roman list. The two default-marker families now map to fixed `1.`
or `1)` markers: there is no unmatched variable start on only one side. The
canonical Roman domain is explicitly 1..26. Noncanonical numerals and the entire
999,999,999-valued language are **not** proved equivalent to decimal conversion.

For repeated bindings, let the distinct fields be `x_1,...,x_n`, and let a frame
mention a field several times. Its language includes the equality constraint
that all occurrences decode to the same value. Decoding first segments every
occurrence, checks those equalities, and retains the labelled tuple once.
Encoding substitutes that value at every declared occurrence. Therefore each
encoded occurrence is recovered, and the tuple is recovered in the other
direction. The occurrence count may differ between encodings; it is a recorded
fixed part of the grammar transformation, not a deleted variable or a claim of
zero copying cost. This covers reference-label reuse, specimen definition/call
bindings and inherited attributes. Equality constraints are checked from bytes;
an inconsistent second label is rejected instead of overwritten in a map.

T3 and T7 now provide whole declared-language counterparts for thirteen formerly
split families: four numeral lists, two default lists, all three callout states,
citation affixes, both caption placements, mixed definitions, specimen binding, and specimen resets. Prefix/key/suffix and caption/table fields all survive independently.
Named containers use the `:::name` production; `::: name` belongs to the
nameless grammar. The source encoding preserves that normative distinction.

Specimen resets add a shared lexical nonterminal
`Positive9 = [1-9][0-9]{0,8}`. The reset marker `(n@key)` is translated to an
ordered-list start `n.` plus a reference definition/call carrying the same key.
Both spellings recognize the same bounded decimal field. The group certificate
uses the common first-marker rule: the first reset/ordered marker establishes
the start, later authored numbers do not restart the group, blank lines preserve
the group, and an outside paragraph separates it from the next group. Anonymous
and duplicate labels are retained as declared repeated fields, rather than
requiring the reference to use footnotes. The declared group productions retain
the first start (`5`) and subsequent authored reset markers. The variable-reset
family exercises `999999999` as well as small starts. This proves the declared
single-line-body reset grammar, not every unrestricted continuation shape.

## Boundary corpus and its limits (T5)

Twelve families retain local proofs:

| Families | Residual outside the certified local grammar |
| --- | --- |
| grid-cell, simple-matrix, headless-matrix, sparse-grid | Column geometry, interval equality, padding, spans, sparse rows and footer grammar. |
| multiline-matrix, headless-multiline | Physical-line segmentation and logical-row block parsing, including the required second row. |
| metadataempty, metadata, metadata-types, metadata-literal | Initial envelope, typed members, ten fields, first-valid retention, recovery and literal indentation. |
| embed-dimensions, image-dimensions | Bounded positive numeric dimensions and suffix selection. |

The concrete CommonMark/GFM counterparts lack these productions. For example,
cmark's image label has no bounded integer nonterminal, a GFM pipe table has no
span/column-interval predicate, and metadata has no typed member production in CommonMark. Erasing those constraints is not an equivalence
transformation. A table's independently varying cells do not eliminate its width
equations. Metadata scalar/list/null alternatives cannot be replaced by an opaque
code block and called the same member grammar.

These identify exact failures of the supplied reference grammars, **not universal
impossibility theorems about every conceivable encoding or parser**. Their local
positive proofs are complete; their residuals are explicitly outside those
proofs. The feature ledger must not label these hosts whole-grammar certified.

Each host is partitioned as an ordered sequence of residual literal byte ranges
and named typed slots. Extents are contiguous, disjoint and exhaustive; joining
them must recover the exact host. Both sides must yield the same labelled slot
set and values. Repeated occurrences of a specimen key must agree; the equality
check and binding work remain in the residual. That key is decoded once in the
local product, not deceptively charged once for each original occurrence.

Each field is re-embedded with the **same explicit parser entry grammar** on
both sides: Body as a paragraph, Word/Phrase inside a one-backtick code span,
and Empty as `[](/empty)`. The local Unit is the certificate's exact ordered
field sequence, not an arbitrary union of fields. By T1 each embedding is
invertible within its fixed field type. The identity between the two resulting
Unit productions extends by repetition to a Document bijection. This is T5,
a proof of local grammar equivalence over arbitrary fields, with source witnesses
and lossless host residuals. The extra entry frame is measured on both sides.
It is **not** a claim that the native parser exposes that interior operation as
a standalone function, or that host cost equals boundary cost plus a subtraction.

Metadata hosts have one initial envelope containing all generated members,
followed by their bodies. The typed metadata host retains the original large
integer, boolean, null, mixed author list and empty keyword list as explicit
residual bytes, alongside varying date members. Unknown-member hosts vary their
keys independently. Repeating a document-initial envelope in the middle of
a document would not remain a metadata scenario. The corpus explicitly composes
one envelope instead. Its lossless pieces retain every occurrence of every value.

## Rejection certificates

Some shared-grammar certificates are built around a construct that the owning
rule rejects: `probe ^k v^ end` opens a superscript, fails, and falls back to
text. The grammar is shared and T6 holds. Each such certificate declares the
construct it `rejects`. A closed table in `features.mjs` records which pinned
references implement each construct:

| Rejected construct | Implemented by | Certificates |
| --- | --- | --- |
| Ordered-list marker (nine-digit limit) | cmark, cmark-gfm | fallback-list-limit |
| Reference link | cmark, cmark-gfm | common-unresolved-reference |
| Strikethrough | cmark-gfm | fallback-strike |
| Footnote reference | cmark-gfm | fallback-footnote |
| Task-list marker | cmark-gfm | fallback-task-separator |
| Superscript, insertion, mark, inline formula, percent comment, cross link, inline directive, attribute block, grid table, citation, bracketed span, image dimensions, metadata envelope | none | the other thirteen `fallback-*` certificates |

When the certificate's reference implements the construct, B and R both attempt
and reject the same prefix. Such a certificate is an ordinary equivalence.
Strikethrough and footnote references exist only in cmark-gfm, so their
certificates use cmark-gfm as the reference. The generator refuses a certificate
whose reference does not implement its rejected construct.

When no reference implements the construct, B is Core attempting and rejecting
it, and R has nothing to reject. For example, on `39ca971` the AST stage of
`fallback-script` cost Core 73,085 Ir and cmark 19,150 Ir. Replacing only its
`^` bytes by `q` brought Core to 24,924 Ir. About 2K Ir per rejected `^` is work
cmark does not do at all. It is not a slower implementation of shared work.
These certificates therefore get no reference measurement and no B/R.

Instead, each declares a **control**: the same production, with the same fields
and frame, in which the trigger bytes of the rejected construct are replaced by
letters. The generator checks every control:

- the field sequence is the same;
- every fixed terminal keeps its width;
- every change turns ASCII punctuation into a lowercase letter, and at least one
  byte changes;
- each control document decodes, under the same normal form, to the same
  derivation.

Core alone measures the control, `<id>-paired-control`. The report lists B, C,
B/C and (B − C) per unit, which is the work the trigger bytes start in Core. A
trigger byte can also be shared syntax. For example, the second `[` of `[[` is
also a link opener in both parsers. In that case the difference includes that
shared work too.

`fallback-noninitial-metadata` has no byte-neutral control. Its rejected envelope
delimiter `---` is also a thematic break or setext underline in both parsers, so
no letter substitution can remove the envelope attempt by itself. The
certificate records that reason in the same form as a boundary residual, and the
report shows B only.

`block-id-escaped` is not a rejection certificate. In `probe \#k# end` the
marker is not at the end of the paragraph, so the block-identifier rule has no
candidate to reject.

## Concrete coverage, variation, and checks

The registry contains the source-grammar certificates themselves. The specification
ledger requires an explicit disposition for each feature and section; no older
AST pairing registry or scenario manifest participates in admission or generation.

The benchmark requests 12 generated derivation units per family, expanded when
necessary to enumerate every combination of finite grammar fields; the four
ordinal families therefore use 26 units. Word widths, independent
keys/values/targets/anchors, numbers of atoms, balanced branching and chain depth
vary deterministically. `instantiateGrammar`
also accepts independently chosen field values: the grammar is not restricted
to the generator's finite schedule or correlated counters. Two checked-in
examples per family provide immediately reviewable source documents; the full
run materializes every `.md`, its digest, grammars, derivations, partitioned
hosts and residuals in `corpus/grammar-corpus.json`.

Run:

```sh
pnpm benchmark --corpus-only --out build/benchmark-grammar
node --test scripts/benchmark/tests/corpus.test.mjs
# Linux with the pinned compiler, oracles and Callgrind:
pnpm benchmark --out build/benchmark-grammar
```

The source-language checks validate all generated derivations, inverse laws,
rejected source syntax, independent fields, finite substitutions, repeated
bindings, lossless partitions and specification/element coverage. They run
without building or executing Core, cmark or GFM. These checks exercise the
formal constructions; T1–T7 establish the mathematical result beyond the finite
samples.

Pipeline responsibilities are orthogonal: parity/regression own native parsing
correctness; this grammar layer owns source-language equivalence; the benchmark
owns performance measurements and their provenance. It does not assert AST
kinds, per-unit native multiplicity, field values or output equality. Multiple
source units may legally compose into one native block. Correctness fixtures
are neither benchmark acceptance gates nor inputs to the grammar identity.

The artifact identity covers the grammar/generator/checker sources, entrypoint,
oracle pins, both proof/coverage documents, syntax specifications and checked-in
ledgers, plus the exact generated documents and
proof records. The Callgrind report lists one concrete comparison per certificate,
retaining full hosts for boundary rows without certifying those host ratios.
Rejection certificates are listed in a separate table, against their controls.
CI measures and archives only this parse corpus.

To regenerate the checked-in example catalog after changing a grammar, run the
following and review its grammar and example diff together with the proof:

```sh
node --input-type=module -e 'import fs from "node:fs"; import {grammarCatalog} from "./scripts/benchmark/corpus.mjs"; fs.writeFileSync("packages/markdown-core/benchmarks/grammar-corpus.json", JSON.stringify(grammarCatalog(), null, 4) + "\n");'
pnpm exec prettier --write packages/markdown-core/benchmarks/grammar-corpus.json
```
