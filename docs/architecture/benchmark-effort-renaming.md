# Whole-parser alphabet-renaming obstructions

The [optimal-effort theorem](benchmark-parser-effort.md) admits a full grammar
conjugacy as one possible proof route. For the six current delimiter proof
domains, **no global byte permutation carrying their marker to `*` can preserve
ordered owner topology on the whole parser domain**. This includes permutations
that change every other byte, not just the obvious two-character swap.

This is a negative theorem about that class of source/output maps. It does not
prove unequal optimal effort, prohibit context-sensitive reductions, or refute
the existing restricted structural proofs. All 43 full-parser optimum statuses
remain `unproved`. The seven identical local contracts remain valid.

## Quantifiers and observation

Let A be Core's complete default parser and B either Core's complete default
parser or the pinned cmark parser. Inputs are finite byte strings, including the
native parsers' handling of NUL, invalid UTF-8, malformed delimiters and EOF.
Choose sufficient resources for these short documents. A full-contract
conjugacy must, in particular, preserve successful outputs in this setting;
adding failure, source-coordinate and field obligations cannot repair a failed
necessary condition.

For a byte permutation `pi`, let `Tpi` apply it independently at every position.
Consider any output bijection F that preserves every owner and the ordered
parent/child relation. Tags and fields may be renamed arbitrarily for this
necessary test. Write `shape` for the preorder sequence of child counts, including
the document root. It uniquely determines an ordered rooted tree. Consequently,

```text
F(A(s)) = B(Tpi(s)) for every s
    implies shape(A(s)) = shape(B(Tpi(s))) for every s.
```

These witnesses have ordinary tree ownership only: no owned inline field groups,
bindings or hidden roots are projected away. The checker neither flattens the
tree nor coalesces/drops Text nodes. It does not use node counts as a substitute
for parentage. Equal projected shapes would not establish semantic equivalence;
unequal shapes suffice to disprove this topology-preserving map.

The proposition also applies to a smaller correctness domain if the proposed
bijection is onto a domain containing the selected target document. Its inverse
must then be one of the checked inputs. A fixed-template domain excluding those
targets is not covered, and cannot be substituted for the full native parser
domain.

## Finite inverse-image theorem

Fix a source marker byte m, target byte `*`, and positive integer k. Choose the
target document `y = '*'^k + 'x' + '*'^k` (with EOF immediately after it).
For **every** byte permutation with `pi(m) = '*'`, set `b = pi^-1('x')`. Then

```text
Tpi^-1(y) = m^k + b + m^k.
```

Only b depends on the remaining permutation. Therefore, if none of the 256
source documents `m^k + b + m^k` has `shape(B(y))`, no such permutation and
ordered-owner bijection F can satisfy the whole-domain commuting equation.
Proof: choose `s = Tpi^-1(y)` in that equation; its necessary shape equality
contradicts the finite premise. There are `255!` permutations with the specified
marker image; 256 parses cover every possible inverse of `x`. The extra case
`b = m` is impossible for a bijection because `x != '*'`, but checking a superset
does not weaken the conclusion. No premise fixes the image of space, newline,
backslash, ASCII letters or non-ASCII bytes.

This is an exhaustive finite proof obligation following a universal reduction,
not a claim that fuzzing a finite number of arbitrary documents proves a grammar.
It also rules out the byte-renaming route before considering machine-instruction
conjugacy: a cost-preserving compiler cannot repair an incorrect semantic map.

## Four certificates, six structural proof domains

Square brackets below contain preorder arities, not node-kind identifiers.

| Certificate | Constraint | k / target | Core inverse-image shapes (number of b values) | Target shape in both engines | Structural proofs affected |
| --- | --- | --- | --- | --- | --- |
| `plus-to-star-v1` | `pi('+') = '*'` | 1 / `*x*` | `[1,1,0]` (252); `[1,1,1,1,0]` (2); `[1,2,0,0]` (2) | `[1,1,1,0]` | `insertion-strong-v1`, `run-insertion-v2` |
| `equals-to-star-v1` | `pi('=') = '*'` | 1 / `*x*` | `[1,1,0]` (256) | `[1,1,1,0]` | `run-mark-v2` |
| `tilde-to-star-v1` | `pi('~') = '*'` | 3 / `***x***` | `[1,0]` (256) | `[1,1,1,1,0]` | `run-strike-v2`, `run-sub-v2` |
| `caret-to-star-v1` | `pi('^') = '*'` | 2 / `**x**` | `[1,3,0,0,0]` (254); `[1,2,1,0,0]` (1); `[1,2,0,0]` (1) | `[1,1,1,0]` | `run-super-v2` |

