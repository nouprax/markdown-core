# Reference-resolution invariants

Reference resolution is order-independent and output-bounded.
`scripts/audit-reference-order-independence.mjs` enforces the first property;
`pathological_reference_expansion_bound` enforces the second without timing.

The parser's reference map owns each winning definition's resource -- its
destination and title -- once, and every occurrence that resolves to the label
is the `Link` or `Image` it names, reading through that one resource; the
identity is what `markdown_core_node_resource` answers with, and each binding
materializes a distinct resource once. The model therefore needs neither a
resolution budget that changes semantics nor output growth proportional to
destination length times reference count.

`ledger.json` is intentionally empty and fail-closed. A row appearing means
that identical references resolved differently because of their order or
because unrelated labels consumed shared work.

```sh
node scripts/audit-reference-order-independence.mjs
ctest --preset correctness -R pathological_reference_expansion_bound
```

Both gates must stay green; satisfying one by giving up the other is a model
regression.
