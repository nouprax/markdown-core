# Upstream lineage and product boundary

Markdown Core inherits from the cmark and cmark-gfm projects. Its C engine
baseline comes from the independently developed fork at
<https://github.com/DongyuZhao/cmark-gfm>.

The exact baseline is commit
`711032b2a16cf25c3df75033833eba086b17ca6a` (`[Feature] Support detect fenced
code status`, committed on 2026-07-05). The baseline tree was imported without
upstream tags. Markdown Core has its own release lineage, beginning at `1.0.0`.

The inherited source already contains code rewritten or extended relative to
cmark and cmark-gfm. Markdown Core continues from that work as an independent
parser product. Its current reconstruction:

- reorganizing the C engine under the monorepo package boundary;
- removing Microsoft-specific parser extensions and options;
- replacing inherited product, ABI, target, file, and package names;
- exposing a read-only C AST facade and deterministic native AST debug dump;
- adds the repository-owned directive, formula, and GFM parser extensions;
- parses one complete source buffer per call and has no public feed/finish or
  file parsing lifecycle;
- deletes the inherited renderers and renderer extension hooks;
- maintaining immutable Swift, Kotlin, and ECMAScript/TypeScript bindings at
  the same commit as the C engine.

## Upstream synchronization policy

The historical fork does not make dormant cmark-gfm the authority for all
Markdown. The upstream layers are intentionally separated:

- CommonMark syntax follows the newest stable cmark release, currently cmark
  `0.31.2` at commit `eec0eeba6d31189fd828314576494566d539b1e3`.
- Tables, strikethrough, autolinks, task-list items, and footnotes use
  cmark-gfm `0.29.0.gfm.13` only as the GFM extension oracle.
- remark/mdast is corrective and supplementary evidence for directives,
  formula, footnote representation, tables, and references. It does not
  silently override either primary authority.

The cmark `0.29.0` to `0.31.2` parser range has been audited and imported. This
includes current CommonMark parsing behavior, published complexity and memory
safety fixes, numeric-entity limits, Unicode Symbol delimiter behavior,
Unicode 17 case folding/classification, compact entity data, and scanner
changes. The exact audit and exclusions are recorded in
`specs/oracles/cmark/IMPORTS.md`.

Synchronization is semantic, not a source-tree merge. Renderer, streaming
parser, mutable node, CLI-format, and build/install changes remain excluded
because this repository deliberately removed those product surfaces. Inline
source-position changes are reviewed against the canonical AST's stronger
source-ownership rule and registered where cmark's representation differs.

The repository completed its product and ABI rename as a distinct migration
step. Old names remain only where needed to describe project history, upstream
attribution, and inherited licenses; no compatibility ABI is provided.

The parser has no writable process-global state. Extension descriptors and
lookup tables are immutable, while parser, option, allocation, and extension
attachment state belongs to each one-shot parse instance. Independent parser
instances may therefore execute concurrently.

Original copyright, attribution, and license notices remain in `LICENSE` and
`COPYING`. They apply to inherited code and data as described in those files.
New work must preserve every applicable inherited notice.

Archived baseline inventories under `docs/deprecated/` are historical evidence
only. The current architecture is specified in `docs/RECONSTRUCTION.md`.
