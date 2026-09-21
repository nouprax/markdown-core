# Benchmark grammar isomorphism

A formal pair compares a declared language of workloads through two grammars.
Its justification comes from those grammars, not the implementations' current
node layout, a favorable measurement, a fixed example or equal node counts.

The contract is narrower than equal application meaning. `Strong` and
`Insertion` render differently but can be corresponding constructors. It is
also stronger than equal amounts of selected work: two trees with the same
number of leaves can have different nesting, ownership, values or bindings.

## Required contract

For a pair declare sublanguages Ld and Lr, a source transformation T and its
inverse, and a bijection F between their abstract derivations. For every x in Ld:

```text
T(x) is in Lr                    inverse(T(x)) = x
abstract(parseR(T(x))) = F(abstract(parseD(x)))
```

The proof must establish both directions. Specify recognition boundaries,
escapes, content interpretation (raw, inline or block), repetition, nesting,
ordering, payload fields, binding/resolution and ownership where present.
State exclusions rather than silently extending a proof to a whole dialect.
Prove base productions and composition; a finite test suite is evidence about
implementations, not a proof over the language.

Abstracting a concrete AST may rename constructors or remove documented
representation-only wrappers. It may not erase arbitrary kinds, reorder
children, drop extra semantic operations or discard fields to force agreement.
Different allocations, storage, helper nodes and passes remain measured costs
of implementing the corresponding abstract work. Byte length need not match;
any source-position correspondence must follow the declared transformation.

There is one `pairs` registry in `benchmarks/corpus.json` (schema 3). A pair
has exactly one contract:

- `proof`: a registered proof with an independent executable domain recognizer
  and a checked structural projection for Core on both inputs and its reference.
- `review`: a closed, registered adjudication of an old workload with reconstructed
  proof domains and, where needed, a source boundary intervention. The original
  workload stays diagnostic and cannot enter a formal median.
- `pending`: the specific missing proof obligation. The workload continues to
  be measured and its existing substitution/count witnesses continue to run.
  Pending does not mean disproved or impossible. Prefer repairing the mapping
  or constructing a better reference input over declaring a bound.

Registering a proof requires reviewing its derivation and executable checker
together. A prose string or a boolean in the manifest cannot certify a pair.
Unknown proofs, missing contracts and legacy registries fail validation.
Changing a proof's domain or structural mapping requires a new proof identifier.

## First proof: insertion-strong-v1

The two spellings have marker `++` and `**`, respectively. In EBNF:

```text
Word     = lowercase ASCII letter, { lowercase ASCII letter } ;
Body     = Word, { " ", (Word | Span) } ;  (* final member must be Word *)
Span     = marker, Body, marker ;
Paragraph = "probe ", Span, { " ", Span }, "\n" ;
Document = Paragraph, { "\n", Paragraph }, [ "\n" ] ;
```

This language permits arbitrary finite nesting and sibling spans, arbitrary
word lengths and multiple paragraphs. It excludes empty bodies, punctuation,
escapes, entities, other Markdown constructs, soft breaks and adjacent marker
runs. These exclusions define the theorem's domain, not parser fast paths.

**Recognition.** Every opening run has length two, is preceded by a space and
followed by a letter. It can open but cannot close. Every closing run follows
a letter and precedes a space or paragraph end. It can close but cannot open.
Nested spans cannot touch their parent's delimiters because every Body begins
and ends in a Word. Consequently the CommonMark rule of three, which requires
a dual-purpose delimiter, never excludes a pair here. Insertion's two-character
consumption and Strong's two-character consumption identify the same runs.
The fixed paragraph prefix precludes list and thematic-break recognition.
Lowercase words cannot introduce any other inline production.

**Mapping and inverse.** Replace each delimiter token `++` with `**`, leaving
words, spaces and paragraph separators unchanged. Neither body language
contains either marker, so the reverse replacement uniquely recovers every
source. Both sides have the same token extents, but equal byte lengths are a
consequence of this particular mapping, not a general pairing requirement.

**Structure.** A Word contributes the same text. A Span wraps the recursively
corresponding ordered body with Insertion on one side and Strong on the other.
By induction on nesting depth these trees correspond. Ordered concatenation
preserves sibling spans and text; paragraph and document composition preserve
the correspondence. The constructor map fixes Document, Paragraph and Text and
maps Insertion to Strong. No fields, bindings or ownership operations differ.
This establishes the equation above for every finite derivation in the domain.

**Implementation checks.** `lib/corpus-pairs.mjs` recognizes this language
independently of either Markdown parser and constructs its expected abstract
tree. The benchmark checks both generated documents at every requested scale
and checks the inverse mapping before measuring. `audit-corpus-pairs.mjs`
compares that expected tree with Core(dialect), Core(common) and cmark(common),
including all children, ordering and literal content. Unmapped kinds or fields
fail closed. Empty default anchor/attribute fields and printer coordinates are
excluded explicitly; this proof does not certify source-position correctness.
Their construction costs remain in the measured stages.

The examples `+++a+++` and `***a***` are outside the domain: insertion leaves
literal plus signs while CommonMark nests Emphasis and Strong. They refute a
whole-language character-replacement claim, not this restricted theorem.

## Measurement meaning

With A = Core(dialect), B = Core(reference-language input), and R = the
reference on that input, all in total stage instructions:

```text
Grammar = A/B        Shape = B/R        Same-job = A/R
```

Only proved-domain pairs enter the formal-pair summary. Candidate pairs retain
their absolute A, B, R and diagnostic quotients in a separate table with the
missing obligation. They do not enter equivalent-work medians or claim grammar
states are formally compared. Unmatched fields on B still suppress A/B and B/R.

The factors share B. Optimizing only B lowers Shape and raises Grammar without
changing A. Grammar includes recognition and construction, not merely scanning.
Always examine absolute costs and the product, and keep distinct references
and measurement identities separate.

## Existing workload disposition

The [pair-by-pair review](benchmark-pair-review.md) closes all 30 former entries:
18 have reconstructed languages and 12 have explicit source boundaries. Forty-two
production domains supplement the recursive insertion proof. The originals and
their substitution/count witnesses remain as diagnostics. Neither old witnesses
nor a restricted replacement certify the whole old workload. Boundary deltas are
whole-document interventions, including interactions, not isolated feature costs.

Reports before schema 4 used the former eligibility rule. Their instruction
counts remain historical measurements; the old 30-pair median must not be
compared with the new proved-domain median as a performance change. The new
insertion pair also changes the corpus digest. Remeasure both parser revisions
on the same corpus and toolchain for a performance comparison.
Schema 4 also records a pairing-identity digest over the registry, proof document
and executable contract checkers, review/boundary definitions and XML adapter. This identity must match when comparing
summaries: a changed proof status or mapping can change their membership even
when every measured document's bytes are unchanged.
