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
- exposes a read-only C AST facade and deterministic native AST debug dump;
- adds the repository-owned directive, formula, and GFM syntax extensions;
- parses one complete source buffer per call and has no public feed/finish or
  file parsing lifecycle;
- deletes the inherited renderers and renderer extension hooks;
- maintaining immutable Swift, Kotlin, and ECMAScript/TypeScript bindings at
  the same commit as the C engine.

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
