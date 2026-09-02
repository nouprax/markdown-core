# cmark-gfm oracle

`deltas.json` pins cmark-gfm `0.29.0.gfm.13` and registers every reviewed
difference in the GFM extension layer.

`scripts/check-upstream-parity.mjs --oracle gfm` enforces it. It selects only
GFM extension sections and extension-tagged regressions, runs Markdown Core
with directive and formula disabled, and fails on any unregistered drift.

This oracle is intentionally not an authority for CommonMark. cmark-gfm has
not published a release since 2023; the separate cmark oracle follows the
newest stable CommonMark reference release instead.

## Why this exists

The product-owned golden tests compare Markdown Core against reviewed output
stored in this repository. The spec fixtures contain the CommonMark and GFM
specifications' examples, but their expected blocks are canonical AST dumps
this parser produced. They pin behaviour without independently proving it, and
a divergence introduced before those dumps were frozen would be preserved.

This gate supplies the primary implementation authority for tables,
strikethrough, autolinks, task-list items, and cmark-gfm footnotes. The sibling
remark/micromark oracle supplies corrective and supplementary evidence.

## Adding a difference

A divergence is a defect until it is deliberately made otherwise. Making it
deliberate means:

1. an entry in `deltas.json` with the input that shows it, both outputs, and
   the evidence for the decision;
2. a row in the table in `docs/specs/canonical-ast.md`; and
3. review of both.

A divergence is never accepted because this implementation looks obviously
right. That is exactly how an accidental one gets normalized into a rule.

## What the comparison does not cover

Upstream's XML writer does not emit table column alignments, an
indented-vs-fenced code flag, a fence-closed flag, or footnote labels, so those
fields have nothing on the upstream side to compare against. They are pinned
by this repository's own golden dumps instead. The comparison also covers only
the corpus it is given, so behaviour no input reaches is behaviour it cannot
check.

Upstream's XML writer has no element name for the footnote extension's nodes
and emits `<unknown>` for both of them; the normalizer tells a reference from a
definition by whether the element is a leaf. If upstream ever emits `<unknown>`
for a third node kind, that heuristic stops being sound — which is why an
unmapped node kind fails the gate rather than being skipped.

## Moving the pin

The upstream pin is an immutable commit, not a tag, for the same reason the
emsdk pin is: a moved tag must never change what the comparison compares
against. Upstream's newest release is `0.29.0.gfm.13` from 2023-07-21 and the
project has been dormant since, so this should rarely move. When it does, it is
a reviewed change — it redefines what parity means, and it can retire entries
in `deltas.json` (the gate already fails if a registered difference stops
reproducing, so a fix upstream cannot pass unnoticed).

## Running it

```sh
scripts/init-environment.sh --install oracle-cmark-gfm # build the pinned oracle
pnpm build:c
pnpm check:gfm-parity                                  # add -- --verbose for every diff
```
