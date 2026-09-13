# Reference-resolution invariants

Reference resolution is order-independent and output-bounded.
`scripts/audit-reference-order-independence.mjs` enforces the first property;
`pathological_reference_expansion_bound` enforces the second without timing.

The parser's reference map owns each winning definition's resource -- its
destination, title, and normalized attributes -- once, and every occurrence that resolves to the label
is the `Link` or `Embedded` it names, reading through that one resource; the
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

P2 extends the structural bound to long anchors, class vectors, and record
values, including occurrence-local merge declarations. The C facade exposes
borrowed primary and inherited attribute values with checked indexed access.
JNI and Wasm encode the definition value once; Swift, Kotlin, and ES decode it
once per parse into their own language-native values. An occurrence without
local declarations reuses its immutable or copy-on-write collections; local
merges allocate native collections proportional to the exposed result. This is
an output-cost boundary, not a requirement for cross-language shared storage.

P4 uses the same resource for implicit heading references. Heading labels are
declared before reference lookup, with explicit definitions taking priority;
the final `#anchor` target is filled once after P3 reserves explicit anchors
and synthesizes heading anchors. Forward references, references inside headings,
and references in owned fields all use this map. Heading attributes and source
ranges are never inherited by a virtual reference. See the
[lifecycle and complexity argument](../../docs/architecture/heading-resolution.md).
