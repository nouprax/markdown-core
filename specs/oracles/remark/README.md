# remark/micromark oracle

`deltas.json` defines where remark/micromark is authoritative, pins the npm
dependency surface through the repository lockfile, selects the compared
corpus, and records every deliberate divergence. `corpus.md` supplies focused
inputs for semantics not already covered by the C extension fixtures.

`scripts/check-mdast-parity.mjs` parses each input independently with remark
and Markdown Core, projects only mutually representable fields, and fails when
an unregistered difference appears or a registered one stops reproducing.
`scripts/fuzz-parity.mjs --oracle remark` reuses the same policy for seeded
generated inputs.

This oracle is corrective and supplementary, not a second owner of the base
language. Current cmark owns CommonMark syntax, cmark-gfm owns only its GFM
extension layer, and a remark agreement can justify a reviewed delta without
silently overriding either primary authority.

The stored corpus contains inputs only. It never stores Markdown Core's
expected output and is not a replacement for canonical AST conformance.
