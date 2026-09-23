# A corpus certified by grammar equivalence

The deliverable is the corpus, its generative grammars, and the proofs below.
[The checked-in corpus catalog](../../packages/markdown-core/benchmarks/grammar-corpus.json)
contains all 55 certificates, the two concrete grammars, their normal forms,
and two complete examples per family. `scripts/lib/grammar-corpus.mjs` generates
arbitrarily chosen values in those grammars; it does not derive a grammar from a
native AST. The default benchmark emits 300 documents: 35 whole grammar pairs
and 20 boundary pairs with both original hosts, at two scales. These cover all
30 historical scenarios and all 43 former structural pairing domains. Eight additional boundary families
retain historical affix/dimension/reset/caption/headless/sparse/definition work
that would otherwise disappear when only the shared restricted domain was kept.

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
alternatives deliberately excluded by these grammars. For example, all words
here are lowercase ASCII, so a URL field has no escape/normalization choice.

These certificates establish the grammar equivalence requested of the benchmark.
They do not assert equal instruction counts, equal byte lengths for all frames,
or an optimality theorem for the native implementations. Those are different
claims. Reports preserve both byte counts and the actual native measurements;
a fixed frame rewrite must not be described as free at runtime.

## Common grammar and unique recognition (T1)

`SP` is one ASCII space and `LF` one newline. The definitions are:

```ebnf
Word   = [a-z]+ ;
Phrase = Word (SP Word)* ;
Empty  = epsilon ;
Body   = Word | Word SP (Atom SP)* Word ;
Atom   = Word | OPEN Body CLOSE ;
```

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

The following 24 pairs are complete concrete grammar pairs, not boundaries.
Their exact productions (including all whitespace and punctuation) are in the
catalog's `grammars` entries and in `productGrammar`. In the table `B=Body`,
`P=Phrase`, `W=Word`, and `E=Empty`.

| Certificate | Normal-form Unit fields | Concrete frame substitution |
| --- | --- | --- |
| leaf-comment | literal:P | comment fence / code fence |
| leaf-formula | literal:P | formula fence / code fence |
| leaf-fence | literal:P | formula info fence / plain code fence |
| leaf-promotion | literal:P | standalone formula / code fence |
| record-span | body:B, key:W, value:W | attribute envelope / link destination and title |
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
Inline administrative productions, apply the fixed frame encoding, and apply
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
consumption. It compares both decoded Documents, separately from native-parser
execution. Native parsing is an additional conformance witness that the emitted
frames activate the intended constructs; native output never defines the proof.

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

## Boundary corpus and its limits (T5)

T1–T4 do not certify the following additional host operations. These are
**unmatched obligations of the supplied full-host counterparts**, not a claim
that no imaginable equivalent counterpart can ever be constructed. The local
pair is positively proved by T5. No full-host ratio receives a grammar certificate.

| Certificate | Why the proposed whole-host normalization fails | Matched Unit fields |
| --- | --- | --- |
| alpha-list | letter-to-integer conversion is not the shared decimal lexical grammar | body:B, tail:B |
| upper-list | case/enclosed letter conversion is not decimal recognition | body:B, tail:B |
| roman-list | subtractive numeral recognition is not digit recognition | body:B, tail:B |
| upper-roman-list | Roman recognition plus marker framing is not decimal recognition | body:B, tail:B |
| default-list | default marker has no independently encoded start integer | body:B, tail:B |
| enclosed-default-list | enclosed default marker likewise loses the target's start field | body:B, tail:B |
| grid-cell | width/max/padding constraints remain on only the grid side | body:B, tail:B |
| simple-matrix | computed column widths and alignment remain on only the simple side | anchor:W, key:W, target:W, value:W |
| specimen-graph | binding/ordinal/hoisting rules are not supplied by independent-field products | body:B, key:W |
| metadataempty | unknown-member acceptance/retention differs from literal code | body:B, key:W, value:W |
| metadata | first-valid typed-member retention differs from literal code retention | body:B, value:W |
| callout | three collapse states map to the same reference frame | body:B |
| citation-affixes | affix attachment/group recognition is absent from a single link label grammar | key:W, literal:P, value:W |
| embed-dimensions | width/height disappear from the CommonMark image counterpart | literal:P, target:W |
| specimen-reset | explicit ordinal reset has no footnote counterpart | body:B, key:W |
| trailing-caption | table-caption attachment differs from a following paragraph | key:W, literal:P, value:W |
| headless-matrix | a headerless matrix differs from a required pipe-table header | anchor:W, key:W, target:W, value:W |
| sparse-grid | spans, sparse/empty rows, footer and empty-caption grammar remain residual | anchor:W, footer:W, key:W, last:W, target:W, value:W |
| leading-caption | leading-caption attachment differs from an independent paragraph | key:W, literal:P, value:W |
| mixed-definitions | multiple/loose/empty definition recognition remains residual | anchor:W, body:B, empty:E, key:W, literal:P, tail:B, target:W |


