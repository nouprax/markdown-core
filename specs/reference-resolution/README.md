# Reference-resolution invariants

Reference resolution is order-independent and output-bounded.
`scripts/audit/check-reference-order-independence.mjs` enforces the first property;
`pathological_reference_expansion_bound` enforces the second without timing.

Each definition is a `Reference` node where it was written, holding its
destination, title and attributes once. Every occurrence that resolves is the
`Link` or `Embedded` it names, holding the normalized label in a `reference`
destination and only its own attributes; the document resolves the label to
its `Reference` or `Heading`. The model therefore needs neither a resolution
budget that changes semantics nor output growth proportional to destination
length times reference count.

`ledger.json` is intentionally empty and fail-closed. A row appearing means
that identical references resolved differently because of their order or
because unrelated labels consumed shared work.

```sh
node scripts/audit/check-reference-order-independence.mjs
ctest --preset correctness -R pathological_reference_expansion_bound
```

Both gates must stay green; satisfying one by giving up the other is a model
regression.

The structural bound covers long anchors, class vectors, and record values
on the definition and on its occurrences: each node's payload is its own, and
the sum stays within the source bytes. Swift, Kotlin, and ES build each node's
values from that node alone.

Headings declare reference labels too. Explicit definitions take priority, so
a label resolves to a heading only when no `Reference` states it. Forward
references, references inside headings, and references in owned fields all
use the same lookup. See the
[lifecycle and complexity argument](../../docs/architecture/heading-resolution.md).
