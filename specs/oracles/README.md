# External parser oracles

This directory contains the repository's independent parser oracles, the
evidence behind the dialect modules that state the rules, and the reviewed
policy that defines each comparison:

- `cmark/` pins the newest stable cmark release as the sole primary oracle
  for CommonMark syntax and parser complexity fixes.
- `cmark-gfm/` pins the dormant upstream fork only for its GFM extension layer.
- `remark/` pins the remark/micromark ecosystem as corrective and
  supplementary evidence for directives, formula, footnote, table, and
  reference semantics.
- `obsidian/` pins the most-used current npm Obsidian parser for the documented
  wikilink/embed, highlight, comment, and custom-task intersection it actually
  implements. The official Obsidian Help is the source of its feature
  definitions; the dialect modules under `docs/specs/dialect/` are the rule.
- `pandoc/` pins the official Pandoc 3.11 manual, reader sources, release CLI,
  and per-platform artifact digests for the explicitly selected Pandoc
  extension layer, including the shared attribute grammar and consumer model.
  Its parity gate is the first implementation-plan phase.

Each active gate's `deltas.json` records the oracle version, compared
corpus, deliberate differences, and fail-closed exceptions. The Pandoc policy
pins its immutable `source.json`, input-only corpus and exact projection digests.
A registered difference must reproduce; a new difference and a
registered difference that disappears both fail an active gate.

Seeded differential fuzzing compares only the shared language of its selected
oracle. Since P4, a document combining a heading with unresolved bracket text
can activate implicit heading references, which cmark, cmark-gfm and remark do
not implement. `scripts/lib/fuzz-scope.mjs` conservatively classifies that
composition with an independent CommonMark parse. It neither consults product
output nor implements heading-label matching. Explicitly resolved references,
headings without unresolved brackets, references without headings, and brackets
inside code/HTML remain eligible for comparison. The scope tests run before
every `pnpm fuzz:parity` invocation.

Out-of-scope and exact registered inputs are reported separately from actual
comparisons; neither counts as an agreement, and zero comparisons fails the
run. The ordinary parity gates keep their fail-closed comparison unchanged.
The anchors fixtures retain the complete CI seed-1 witness, and the Pandoc
corpus independently verifies its reduced heading-reference interaction.
The `heading-anchor-unavailable` Remark boundary projects out only
`Heading.anchor`, a fact mdast cannot express. Heading levels, content and
attributes, and anchors on other kinds remain compared. This projection runs
in the ordinary gate as well as fuzzing; its tests prove those remaining facts
still detect differences. Heading-anchor values are checked by Pandoc and the
product fixtures.

Evidence is scoped, not voted: cmark-gfm cannot override current cmark on the
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
