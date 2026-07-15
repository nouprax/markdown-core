# Upstream and Project History

Markdown Core inherits from the cmark and cmark-gfm projects. Its C engine
baseline comes from the independently developed fork at
<https://github.com/DongyuZhao/cmark-gfm>.

The exact baseline is commit
`711032b2a16cf25c3df75033833eba086b17ca6a` (`[Feature] Support detect fenced
code status`, committed on 2026-07-05). The baseline tree was imported without
upstream tags. Markdown Core has its own release lineage, beginning at `1.0.0`.

The inherited source already contains code that was rewritten or extended
relative to cmark and cmark-gfm. Markdown Core continues from that work as an
independent project and does not plan to merge its changes back upstream.

The initial independent development plan includes:

- reorganizing the C engine under the monorepo package boundary;
- removing Microsoft-specific parser extensions and options;
- replacing inherited product, ABI, target, file, and package names;
- introducing a read-only C AST facade and deterministic native AST dump;
- migrating parser tests away from renderer output, then deleting renderers;
- adding immutable Swift, Kotlin, and ECMAScript/TypeScript bindings maintained
  at the same commit as the C engine.

The repository completed its product and ABI rename as a distinct migration
step. Old names remain only where needed to describe project history, upstream
attribution, and inherited licenses; no compatibility ABI is provided.

Original copyright, attribution, and license notices remain in `LICENSE` and
`COPYING`. They apply to inherited code and data as described in those files.
New work must preserve every applicable inherited notice.

The detailed baseline commands and migration inventories are recorded in
`docs/migration/2026-07-11-phase-0-baseline.md`.