For example, varying a default list's start changes the decimal counterpart but
not the default marker. Varying the collapse state likewise leaves its old quote
counterpart unchanged. Both give a direct counterexample to an invertible full
field translation. A Roman string and its numeric value require a separate
numeral grammar/conversion proof; deleting that work is not an administrative
production rewrite. A table's width equation refers to multiple fields and
cannot be discarded when normalizing to their independent product. These
specific obligations explain each split; AST-owner differences do not.

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

## Concrete coverage, variation, and checks

The catalog records each certificate's historical scenario IDs. The registry
requires exactly the union of all 30 old scenarios and dispositions for every
old structural domain, including the four previously unpaired families. No
historical witness is relabelled a proof by changing its title. Old structural
fixtures remain separate diagnostic controls.

The benchmark uses 12 and 24 generated derivation units. Word widths, independent
keys/values/targets/anchors, numbers of atoms, balanced branching and chain depth
vary deterministically. Chains include depth 32 at scale two. `instantiateGrammar`
also accepts independently chosen field values: the grammar is not restricted
to the generator's finite schedule or correlated counters. Two checked-in
examples per family provide immediately reviewable source documents; the full
run materializes every `.md`, its digest, grammars, derivations, partitioned
hosts and residuals in `corpus/grammar-corpus.json`.

Run:

```sh
pnpm benchmark:grammar --corpus-only --out build/benchmark-grammar
pnpm audit:corpus-pairs
# Linux with the pinned compiler, oracles and Callgrind:
pnpm benchmark:grammar --scale 2 --out build/benchmark-grammar
```

The native audit runs Core/cmark/cmark-gfm on the actual emitted documents.
It checks recursive meanings, literal fields, fixed task/formula states and
boundary field counts against grammar-derived expectations. It also verifies
that historical residual hosts really retain their affixes, dimensions, reset
ordinals, captions, headless sections, row/column spans, footer, and empty/loose
definitions. Merely recognizing a generic Table or mapping a scenario ID to a
shared subset cannot pass that check. Tests separately
check inverse laws, rejected syntax, independence, lossless partitions, complete
coverage and reporting boundaries. A native audit is finite evidence that our
encodings match the implementations; it is not a substitute for T1–T5.

The artifact identity covers the grammar/generator/checker sources, entrypoint,
oracle pins and this proof document, plus the exact generated documents and
proof records. The Callgrind report lists the concrete ratio per certificate
and scale, retaining full hosts for boundary rows without certifying those host
ratios. CI measures and archives this suite alongside the older diagnostic suite.

To regenerate the checked-in example catalog after changing a grammar, run the
following and review its grammar and example diff together with the proof:

```sh
node --input-type=module -e 'import fs from "node:fs"; import {grammarCatalog} from "./scripts/lib/grammar-corpus.mjs"; fs.writeFileSync("packages/markdown-core/benchmarks/grammar-corpus.json", JSON.stringify(grammarCatalog(), null, 4) + "\n");'
pnpm exec prettier --write packages/markdown-core/benchmarks/grammar-corpus.json
```
