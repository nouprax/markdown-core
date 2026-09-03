# External parser oracles

This directory contains the repository's independent parser authorities and
the reviewed policy that defines each comparison:

- `cmark/` pins the newest stable cmark release as the sole primary authority
  for CommonMark syntax and parser complexity fixes.
- `cmark-gfm/` pins the dormant upstream fork only for its GFM extension layer.
- `remark/` pins the remark/micromark ecosystem as corrective and
  supplementary evidence for directives, formula, footnote, table, and
  reference semantics.
- `obsidian/` pins the most-used current npm Obsidian parser for the documented
  wikilink/embed, highlight, comment, and custom-task intersection it actually
  implements. Official Obsidian Help remains the language authority.
- `pandoc/` pins the official Pandoc 3.11 manual, reader sources, release CLI,
  and per-platform artifact digests for the explicitly selected Pandoc
  extension layer, including the shared attribute grammar and consumer model.
  Its parity gate is the first implementation-plan phase.

Each active gate's `deltas.json` records the authority version, compared
corpus, deliberate differences, and fail-closed exceptions. The Pandoc policy
currently has an immutable `source.json` and input-only corpus; it explicitly
does not claim parity until the planned gate and its initial delta registry
land. A registered difference must reproduce; a new difference and a
registered difference that disappears both fail an active gate.

Authority is scoped, not voted: cmark-gfm cannot override current cmark on the
base language, Pandoc cannot replace inherited CommonMark/GFM behavior with its
default dialect, and Remark's directive tokenizer cannot replace the explicitly
selected shared Pandoc attribute grammar.
When a primary implementation is demonstrably wrong, the exception is reviewed
and registered, with independent implementation agreement used as evidence
where available.

These are external oracle policies, not copies of Markdown Core's expected
output. Product-owned golden AST dumps remain solely in
`packages/markdown-core/tests/fixtures/`, and cross-binding contract fixtures
remain solely in `specs/canonical-ast/`. Root-level `.txt` golden mirrors are
forbidden here by `scripts/audit-test-topology.sh`.
