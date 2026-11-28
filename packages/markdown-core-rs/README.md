# markdown-core-rs

The Rust core of Markdown Core. It starts from markdown-rs 1.0.0 (see the
repository's UPSTREAM.md) and is being rewritten into this repository's
parser: one dialect, canonical AST output, interfaces that enforce the
tokenizer architecture, and incremental editing.

The C implementation under `packages/markdown-core` and its fixtures are the
oracle for this package. The plan lives with the project notes.
