# External parser oracles

This directory contains the repository's independent parser authorities and
the reviewed policy that defines each comparison:

- `cmark/` pins the newest stable cmark release as the sole primary authority
  for CommonMark syntax and parser complexity fixes.
- `cmark-gfm/` pins the dormant upstream fork only for its GFM extension layer.
- `remark/` pins the remark/micromark ecosystem as corrective and
  supplementary evidence for directives, formula, footnote, table, and
  reference semantics.

Each `deltas.json` records the authority version, compared corpus, deliberate
differences, and fail-closed exceptions used by its parity gate. The remark
oracle also owns a small purpose-built input corpus. A registered difference
must reproduce; a new difference and a registered difference that disappears
both fail the gate.

Authority is scoped, not voted: cmark-gfm cannot override current cmark on the
base language, and remark does not silently override either primary oracle.
When a primary implementation is demonstrably wrong, the exception is reviewed
and registered, with remark/mdast agreement used as independent evidence where
available.

These are external oracle policies, not copies of Markdown Core's expected
output. Product-owned golden AST dumps remain solely in
`packages/markdown-core/tests/fixtures/`, and cross-binding contract fixtures
remain solely in `specs/canonical-ast/`. Root-level `.txt` golden mirrors are
forbidden here by `scripts/audit-test-topology.sh`.
