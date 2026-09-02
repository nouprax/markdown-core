# cmark 0.29.0 to 0.31.2 import audit

Markdown Core's historical engine entered this repository through a cmark-gfm
fork based on cmark 0.29. The CommonMark layer now follows the newest stable
cmark release directly. The audited range is:

- base: cmark `0.29.0`, commit `8daa6b1495124f0b67e6034130e12d7be83e38bd`
- target: cmark `0.31.2`, commit `eec0eeba6d31189fd828314576494566d539b1e3`

“Import” means adopting the parser behavior, safety fix, complexity bound, or
Unicode data in Markdown Core's one-shot parser architecture. It does not mean
resurrecting cmark's renderer, streaming parser API, CLI formats, mutable node
API, or build/install surface. Those are deliberately outside this product.

## Parser semantics

The following upstream changes are present and are covered by the complete
cmark 0.31.2 `spec.txt` parity gate. Changes that were already present in the
inherited fork were re-verified; missing post-fork behavior was ported into the
shared parser rather than added as an alternate mode.

| cmark commit(s) | Behavior audited in Markdown Core |
| --- | --- |
| `58dd044` | reject link destinations with unbalanced unescaped parentheses |
| `77f7e7a` | do not merge consecutive indented code blocks |
| `74e8f63` | skip a UTF-8 BOM at the beginning of input |
| `06e3af5` | treat `textarea` as a type-1 HTML block |
| `055b9ea` | use the specification's whitespace definition |
| `5acc7d4` | do not let type-7 HTML blocks interrupt paragraphs |
| `34250e1` | preserve the final resolution of cmark issue 383 |
| `4efec35` | parse emphasis correctly before links |
| `7e63cfd` | correct unmatched/mixed backtick handling |
| `3dfe48d`, `4470ff3` | updated HTML comment scanning |
| `8be7f66`, `b1d961c` | case-insensitive doctypes and declarations without a space |
| `8bafc33` | track underscore and asterisk delimiter lower bounds separately |
| `cb1cd88`, `7b35d4b` | enforce decimal and hexadecimal numeric-entity digit limits |
| `82969a8` | treat Unicode Symbols like Punctuation for delimiter flanking |
| `e0179b7` | remove `source` and add `search` in the block-tag set |
| `db0da21` | accept lowercase inline HTML declarations |
| `a739d49` | mark the root open while block parsing is active |

## Complexity and memory safety

These changes are parser requirements, not benchmark-derived fast paths. The
implementation keeps one general algorithm and the pathological suite owns
adversarial witnesses.

| cmark commit(s) | Invariant |
| --- | --- |
| `b2378e4` | reference lookup remains correct under hash collisions |
| `78e2b78` | repeated reference links do not duplicate definition payload quadratically |
| `ade396d`, `ed0a4bf`, `8bafc33`, `76cbc2d` | inline and emphasis processing is linear in adversarial delimiter shapes |
| `3253e19`, `6a5126a` | repeated inline HTML/comment prefixes do not cause quadratic rescans |
| `10b56dc` | smart-quote processing does not repeatedly search the same prefix |
| `7606801` | reference-definition parsing avoids the published quadratic case |
| `b69051f` | blank lines inside deeply nested lists do not rescan every open block |
| `53abb8e` | empty buffers never call `memcpy` with a null pointer |

The local suite additionally asserts 10,000-level nested-list cases and a
20,000-level blockquote/emphasis case structurally, so a fake depth cap cannot
make a timing test pass. Allocation failure remains stricter than cmark: every
required allocation is terminal and no fallback AST is accepted.

## Unicode, entities, and generated scanners

| cmark commit(s) | Imported data or implementation |
| --- | --- |
| `b467630`, `c964590`, `9d74662`, `b8886b2` | Unicode classification/case-fold data through Unicode 17.0 |
| `f18095f` | table-driven Unicode case folding |
| `c91ced1` | compact entity lookup table and bounded entity scanner |
| `d8de8c7`, `53c47ed` | non-UTF-8 re2c generation and scanner cleanup |
| `2f31b3c` | valid UTF-8 encoding of U+FFFE and U+FFFF |

The entity runner walks every packed named-entity entry, while the CommonMark
parity corpus covers the externally observable numeric-entity and delimiter
classification changes.

## Reviewed but intentionally superseded

`2734f21` changes cmark's inline source positions. Markdown Core does not copy
that representation because its public AST defines an element's scope to cover
the markup that creates it, including code-span delimiters, and requires raw
HTML's closing byte to belong to the node. `specs/positions/inline-sourcepos.json`
compares every cmark 0.31.2 Code/HTML position and fail-closes the resulting
reviewed differences.

The 2020–2024 node-layout, allocator, parser-construction, and tree-API changes
were reviewed against Markdown Core's independent ownership model. Observable
parser behavior is covered by parity, OOM injection, concurrency, and tree
invariant tests; cmark's mutable/public lifecycle is not imported.

## Deliberately outside the product

Changes whose only effect is in cmark's HTML/XML/CommonMark/LaTeX/man renderers,
renderer escaping or wrapping, renderer CLI flags, man pages, CMake install
exports, shared/static library selection, or the caller-driven
`parser_new/feed/finish` API are not applicable. Importing them would contradict
the parser-only boundary rather than update CommonMark syntax.

Every future stable cmark bump must update the immutable commit in
`deltas.json`, run the complete new upstream spec, review the upstream commit
range into these same categories, and update this audit in the same change.
