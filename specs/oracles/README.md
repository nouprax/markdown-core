# External parser oracles

This directory contains the repository's independent parser authorities and
the reviewed policy that defines each comparison:

- `cmark-gfm/` pins upstream cmark-gfm for the shared CommonMark/GFM language.
- `remark/` pins the remark/micromark ecosystem for directive, formula,
  footnote, and table semantics that cmark-gfm cannot judge.

Each `deltas.json` records the authority version, compared corpus, deliberate
differences, and fail-closed exceptions used by its parity gate. The remark
oracle also owns a small purpose-built input corpus. A registered difference
must reproduce; a new difference and a registered difference that disappears
both fail the gate.

These are external oracle policies, not copies of Markdown Core's expected
output. Product-owned golden AST dumps remain solely in
`packages/markdown-core/tests/fixtures/`, and cross-binding contract fixtures
remain solely in `specs/canonical-ast/`. Root-level `.txt` golden mirrors are
forbidden here by `scripts/audit-test-topology.sh`.
