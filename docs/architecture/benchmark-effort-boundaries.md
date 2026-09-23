# Equal optimal effort at explicit parser boundaries

This is a **local** certificate suite. It does not establish equal full-parser
optima for any of the 43 structural pairs. Nor does failure to find that theorem
prove that such a theorem is impossible. All 43 full-parser questions remain
explicitly unproved; their distinct recognition obligations stay in the report.

## The theorem, and what is actually compared

For each operation below define one problem Q = (D, O, E, M), using the
`parser-effort-v1` machine. Both candidates receive the **same canonical C entry
state**, operation, source bytes and parameters. They must implement the same
relation O on every member of D, not merely the fixture strings. There is one
admissible algorithm class A(Q). The two compiler transformations in the
[effort theorem](benchmark-parser-effort.md) are the identity on that class.
Consequently, for every x in D and every positive weight vector w,

```
inf { Cw(P,x) : P in A(Q) } = inf { Cw(P,x) : P in A(Q) }.
```

This establishes **equal optimal effort for the local problem**, without
asserting that either measured implementation attains the infimum. It works
because the complete operation contracts are identical, not because selected
ASTs, traces, timings or asymptotic classes are equal. Both measured algorithms
are native production operations surrounded by explicit descriptor adapters;
ALL those adapters execute inside `bench_effort_operation`. There is no
zero-cost transformation of native parser states in the theorem.

The suite compares implementations of the common task. It is not a synthetic
fast parser used as an oracle for an entire grammar. Callgrind Ir is machine
instruction work of these implementations, not an evaluation of the abstract
optimum. A ratio of 2 does not mean half the instructions can be removed.

## Common entry, ownership and failure contract

`effort_runner.h` defines the common state. Input is an immutable, independently
owned, length-delimited byte string plus a NUL sentinel. Length is nonnegative
and less than INT32_MAX/2. For the six byte tasks, output is a distinct mutable
buffer with capacity length+1. It initially holds input and its sentinel,
except for copy, whose initial logical length is zero. The caller retains both
allocations throughout the call; no output may borrow input. Byte output is
bytes, length and trailing NUL; bytes beyond the terminator are unspecified.
The ownership task instead returns a native graph described below; its separate
observation buffer has capacity 5*length+1. Input must remain unchanged. All
fresh-search result fields, the 81-word memo and the failure field start at zero;
the native graph pointer starts null.

There is sufficient writable capacity on both sides. The six byte operations
may not allocate, fail semantically, or report OOM. The ownership operation below charges
its native array allocation inside the measured edge and reports a checked
resource failure if calloc fails; neither native attach primitive allocates.
The optimum theorem concerns successful computation on resource-sufficient
machine states: both candidates have enough storage for their native array and
result. On that domain returning an allocation error is not an admissible way
to avoid constructing the graph. A real allocation failure invalidates the
measurement rather than producing a cheap success. The checked failure branch
is defensive behavior outside that promised measurement domain; no equality
of full-parser OOM policies is inferred.
This is a complete local contract with explicit preconditions, not a claim that the full parsers share failure
behavior. Core sticky OOM and cmark's allocator/abort behavior are deliberately
outside this domain. Admission rejects invalid lengths, cursor/run parameters,
CR/NUL in normalized inputs, and a cursor splitting a delimiter run. Rejected
inputs must produce a nonzero process result, never a timing row.

`bench_effort_prepare` pays for state allocation, output allocation, initialization
and input copying. `bench_effort_release` pays for cleanup. Both edges are reported
beside operation. Loading the fixture and verifying every output are harness
work outside these edges. The input buffer remains alive until all calls and
receipts finish. Each of 16 measured invocations gets an independent fresh state;
repeated calls do not accidentally benchmark an already normalized buffer.

## Seven complete local domains and output relations

| Operation | Domain beyond common entry | Exact output |
| --- | --- | --- |
| copy | Arbitrary bytes, including NUL and non-UTF-8 | Input bytes and length, independently stored |
| trim | Bytes excluding VT (11) and FF (12) | Remove the maximal leading and trailing runs from W = {9,10,13,32} |
| unescape | Arbitrary bytes | Left-to-right, replace backslash followed by ASCII punctuation with that punctuation; consume both bytes; retain every other byte including a final backslash |
| whitespace | Bytes excluding VT and FF | Replace every maximal nonempty W-run by one ASCII space; preserve all other bytes |
| code | NUL/CR-normalized bytes, otherwise arbitrary | Replace LF by space; if the result contains a non-space and starts/ends with space, remove exactly one at each end |
| closer | NUL/CR-normalized bytes; 0 <= start <= length at a maximal-run boundary; 1 <= ticks <= 80 | Find the first maximal backtick run of exactly ticks after start; return its exclusive end and cursor, or result=0/cursor=length/scanned=1 if absent. Memo[k] is the start of the last visited run of length k, k=1..80, initially zero; stop at the matching run. Preserve input/output bytes. |
| owners | Nonempty topologically ordered parent-index stream, as specified below | Ordered ownership graph; all five intrusive links per owner; checked allocation failure |

