# Repository agent instructions

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
