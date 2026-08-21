# Position oracles

Three gates that judge the engine's source positions, and one ledger each.

| Ledger | Script | Asks |
|---|---|---|
| `inline-sourcepos.json` | `scripts/audit-inline-sourcepos.mjs` | does an authority outside this repository agree? |
| `containment.json` | `scripts/audit-scope-containment.mjs` | is the tree's geometry consistent with itself? |
| `places.json` | `scripts/audit-position-places.mjs` | does each coordinate name a byte that exists? |

## Why three

A source position in this engine is a (line, column) pair counted in **bytes**
from 1, and the canonical dump prints one closed interval of them per node.
Before these gates, nothing in the repository checked that those numbers named
anything:

- the golden dumps **assert** them, which means a wrong position is preserved
  by regeneration rather than caught by it;
- `scripts/audit-scope-sanity.mjs` classifies three shapes that are not
  positions at all — the `0:0..0:0` sentinel, a reversed range, and line zero —
  and passes everything else;
- both parity gates compare structure and text and drop position entirely.

So a **well-formed but wrong** position sailed through every gate. That is how
`Text scope=0:0..0:0` came to be asserted as expected output, and how a `Code`
node ending four columns past the end of its own line still does.

None of the three subsumes another, and each is blind where another sees:

- Upstream cmark-gfm carries several of this engine's position defects — the
  autolink column, the link start taken from the closing bracket, the
  whole-run emphasis start, the consumed-definition line. It cannot be the
  authority for them, which is why `inline-sourcepos` compares inline `Code`
  and `HTML` **only**: those are where upstream is right and this engine is
  known to be wrong. Widening it would make it go red on the commits that fix
  the rest.
- Containment is blind to a whole subtree displaced by the same amount, and
  blind to `Code scope=1:9..1:17` on a twelve-byte line. Its sibling half is
  what catches two nodes claiming one byte, which is not a containment
  violation at all.
- Place-ness needs no authority and no model of what a node ought to cover, and
  it is the only one that reads the input. It is also the only one that can see
  a position which is inside its parent, agrees with upstream, and still names
  no byte.

Measured, on the tree these ledgers were first written against: un-gating
`adjust_subj_node_newlines` clears all 12 `inline-sourcepos` rows, moves
`places` by 13 out and 3 in, and moves `containment` by **nothing**. Correcting
`S_insert_emph`'s columns clears 14 `containment` rows and moves the other two
by **nothing**.

## The protocol

Each ledger records the exact rows that are wrong today, grouped by the input
that produces them, and the gate requires the measured set to match **exactly**.
Not a budget — a set. A count cannot tell a fix that cleared twelve rows from
one that cleared twelve and introduced one, and that is not hypothetical: the
un-gating above does precisely that.

So both directions fail without `--update`, and `--update` is a deliberate act
taken in the commit that moves the behaviour, whose message names the rows.

```
node scripts/audit-inline-sourcepos.mjs [--update] [--verbose]
node scripts/audit-scope-containment.mjs [--update] [--verbose]
node scripts/audit-position-places.mjs   [--update] [--verbose]
```

A row identity is what was measured — the input, the node's index path from the
document, its kind, and the position. `class` and `closedBy` are annotations
written by hand: they carry across an update and never distinguish two rows.
`class` is analysis, `closedBy` is measurement, and a row whose owner has not
been measured says `unassigned` rather than guessing.