The byte domains include empty input, malformed/unclosed delimiters, arbitrary
high bytes and adversarial long runs. They cover every length admitted by the
fixed-word machine bound, not only the old six-digit template languages. The finite test corpus
is regression evidence; it is not a proof over every string.

### Ownership construction for all 43 structural pairs

The seventh operation consumes a nonempty array of little-endian uint32 parent
indices. Entry zero is the unique root with parent UINT32_MAX; every other
parent index must be less than its child index. The byte length is divisible by
four and below INT32_MAX/10, leaving room for the observation buffer. This is
the complete admitted domain: arbitrary depth, branching, insertion order and
size within that bound, not only the 43 fixture shapes. By induction on creation
index every node belongs to exactly one acyclic tree rooted at zero.

The output is an ordered **intrusive ownership graph** with the input parent
relation and siblings in creation order. Every node's parent, previous/next
sibling and first/last child is observed, not just the number of nodes. It is
not a complete typed AST. The observer serializes these five links as indices
only for checking, outside the operation edge, just as the byte observer prints
buffer contents outside it. Native pointers and layout are algorithm choices;
no source coordinates, semantic fields or bindings are in this local output.

Both adapters allocate one zeroed native-node array INSIDE operation, initialize
container tags, and append each fresh child using real production primitives:
Core's validated attach and cmark's checked append. Their different guards are
implementation costs of the same admitted task. Each child is initially empty
and detached; its parent is an earlier live node. Both use block containers
(Core Callout / cmark BlockQuote) so all these edges are admissible without
inventing a parser kind or bypassing a semantic rejection. Tag initialization,
native record size/zeroing, allocation and pointer linking are charged. The
array is caller-owned scratch for the ownership primitive; it is never passed
to a full-AST destructor or advertised as a fully initialized semantic AST.
Release frees the array once. Allocation failure sets the common explicit
failure field and fails the run; it is not a zero-work success.

Correctness follows by induction: before append i, all previous owners have the
required ordered child chains and node i is detached. Each native primitive
sets i.parent, i.prev, the previous last sibling's next (or parent's first),
and parent's last. The remaining links retain their induction invariant. The
fresh node's next/children remain null. Both therefore meet the same graph
relation on the entire admitted domain, so the identical-problem optimum theorem
applies. Native layout and allocation costs can differ substantially without
changing this equality of *problem* optima.

For each of the 42 production proofs the independent semantic action constructs
the canonical ownership tree at 16 and 32 distinct unit values; the recursive
span proof supplies nested-owner trees at those two scales. An iterative walk
encodes all owners, retaining order, depth and branching. This supplies **86
local ownership comparisons tied to all 43 proofs**, plus independent deep and
wide adversarial trees and the singleton boundary. The production proof is used
to derive topology, not to confer grammar-effort equivalence. Deciding those
owners, initializing their real kinds/fields, copying literals, allocating
parser-specific payloads, source mapping and bindings remain residual work.
The boundary table lists those obligations for every original pair. No ratio
of this isolated ownership graph is a replacement for whole-document A/R.

### Proven mismatch and boundary split: VT/FF

The unconstrained trim/whitespace operations do **not** have the same output
contract. Core classifies VT/FF as ordinary bytes; cmark classifies them as
whitespace. Concrete counterexamples: trim(VT) returns VT versus empty;
normalize(VT) returns VT versus space. This refutes the proposed identity of
these operations on all byte strings. It does not prove that their optima could
never coincide under another problem mapping.

The valid common domain excludes those two bytes. For an input containing them,
cut at every VT/FF: maximal intervening chunks satisfy the local contract;
handling the cut bytes and joining chunks are residual operations. Their costs
cannot be erased or inferred from chunk ratios. Admission on BOTH C and JS
sides rejects the unsplit input. All other byte values, including NUL and high
bytes, remain admitted; the test suite exhausts them. This is a semantic split,
not a corpus-size special case.

### Correctness arguments for the native adapters

Both buffer layouts are converted from the same entry descriptor inside the
measured function. Capacity length+1 exceeds every requested length, so the
native set/normalizer/drop/truncate paths cannot grow. Their output pointer
stays the supplied pointer; no caller state is silently transferred.

