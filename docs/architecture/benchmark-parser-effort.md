# Parser effort and the limits of structural pairs

The existing 43 proofs establish semantic ownership/field correspondences on
restricted input languages. **None establishes equal optimal parse effort for
the complete parser contracts.** They are structural controls, not an
equal-effort baseline. Their measured A/B, B/R and A/R remain descriptive
quotients. The former `sameJob` interpretation and equivalent-work median are
withdrawn. The 30 historical workloads and 12 boundary interventions do not
acquire stronger status through those proofs.

## The problem that must be specified

A parse problem is a tuple `(D, O, E, M)`:

- `D` is the complete correctness domain, including malformed inputs, enclosing
  contexts, feature settings and the validity information supplied to the parser.
- `O` specifies ordered owners, fields, literal bytes, normalization, binding
  edges and source coordinates. Default values and metadata cannot disappear
  merely because the structural projection does not print them.
- `E` specifies recognition failure, resource failure and observable lifecycle
  behavior, including which input/storage may be borrowed and for how long.
- `M` is the machine and cost model below, including allowed operations and
  initial/final storage. Output buffers, indexes or normalized text cannot be
  supplied for free to one side.

A parser in the admissible class must meet this contract for every input in D,
not only for the measured successful examples. Promising that an input consists
of fixed templates is a different problem from parsing arbitrary Markdown.
Proving a property of a restricted recognizer cannot silently confer that
promise on a measured general parser. A source substitution is not free merely
because the benchmark generator performed it before measurement.

## Cost model: parser-effort-v1

Use a deterministic byte-addressed RAM with a fixed word width, finite control,
explicit input/output extents and ordinary mutable memory. Record a work vector:

| Dimension | Charged operation |
| --- | --- |
| Input | Input bytes inspected, including repeated reads and lookahead |
| Control | Primitive tests, branches and integer/index operations |
| Memory | Bytes read/written in working storage, including stack/map/index operations |
| Storage | Allocation/release operations and requested byte extents |
| Output | Output records/edges and field/literal/coordinate bytes written |

Every composite operation expands into these primitives. Unicode decoding,
substring recognition, hashing, normalization, searching and copying are not
unit-cost oracle calls. Nor is a dynamic source or field transformation free.
An output owner may use a different static tag under an explicit bijection, but
creating or deleting a field, owner or byte sequence must be charged. A chosen
layout is part of an algorithm, not a theorem about the problem.

For any fixed positive weight vector w, let `Cw(P,x)` be the weighted sum of
these operations for admissible parser P. The optimum is
`Optw(Q,x) = inf { Cw(P,x) : P meets Q on its entire correctness domain }`.
The same argument can be applied to worst-case cost at a declared input size.
Equal big-O complexity, equal node count, or one pair of equal measured costs
does not establish equality of these optima. Callgrind Ir is a separate machine
measurement; it is not an evaluation of this abstract vector or a measured
distance from the optimum.

## Sufficient theorem for equal optimal effort

Specify source/output bijections T/F and **two compiler transformations** U/V
between the admissible parser classes. They must preserve correctness and
failure behavior over the complete domains and must satisfy, for every
admissible P and Q and every corresponding input:

```text
work(U(P), T(x)) = work(P, x)
work(V(Q), x)    = work(Q, T(x))
```

Both statements include setup, recognition, construction, coordinate tracking
and the declared lifecycle. These transformations are mathematical mappings
between programs; a runtime adapter they introduce is part of the charged work.
Taking infima gives each inequality and therefore
`Optw(QA,x) = Optw(QB,T(x))` for every w. A single direction gives only a bound.
An adapter with additional linear work gives a bound with that overhead, not
equality. This is a sufficient route, not a necessary condition. A certificate
may instead prove matching lower bounds for every admissible parser and provide
algorithms attaining them on the paired domain. Those algorithms must remain
correct outside the measured domain; dispatch and fallback selection are charged.
Different complete grammars therefore do not by themselves prove unequal optima
on a particular input family.

