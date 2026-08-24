# Deprecated documents

**Nothing in this directory is normative.** It is archive.

These documents were moved here wholesale when the engine was reset to its 1.0
baseline (`580d10c`). They describe engines, contracts and programs that either
no longer exist on this branch or have not been rebuilt yet.

Some are still accurate for the baseline engine — the migration phase records,
the toolchain and environment notes, the release notes. Others are actively
false: `specs/canonical-ast.md` still claims that link reference definitions
"are not a difference: both parsers consume them into the reference map and
neither leaves a node behind", which the reconstruction contradicts directly.

The rule is deliberately blunt, because a half-true document is worse than an
archived one. Read [`../RECONSTRUCTION.md`](../RECONSTRUCTION.md) instead; where
the two disagree, that one is right.

A document returns to `docs/` by a commit that names the step which made it true
again.
