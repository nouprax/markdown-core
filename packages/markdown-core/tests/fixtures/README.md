# C specification fixtures

This directory is the single source of truth for C parser correctness
fixtures. Each tracked `.txt` file uses the CommonMark 32-backtick example
format: Markdown input, a `.` separator, and the reviewed canonical AST dump
expected from the configured parser profile.

The directive and formula fixtures are the product's reviewed extension
requirements. CTest executes their input and expected blocks directly. The
upstream cmark-gfm and remark/micromark parity gates reuse selected **input**
blocks from these same files, but derive the other parser's output
independently; they never treat our stored expected dump as external evidence.

Do not mirror these files under `specs/` or another package. Cross-platform AST
contract cases belong in `specs/canonical-ast/`; external authority pins and
deliberate differences belong in `specs/upstream-parity/` and
`specs/mdast-parity/`; position findings belong in their ledger directories.
`scripts/audit-test-topology.sh` enforces the absence of the former
`specs/oracles/` mirror.

`spec_runner --rewrite` is a maintenance command, not an acceptance mechanism.
Every generated dump change must be reviewed together with the parser or AST
contract change that caused it.