A bisimulation between two selected parsers proves their traces have
equal cost; without a result about the admissible classes it does not prove
equality of the optimal costs.

One possible route is a literal alphabet/constructor renaming under which the
entire machine instruction set and parser contract are conjugate without added
operations. This is a conditional theorem, not a certificate for `++`/`**`.
Character classes, arithmetic on byte values, other productions using those
characters, invalid inputs, context and source coordinates must all map too.
The full Markdown grammars currently measured have no such established map.

## Re-audit of the current corpus

`scripts/lib/pair-effort.mjs` records an explicit adjudication for each of the
43 structural proofs. Registry validation rejects missing and stale entries.
Every current status is **unproved**, not "proved unequal" or "unpairable".
Adding a manifest boolean, equal trees/bytes or a successful trace cannot admit
an equal-effort result. A new certificate requires a valid optimum theorem
(algorithm-class reductions or matching lower/upper bounds) and its checked
domain/cost machinery; there is deliberately no self-certifying flag.

The audit distinguishes these recurring gaps:

| Family | What the structural proof omits from an effort theorem |
| --- | --- |
| Recursive insertion and fixed span runs | Full-context flanking, residues, rejection and coordinate obligations; the fixed-run `***` terminator also conflicts with a global alphabet swap |
| Opaque and promoted leaves | Closer rules, padding/LF normalization, line/fence decisions and phase transitions |
| Task markers and nondecimal lists | Complete marker languages, value conversion, field representations and continuation |
| Attribute/cross-link/citation fields | Different validation, delimiter, normalization and derived-value work |
| Directives and definition owners | Image/link activation or caption/list/quote precedence versus directive/definition recognition |
| Fenced containers | Close/name/attribute decisions versus quote-prefix continuation |
| Grid and simple/pipe tables | Geometric/topological work versus prefix or delimiter recognition |
| Named binding graphs | Full symbol lookup/resolution, ordinals, failure and source-coordinate contracts |
| Identical decimal-list source | A/B is an identity control; it proves no equivalence of Core's and cmark's full contracts |

Most production domains vary only six decimal digits and repeat a fixed unit.
They prove more than agreement on one example, but cover much less than a whole
feature grammar. Hard-coding the template into a theoretical decoder would
change the correctness domain and does not repair that gap. The recursive
insertion language is broader, but its tree induction still is not a theorem
about parser cost. Zero current equal-effort certificates is an honest audit
result, not permission to weaken the admission criteria until some rows pass.

## Measurement policy

Publish three clearly distinguished forms of evidence:

1. **Same-input before/after:** identical bytes, harness, parser contract,
   compiler/runtime and references. This remains the primary test of a proposed
   optimization. It establishes measured savings, not theoretical optimality.
2. **Structural controls:** retain the existing semantic proofs and A/B/B/R/A/R
   data with the per-pair effort gap. Do not aggregate them into an equal-effort
   median, name A/R `sameJob`, or interpret A/R minus one as removable overhead.
3. **Boundary interventions:** retain Full/Without and signed deltas, including
   grammar/construction interactions. They are neither additive feature prices
   nor lower bounds on unavoidable effort.

The full-parser equal-effort cohort is empty until a certificate meets the theorem.
The [local boundary suite](benchmark-effort-boundaries.md) supplies seven separate
identical-contract certificates and measured production implementations. Their
entry promises, adapters, preparation, residuals and failure domain are explicit;
they do not upgrade any of the 43 full-parser claims.
Do not label a restricted-domain toy decoder as a full-parser certificate.
Benchmark prioritization can use high absolute costs, but a repair must identify
redundant work or a better general data structure and then pass semantic/failure
tests and controlled before/after measurements.

The model, adjudication records and structural checkers participate in pairing
identity. Reinterpreting an old artifact must retain its original measurement
identity and separately identify the new interpretation; it is not a new
Callgrind run or a parser speedup.
