# remark/micromark oracle

`deltas.json` defines where remark/micromark is the evidence, pins the npm
dependency surface through the repository lockfile, selects the compared
corpus, and records every deliberate divergence. `corpus.md` supplies focused
inputs for semantics not already covered by the C extension fixtures.

`scripts/check-mdast-parity.mjs` parses each input independently with remark
and Markdown Core, projects only mutually representable fields, and fails when
an unregistered difference appears or a registered one stops reproducing.
`scripts/fuzz-parity.mjs --oracle remark` reuses the same policy for seeded
generated inputs.

This oracle is corrective and supplementary, not a second oracle for the base
language. Current cmark is the CommonMark oracle, cmark-gfm only the
GFM-extension oracle, and a remark agreement can justify a reviewed delta
without silently overriding either primary oracle. The dialect modules state
every rule; no oracle is an authority over behavior.

For directives, Remark is the evidence for the envelope, label, and attribute
attachment position, which the directives module states. It is not evidence
for the attribute member grammar or public attribute shape: those follow the
pinned Pandoc 3.11 contract at every attachment site.
The active delta registry records only differences the current runtime already
exhibits; Phase 2 must register the additional grammar differences atomically
when it replaces the existing directive-only attribute parser.

The stored corpus contains inputs only. It never stores Markdown Core's
expected output and is not a replacement for canonical AST conformance.