For copy, the production memmove writes exactly the input range, then size and
NUL. Empty input clears the descriptor and NUL. For trim, the first scan finds
the maximal W-prefix; drop preserves the remaining order; the reverse scan
removes exactly its W-suffix. The all-W case becomes empty. On the admitted
domain both classification tables have exactly the W set and the ASCII punctuation ranges 33..47, 58..64,
91..96, 123..126, independent of locale and signed-char representation.

For unescape, at every read boundary the written prefix equals the relation on
the consumed prefix, and write <= read. A recognized pair advances the read
position by two and writes its second byte; otherwise one byte is copied. The
sentinel makes the final backslash's lookahead non-punctuation. For whitespace,
the same prefix invariant holds with one Boolean recording whether the last
consumed byte was in W. The final truncate adds the sentinel.

For code, excluding CR means read and write positions are equal throughout the
production loop. Thus the non-space flag observes the **normalized** byte,
including when LF was changed to space. The second phase implements the exact
one-space padding rule. Empty/all-space cases never index before the buffer.
These statements would need a different argument on unnormalized CRLF input;
that input is rejected here, not quietly supplied to the benchmark.

For closer, the cursor skips only non-backticks and then consumes a maximal run.
Every loop either returns or advances, bounded by input length. Each memo write
records exactly the run just consumed. Both return at the first equal-width run
and otherwise mark the completed scan. Core supports at most 80 opening ticks;
cmark 0.31.2 supports 1000. Restricting the query to 1..80 makes their observable
relation identical even when the input contains longer runs. cmark's additional
memo slots are private working storage; initializing them and copying the common
81-slot result are charged in its adapter. Core's prepared common memo avoids
allocation but native state initialization still occurs inside the operation.
This is a **fresh search** certificate, not a certificate for cached continuation,
whole code-span parsing, source coordinates, or formula/comment closing rules.

## Relation to the 43 structural pairs

The full-pair audit remains in `pair-effort.mjs`; the generated boundary report
lists every proof and its unmatched obligations. Boundary certificates are
registered once for the seven tasks. The 43 ownership shapes are instances of
one local theorem, not 43 newly proved grammar equivalences. A local operand must pass its own domain
admission. Availability of a copy/normalization kernel does not prove a given
whole parse executed that kernel, nor that it copied the same number of bytes.

The valid decomposition is conceptual and explicit:

```
source/context -> recognition and boundary preparation -> admitted local task
               -> remaining ownership, binding, coordinates and lifecycle
```

Only the admitted local task has the identity theorem. Recognition, preparation
and residual work do not disappear. In particular, grid geometry, definition
precedence, directive/link activation, attribute field recognition and delimiter
flanking remain unmatched. The older Full/Without interventions can diagnose
those remainders, but their deltas are not optimal-effort certificates. Local
ratios must not be multiplied into, subtracted from, or used as a coverage
percentage of whole-document A/R. Whole-parser optimization still requires
same-input before/after profiles and semantic/failure tests.

A complete grammar certificate would require the stronger theorem from the
cost model; this PR does not rename local success as full success. The smaller
boundary domain is explicit and costs of reaching it are visible. There is no
claim that full equivalence is mathematically impossible.

## Reproduction and provenance

The existing stage benchmark builds both runners with its pinned compiler,
flags and cmark checkout, then runs the suite and writes `effort/effort.json`,
`effort/effort.md`, exact input files and raw Callgrind dumps. Report identity
hashes this proof, the cost model, both adapters, common driver, measurement
logic and build definitions. It conservatively includes the complete benchmark
library tree and stage entry point, so indirect cache/isolation, profile parsing
and compile-identity dependencies cannot change without invalidating the identity.
Each binary digest and actual compile inventory
is recorded alongside the parent run's toolchain/reference identity.

Production `code.c`/pinned `inlines.c` are included in adapter translation units
to expose their private normalizers without altering product APIs or copying
algorithms. Those translation units also supply their ordinary external symbols,
so the static linker does not extract a second copy from the archive. This build
context is part of the measurement, not an instruction trace of an unchanged
whole-parser binary. Code and input identities must match before comparing runs.

The independent JS oracle checks every invocation's complete logical bytes,
length (encoded by hex), sentinel (checked natively), cursor, result, scanned
flag and memo. Exhaustive short words, every byte value, empty/all-whitespace
inputs, backslashes, boundary run lengths and long misses guard the contract.
Missing Callgrind edges, wrong invocation counts, nonpositive Ir, or unequal
receipts fail the benchmark. A correctness-only local run emits no ratios.
