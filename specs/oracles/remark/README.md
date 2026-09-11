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

mdast does not retain ordered-list delimiter punctuation, so this oracle does
not compare `List.delimiter`. The cmark oracle compares that field, and the
canonical fixtures check the authored spelling in every binding.

mdast's ragged table rows are projected to the delimiter's column count,
matching its HTML conversion and the canonical AST: absent cells are empty and
excess cells are omitted. Only the oracle tree is normalized; a malformed
native row still fails comparison.

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

`list-tightness-shape` combines mdast list and direct-item spread flags into
one tight flag, following mdast-util-to-hast's `listLoose`. Each list is
projected independently, and the native flag is compared unchanged.

`task-marker-completion` compares absent, incomplete, and complete task states
against boolean-only mdast and cmark-gfm XML. Those oracles cannot attest to
`x` versus `X`; exact authored markers remain covered by canonical fixtures
and binding tests. An unchecked marker followed by literal `[x]` still
exposes the registered upstream task-state defect.

P8 adds nameless containers to the shared directives corpus. Remark implements
named directive envelopes only, so eleven exact-input witnesses retain its
literal fallback for the new openers and their nested or opaque content.
Pandoc independently checks nameless recognition and definition-list content.

The reduced `directive-inherited-lazy-parent` witness retains the product's
pre-P8 lazy paragraph continuation through quoted directives. Rebuilding the
previous commit confirms unchanged behavior; Remark ends at the missing prefix.
The O5 task-prefix exclusion covers the already registered newline-only separator
case as well as empty task bodies followed by spaces.
