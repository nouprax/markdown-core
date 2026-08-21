# Reference resolution

`ledger.json` records D9: **whether a reference resolves depends on how many
resolved before it.** `scripts/audit-reference-order-independence.mjs` enforces
it, and the enforcement is unusual — **the gate is registered RED and fails if a
row stops reproducing.**

That is the mdast backlog's shape, for the same reason. D9 has no local fix, and
a gate that only caught the defect appearing would be satisfied for the wrong
reason the day someone deletes the budget: the rows would clear, every other
suite would stay green, and the engine would be quietly producing 134 MB of
output from 656 KB of input.

## The trade, measured

| | order-independent | output bounded |
|---|---|---|
| today, with the budget | **no** — 100 of 200 identical references resolve | yes — 0.999x |
| budget deleted | yes | **no** — 204.678x |
| Step 9a's model | yes | yes |

The budget exists because resolving a reference **copies** the definition's
destination and title into the node. Step 9a lets a reference *name* its
definition instead, which removes the reason for a budget rather than the
budget. Nothing smaller reaches both properties, which is why D9 is the one
defect in §2 that Stage 0a pins rather than fixes.

## The two gates

```
node scripts/audit-reference-order-independence.mjs   # RED, 2 rows, must stay red
ctest --preset correctness -R pathological_complexity_reference_expansion_bound   # GREEN, must stay green
```

Neither may be satisfied by giving up the other. The statement lives beside the
code as well, at `packages/markdown-core/core/map.c`, so a reader who arrives at
the three-line guard without this file finds out why it is there before deleting
it.
