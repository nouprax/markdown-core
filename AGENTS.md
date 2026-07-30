# Repository agent instructions

## First-principles engineering

- Every code change must be designed from the full set of current
  requirements and invariants. Do not optimize for the smallest patch, the
  fewest edited lines, or the quickest local fix.
- Prefer the simplest coherent abstraction that makes semantics, ownership,
  lifecycle, failure behavior, and performance explicit. “Minimal” means the
  fewest independent concepts and mechanisms, not the smallest diff.
- One semantic operation should have one data model and one general algorithm.
  A separate path is justified only by a documented semantic, ownership, or
  lifecycle invariant—not by a convenient input shape, a current test, or a
  benchmark result.
- Treat awkward control flow, duplicated state, leaky ownership, magic values,
  mode flags, repair-up callbacks, and exceptions to an abstraction as
  evidence that the model is wrong. Fix the model instead of normalizing ugly
  code around it.
- Do not accept an implementation that creates latent complexity, divergent
  behavior, or maintenance debt for a short-term delivery or benchmark gain.
  If the durable design requires a broader refactor, perform that refactor and
  protect its invariants with tests.
- Performance is a design requirement, not a later patch. Reason about
  asymptotic work, allocation behavior, data locality, and adversarial inputs;
  then use measurements as evidence. Do not trade a robust general design for
  a locally faster special case.
- Before changing an abstraction, audit all of its callers and consumers so
  the result is repository-wide and internally consistent. Remove superseded
  paths rather than leaving parallel legacy and replacement mechanisms.
- Tests must verify semantic and complexity invariants, including adversarial
  shapes that would defeat the previous design. Passing the current examples
  is necessary but is not proof that the abstraction is sound.
- Code review follows the same standard. Reject changes that merely mask a
  symptom, encode an input-shape exception, duplicate an existing model,
  obscure ownership or lifecycle, or buy short-term gains by planting future
  failure modes—even when the patch is small and all current tests pass.

## Performance refactoring

- Benchmarks are diagnostic evidence and regression gates; they are not an
  oracle for designing alternate algorithms around the measured examples.
- Do not introduce branches based on benchmark-observed cardinality, input
  size, or a convenient “common case” (for example `count == 1`) to recover a
  local number. One semantic operation must have one coherent algorithm and
  data model.
- Improve constant factors by improving that shared algorithm or its data
  structure. A separate path is acceptable only when a documented semantic,
  ownership, or lifecycle invariant makes it a genuinely different
  operation—not because a benchmark happens to favor it.
- Complexity tests must verify the intended general invariant, including
  adversarial shapes that defeat the former implementation.

The durable rationale is recorded in
`docs/specs/test-architecture.md` and the active first-principles ledger.
