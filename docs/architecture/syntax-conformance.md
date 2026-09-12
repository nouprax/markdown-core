# Syntax conformance

[Documentation](../specs/README.md) · [Testing architecture](testing.md)

The [syntax guide](../specs/dialect.md) defines Markdown Core's language. The
[canonical AST contract](../specs/canonical-ast.md) defines its public values.
Tests check those contracts; implementation plans and third-party output do not
silently replace either one.

## Where evidence lives

| Material | Owner | Purpose |
| --- | --- | --- |
| Reader examples and boundary rules | `docs/specs/dialect/` | Explain accepted syntax and parsed results. |
| Machine kind/field inventory | `docs/specs/canonical-ast.json` | Check all public projections and documentation tables. |
| Shared input/AST pairs | `specs/canonical-ast/` | Verify the same contract independently on every platform. |
| Element correctness fixtures | `packages/markdown-core/tests/fixtures/` | Assert exact source-to-AST behavior, including malformed and composed cases. |
| External parser policies and inputs | `specs/oracles/` | Pin independent evidence and review exact differences. |
| Design and implementation decisions | `docs/architecture/` and `docs/plans/` | Explain implementation invariants and historical decisions. |

Wiki examples use ordinary fenced Markdown followed by a prose result. Full
expected dumps live in executable fixtures, avoiding a second, stale dump corpus
inside reader documentation. Changing prose must preserve the stated semantics;
a language change updates its specification and executable evidence together.

C correctness fixtures use the CommonMark-style 32-backtick example format:
source, a line containing `.`, and the canonical expected dump. Fence tags
classify inputs for oracle selection; they never turn syntax on or off. Only
an explicit `disabled` fixture marker affects execution. The native runner
parses each case once, dumps twice to check determinism, and compares exact
bytes. Its `--rewrite` mode generates candidates for human review; it is not
a way to accept unintended parser drift.

## Independent parsers

The [oracle directory](../../specs/oracles/README.md) holds each parser's pinned
version, comparison policy, corpus, and registered differences. Lockfiles and
machine policies hold artifact integrity values; reader syntax pages do not
repeat package hashes.

| Evidence | Scope | Entry |
| --- | --- | --- |
| cmark | CommonMark foundation | `pnpm check:commonmark-parity` |
| cmark-gfm | GFM additions implemented by that parser | `pnpm check:gfm-parity` |
| remark/micromark | Supplemental structures, directives, and formula comparisons | `pnpm check:mdast-parity` |
| Obsidian-compatible parser and YAML test tooling | Their implemented syntax/metadata intersection | `pnpm check:obsidian-parity` |
| markdown-it with markdown-it-ins | Insertions | `pnpm check:ins-parity` |
| Pandoc | Explicitly selected Pandoc-derived syntax | `pnpm check:pandoc-parity` |

`pnpm check:oracle-parity` runs these gates. Each invokes the same full-dialect
product parser. The source of a spelling limits which inputs an oracle judges;
it does not select a product parsing mode. The Obsidian-compatible package does
not attest to features it lacks, and YAML tooling is neither a runtime dependency
nor authority for accepting additional properties syntax.

A deliberate difference must identify exact inputs/digests or a narrowly scoped
projection in the oracle's policy. A new difference fails the gate; a registered
difference that stops reproducing also fails. A known implementation gap remains
explicitly tracked until fixed. Do not normalize away product values to obtain
agreement or treat excluded inputs as comparisons.

Some external models lack facts this AST retains. Examples include raw task
markers, generated heading anchors, definition compactness in certain body
shapes, and shared table geometry. Their policies state exactly what can be
compared. Product fixtures continue to assert the complete value, scope, and
ownership model. Exact-input witnesses cover differences such as attribute
duplicates, empty superscripts, citation fallback, and container continuation.

Seeded differential fuzzing compares the supported shared-language domain using
an independent classifier, never product output to decide eligibility. It
reports out-of-scope and registered cases separately from real comparisons;
zero comparisons fails. Regular parity gates keep their fail-closed policy.
See the [oracle policy](../../specs/oracles/README.md) for heading-reference
classification and projection boundaries.

## Reviewing a language change

1. Update the owning syntax page with normal usage, parsed results, and boundary
   behavior. Cross-syntax precedence belongs in the compatibility page and the
   affected syntax rules, not a new implementation-stage checklist.
2. Add or update package fixtures for successful, malformed, empty, escaped,
   nested, and interacting forms. Verify original-source scopes, not just node
   names. Preserve definitions, duplicate values, and empty states as required
   by the contract.
3. Add shared canonical cases for new kinds, fields, enum values, and nullable
   states. Update the JSON inventory, prose, dump grammar, and every binding
   together. The shared manifest is the sole cross-platform case list.
4. Test failure and resource invariants: allocation failures publish no partial
   result, deep traversal is stack-safe, and repeated malformed candidates do
   not cause unbounded rescanning or expansion. A size-doubling case parses
   the same construction at n and 2n bytes and checks expected AST structure
   and a bounded output/source byte ratio; timing alone is not proof.
5. Register deliberate external differences and close any implementation gaps
   addressed by the change. Run the affected oracle gates, position audits,
   correctness tests, and conformance targets.

Important compositions include bracket precedence, literal-body ownership,
task prefixes before the first block, inherited attributes and references,
heading/specimen resolution independent of source order, caption ownership,
and table span geometry. Maintain semantic coverage rather than a percentage
of executed branches or a duplicate documentation-only fixture registry.

## Checks for documentation work

Parse newly written examples with the product CLI and compare their output with
the prose. Check links and rendered code fences, especially nested code examples,
escaped table pipes, and literal backslashes. Run the AST projection and fixture
audits when touching contract prose. Preserve historical executable cases even
when their full dumps are removed from reader pages.

Run tracked-file audits again after adding new files to the index. Run the
repository's clean-checkout audit after committing and before pushing. See
[testing](testing.md) for platform checks and [releasing](../releasing.md) for
release acceptance.
