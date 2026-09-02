# Agent instructions

These instructions intentionally contain repository-independent engineering
and execution principles. Keep product concepts, module-specific rules,
temporary project state, and repository-layout-dependent references in the
relevant specifications or architecture documentation instead of this file.

## First-principles engineering

- Every code change must be designed from the full set of current requirements
  and invariants. Do not optimize for the smallest patch, the fewest edited
  lines, or the quickest local fix.
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
- Before changing an abstraction, audit all of its callers and consumers so
  the result is repository-wide and internally consistent. Remove superseded
  paths rather than leaving parallel legacy and replacement mechanisms.
- Tests must verify semantic invariants and failure boundaries. Passing the
  current examples is necessary but is not proof that the abstraction is
  sound.
- Code review follows the same standard. Reject changes that merely mask a
  symptom, encode an input-shape exception, duplicate an existing model,
  obscure ownership or lifecycle, or buy short-term gains by planting future
  failure modes—even when the patch is small and all current tests pass.

## Performance and complexity

- Performance is a design requirement, not a later patch. Reason about
  asymptotic work, allocation behavior, data locality, and adversarial inputs
  before choosing the abstraction.
- Benchmarks are diagnostic evidence and regression gates; they are not an
  oracle for designing alternate algorithms around the measured examples.
- Do not introduce branches based on benchmark-observed cardinality, input
  size, or a convenient “common case” to recover a local number. Improve
  constant factors by improving the shared algorithm or its data structure.
- Complexity tests must verify the intended general invariant, including
  adversarial shapes that defeat the former implementation. A favorable timing
  result alone is not proof that the implementation is sound.
- Every loop on the derive or feed path names the dimension that bounds it —
  depth D, width W, open O, changed C, extensions E, distinct names R, table
  length T, feeds K — and the product per derivation or per line. A loop
  bounded by a dimension its unit does not own (the parent chain per node, the
  spine per line, the table per container, the extension list per block) is
  the finding, before any measurement; the fix carries the context down the
  walk that already stands on it rather than re-deriving it per unit. The
  parser's derive ledger (`core/parser.h`) and the work-ledger shape matrix
  (`projection_runner --case work_ledger`) are where a new term must show as
  a count, under every sanitizer preset, and `scripts/complexity-callgrind.sh`
  is where it must show as instructions per unit that do not grow with the
  size.

## Execution environment boundaries

- Treat the sandbox, container, and host machine as distinct execution
  environments. A result observed inside the sandbox is evidence about the
  sandbox only unless the tool explicitly runs with host access.
- Do not infer host credential, network, keychain, GUI, daemon, device, or
  filesystem state from a sandbox failure. In particular, a sandboxed
  `gh auth status` or GitHub network error does not prove that the host GitHub
  CLI is logged out or offline.
- When a task depends on a host integration, use the available host-scoped or
  escalated mechanism to verify it before reporting a blocker or asking the
  user to reconfigure anything. State which environment produced the evidence.
- Never work around sandbox boundaries implicitly. Use the platform's explicit
  approval or connector path, keep the requested authority scoped to the task,
  and distinguish an approval denial from a real host-side failure.
