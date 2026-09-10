# Insertion oracle

The [insertion module](../../../docs/specs/dialect/insertion.md) owns the
language. This oracle supplies independent executable evidence for I0/I1.

`markdown-it@14.2.0` and `markdown-it-ins@4.0.0` are exact development dependencies.
Their SHA-512 package integrity values are checked against both the lockfile
and the normative module. The plugin is registered with `use`; HTML token
recognition is enabled so token opacity is exercised. Other markdown-it defaults
remain unchanged, including disabled bare-URL linkification.

Run after `pnpm install --frozen-lockfile` and `pnpm build:c`:

```sh
pnpm check:ins-parity
```

The aggregate `check:oracle-parity` and required External parity CI job both run
this gate. Builds and tests never fetch the oracle at runtime. Harness tests
reject missing or unbalanced insertion token pairs, unknown kinds, changed corpora,
duplicate/stale exceptions, changed digests, and exceptions that now agree.

## Corpus provenance

`corpus.json` contains inputs only. Its 57 cases consist of:

- All 16 input groups from the [upstream fixture file](https://github.com/markdown-it/markdown-it-ins/blob/d1a13b290c944e8f212d3a6bd2de2f70b751c924/test/fixtures/ins.txt),
  at the module's pinned commit, preserving each group's line breaks and order.
  The upstream MIT license is retained below. Rendered HTML expectations are
  not copied or used as product goldens.
- All 11 normative module examples, byte for byte.
- 30 composition and failure-boundary inputs: Unicode and punctuation,
  odd runs with both roles, escapes/entities, crossed and nested delimiters,
  CrossLink/CrossEmbedded, Cite, links/media, every opaque context, separate
  inline containers, table unescaping, named inline fields and failed scanners.
  The Unicode cases include astral symbols, astral punctuation and non-ASCII
  whitespace retained at paragraph/heading boundaries.

The complete input list has a reviewed SHA-256 digest in `deltas.json`.
Product AST expectations live separately in
`packages/markdown-core/tests/fixtures/dialect-insertion.txt`; shared binding
expectations live in `specs/canonical-ast/insertion.ast`.

## Upgrade evidence

The 13.0.2 → 14.2.0 upgrade preserves all 54 original projected oracle trees
and all twelve registered divergence digests. Three additional composition
cases exercise the [14.2.0 Unicode fixes](https://github.com/markdown-it/markdown-it/blob/829797aa00353ce0b62ddeb9b4583b837b1ffd9b/CHANGELOG.md):
astral symbol and punctuation flanking, plus paragraph/heading whitespace
preservation. Each changes the old oracle result and agrees with the existing
product under 14.2.0. The upgrade requires no product rule or projection change.

## Comparison and policy

`ins_open`/`ins_close` pairs become nested `Insertion` nodes. The comparison keeps
ordered text, opaque literals, and all surrounding token/container boundaries,
so moving an insertion, changing its content, or crossing another construct
changes the evidence. It omits source scopes and unrelated scalar attributes;
product fixtures verify those separately. Adjacent text tokens are joined and
empty text is omitted. Empty named table arrays have no token counterpart and
are omitted; populated table sections remain in place. The canary requires
exactly one pair for `++inserted++`, tests odd-run placement, nested pairs, and
escaped/code/HTML-attribute opacity before any corpus result is accepted.

All upstream and module cases agree. `baselineGaps` is empty after I1. Twelve
exact composition differences record the product's existing highlights, raw
cross links/embeds, footnotes, comments, formulas, autolinks and named inline
fields, which the pinned oracle does not implement. Each exception names its
input, reason and both semantic digests. An unregistered difference, changed
side, unreachable exception or new agreement fails the gate.

The product has no fixed delimiter nesting limit. Its shared size-doubling
deep-run tests and nested emphasis/strong stress cases verify this behavior
independently of the pinned oracle's own nesting guard.

## Upstream license

Copyright (c) 2014-2015 Vitaly Puzrin, Alex Kocharin.

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