The finite classifications have grammatical explanations:

- [Insertion](../specs/dialect/insertion.md) requires two signs; `+b+` normally
  remains paragraph text. With b equal to TAB or SPACE, the signs make nested
  empty bullet items. With LF or CR, they make two sibling empty items. Those
  siblings give four nodes, just like `*x*`, but a different owner tree.
- [Mark](../specs/dialect/marks.md) requires two signs; `=b=` has no matching
  pair, preceding paragraph for a Setext underline, or other complete inline
  construct, and remains paragraph text, including the two physical-line cases.
- Three initial tildes start a [code fence](../specs/dialect/code.md).
  `~~~b~~~` is always one CodeBlock. LF/CR can put the final run on a closing
  line; other bytes remain on the opening line. Either way the tree has only
  Document and CodeBlock. The triple-star target instead has nested inline
  emphasis/strong owners under a paragraph.
- [Superscript](../specs/dialect/superscript-and-subscript.md) admits empty `^^`
  owners. `^^b^^` normally has two empty scripts around a Text or SoftBreak.
  The `[` case invokes the inline-footnote attempt and ends with a script
  containing Text followed by another Text. Backslash escapes the next caret,
  leaving an empty script followed by Text. Neither special case has the
  target's one-child chain. The backslash case again has the same node count
  as the target, so a census would miss the obstruction.

The targets follow [CommonMark 0.31.2 emphasis rules](https://spec.commonmark.org/0.31.2/#emphasis-and-strong-emphasis).
The cmark pin is owned by `scripts/init-environment.sh`. The finite source facts
were first checked at Core `c79b5610b575d698a7daa0ac7c46a365c73b88cf`; the native
audit checks the current source build on every applicable CI run. For the
implementation-specific recovery cases, the theorem is conditional on these
explicit finite facts; it is not an assertion that testing this implementation
proves every implementation of a prose specification correct.

## Executable obligation and reporting

`scripts/lib/effort-renaming.mjs` owns the four classifications and the common
inverse-image checker. `pnpm audit:corpus-pairs` runs it through the same current
Core binary and pinned clean cmark checkout as the structural audit. It sends
raw Buffers to stdin, so `00`, `80` and `ff` remain distinct single-byte inputs.
It checks 1,024 source documents and eight target executions, and prints the
certificate, target and complete shape histogram. Any changed target, source
classification, missing output, parser error or topology collision fails the
audit. Reassess the certificate if the grammar changes; do not silently update
the expected shapes from fresh output.

Unit tests cover nontrivial byte permutations, exhaustive byte transport,
owner-order sensitivity, late-byte collisions and parser failure. They test
the checker; the native audit supplies the grammar observations. The existing
external-parity CI job runs the native obligation. The proof text and checker
enter pairing identity, and the proof text also enters local-report identity
because those reports carry the full-parser adjudications.

Each affected effort record has two independent conclusions:

```text
status: unproved                         // full-parser optimum
alphabetRenaming.status: refuted         // only the declared map class
alphabetRenaming.scope:
  whole-domain-byte-permutation-with-ordered-owner-bijection
```

The other 37 structural proofs have no alphabet-renaming adjudication. An old
measurement can be interpreted under these certificates only with its original
measurement identity retained and the new interpretation identified separately.
This work introduces no parser speedup and no new Callgrind measurement.

## What remains open

The result closes the simplest proposed whole-parser proof route for these six
domains, even though their successful templates remain structurally isomorphic.
It does **not** close the optimal-effort question:

1. A context-sensitive source/output mapping might exist. It must specify the
   complete contract, reversible treatment of malformed/contextual inputs, and
   bidirectional program transformations. The recognition, dispatch and runtime
   conversions introduced by those transformations count as work.
2. A transformation requiring extra work establishes bounds with that overhead,
   not exact equality. Equal-width templates do not make the transform free.
3. Matching universal lower bounds and attainable upper bounds are another
   route. One selected implementation's trace is neither bound on all admissible
   programs. No such matching bounds have yet been established here.
4. Boundary certificates can still compare identical local problems. Extending
   them to a full parse requires proving recognition/preparation, output fields,
   coordinates, state sharing and lifecycle residuals as well. Summing local
   ratios does not discharge those obligations.

The next positive full-parser certificate must meet one of those obligations;
neither excluding these witnesses from the measured parser's correctness domain
nor replacing the parser by a template decoder does so.
