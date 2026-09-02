# cmark oracle

This oracle owns the CommonMark layer. It pins the newest stable reference
release, currently `cmark 0.31.2`, consumes that same commit's full specification
corpus, and runs Markdown Core with every syntax extension disabled. Moving the
pin therefore moves the reference implementation and the language corpus as one
reviewed change.

`IMPORTS.md` is the corresponding source audit. It classifies the complete
0.29.0-to-0.31.2 parser range into imported semantics, complexity/safety fixes,
Unicode/generated data, reviewed representation differences, and product
surfaces that are intentionally absent.

`cmark-gfm` is deliberately not treated as a CommonMark authority. Its sibling
oracle owns only the GFM/cmark-gfm extension layer. remark/mdast is an
independent corrective and supplementary implementation: agreement can support
a deliberate cmark delta, while disagreement triggers review; it does not
silently replace cmark in the CommonMark scope.

The policy is fail-closed. Every unregistered difference fails, and every
registered input or tree projection must continue to reproduce.

## Running it

```sh
scripts/init-environment.sh --install oracle-cmark
pnpm build:c
pnpm check:commonmark-parity
```
