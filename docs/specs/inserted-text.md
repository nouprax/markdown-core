# Inserted text

Status: normative target contract for the optional `insertedText` parser
extension. Authority: the official
[`markdown-it-ins` README](https://github.com/markdown-it/markdown-it-ins/blob/d1a13b290c944e8f212d3a6bd2de2f70b751c924/README.md),
[`markdown-it-ins` implementation](https://github.com/markdown-it/markdown-it-ins/blob/d1a13b290c944e8f212d3a6bd2de2f70b751c924/index.mjs),
and
[upstream conformance cases](https://github.com/markdown-it/markdown-it-ins/blob/d1a13b290c944e8f212d3a6bd2de2f70b751c924/test/fixtures/ins.txt)
for release 4.0.0 at commit
`d1a13b290c944e8f212d3a6bd2de2f70b751c924`, read on 2026-09-03. The
README owns the valid source form and `<ins>` meaning; the implementation and
cases own delimiter behavior that the README describes by reference to
CommonMark emphasis.

This is an independent syntax extension. It is not Obsidian Flavored Markdown,
and no Obsidian option enables `insertedText`. A caller may
compose both extensions explicitly.

## Consumer semantics

`++content++` represents text inserted into, or added to, a document:

```text
Insert(
  content: [Markup],
  scope: Scope
)
```

`Insert` is an inline `Markup` kind. Its `content` is parsed by the shared
inline engine and may contain any inline construct whose delimiters nest
legally within the insertion. `scope` covers both `++` delimiters and the body.
A matched pair has non-empty source between its delimiter runs. Its final
`content` array may nevertheless be empty if another enabled extension
semantically removes every child in that source region.

The semantic node is named `Insert`, not `Underline`. The plugin's normative
output is HTML `<ins>`; underlining is only the conventional presentation of
that element. A renderer may choose another accessible presentation without
changing the AST. A purely presentational underline, if supported separately,
must not be encoded as `Insert`.

## Delimiter runs

A *plus run* is a maximal sequence of one or more unescaped `+` code points in
the current inline container. A run of one `+` is text. A longer run is split
into two-character delimiter units. Tokenization first applies these rules:

1. If its length is odd, the first `+` remains literal text.
2. The remainder is partitioned from left to right into two-character `++`
   delimiter units.
3. Every unit inherits the opening and closing eligibility of the complete
   source run.

For a complete plus run, let `before` and `after` be the adjacent Unicode code
points outside the run. The beginning and end of the inline container count as
whitespace. Using the shared CommonMark definitions of Unicode whitespace and
punctuation:

```text
leftFlanking =
  after is not whitespace and
  (after is not punctuation or before is whitespace or before is punctuation)

rightFlanking =
  before is not whitespace and
  (before is not punctuation or after is whitespace or after is punctuation)
```

Each unit may open exactly when the run is left-flanking and may close exactly
when it is right-flanking. As with `markdown-it-ins`, intraword opening and
closing are allowed, and the CommonMark emphasis “rule of three” is not applied
to plus runs.

Eligible units enter the shared delimiter stack in source order. A closing
unit matches the nearest legal unmatched opening unit without crossing an
already established inline boundary. Units belonging to the same source run
cannot match one another. Consequently `++++` and `a++++b` are entirely
literal, while separate runs around content can match.

Multiple matching units nest rather than merge: `++++text++++` produces
`Insert(Insert(text))`. For an odd run used as an opener, its unmatched `+`
precedes the opening units. For an odd run used as a closer, the unmatched `+`
follows all consecutive closing units produced by that run. Thus
`+++text+++` produces literal `+`, then `Insert(text)`, then literal `+`.
This closing-side normalization is required even though tokenization initially
places the unmatched character at the start of every odd run.

This algorithm, rather than an exact-two-character lexical rule or a regular
expression, is normative. An empty candidate such as `++++` does not produce
an `Insert`.

## Composition and opacity

The insertion delimiter uses the same inline delimiter machinery and
precedence boundary as emphasis. Properly nested inline markup is parsed in
the `Insert.content` array. Crossed delimiters are not repaired: a plus pair
that would have to cross emphasis, strong, link-label ownership, or another
established delimiter boundary remains literal according to the shared
fallback rules.

For example, `[++link++]()` contains an `Insert` inside a `Link` label. By
contrast, plus delimiters that start inside a link label and finish after its
closing bracket do not form an `Insert`. `++**text**++` is a valid insertion
containing `Strong`; crossed forms such as `**++text**++` do not manufacture a
cross-boundary insertion.

Backslash-escaped plus signs are text and do not join a plus run. Inline code,
comments, and source bytes owned by an HTML token are opaque to insertion
recognition. A matched inline HTML opening and closing tag does not create an
opaque region between the two HTML tokens; ordinary text between them remains
eligible under the inherited Markdown rules.

A soft line break may occur inside a valid insertion. A line ending adjacent
to a candidate delimiter is whitespace for the flanking calculation; there is
no separate multiline prohibition. Delimiter pairing remains local to the
current inline container.

## Option and fallback behavior

With `insertedText=false`, no plus run has delimiter meaning and every source
byte follows inherited Markdown parsing. With `insertedText=true`, an
unmatched or ineligible unit remains literal text. Failed recognition must not
consume escapes, brackets, or plus signs needed by a later valid construct.

The implementation must scan each run once and use the shared delimiter stack;
it may not repeatedly rescan the remaining inline source. Parsing must remain
linear for long plus runs and for many unmatched candidates, use the normal
inline nesting limit, and remain strict under allocation failure.

## Oracle setup

The deterministic syntax oracle is `markdown-it@13.0.2` with
`markdown-it-ins@4.0.0` registered through `use`, matching the parser family
against which the pinned plugin release declares and runs its tests. The npm
release identities are:

```text
markdown-it@13.0.2
  gitHead: e476f78bc3ea3576beb61bdc94322d0a6b2d85cc
  integrity: sha512-FtwnEuuK+2yVU7goGn/MJ0WBZMM9ZPgU9spqlFs7/A/pDIUNSOQZhUgOqYCficIuR2QaFnrt8LHqBWsbTAoI5w==

markdown-it-ins@4.0.0
  gitHead: d1a13b290c944e8f212d3a6bd2de2f70b751c924
  integrity: sha512-sWbjK2DprrkINE4oYDhHdCijGT+MIDhEupjSHLXe5UXeVr5qmVxs/nTUVtgi0Oh/qtF+QKV0tNWDhQBEPxiMew==
```

Conformance compares placement and nesting of `ins_open`/`ins_close` tokens,
not complete rendered HTML. Markdown Core maps each matched token pair to one
`Insert`; inherited Markdown and other extension nodes remain owned by their
respective contracts. A canary must require `++inserted++` to contain exactly
one matched `ins` pair before any oracle result is accepted.

## Required conformance cases

Tests must replay the pinned upstream `markdown-it-ins` cases and additionally
cover the consumer contract. Coverage must include:

- plain, adjacent, nested, and intraword insertions;
- one-, two-, three-, four-, odd-, and even-length plus runs, including the
  literal `++++` and `a++++b` cases;
- whitespace, punctuation, Unicode punctuation/whitespace, and soft-line-break
  flanking;
- nested and crossed emphasis, strong, strikethrough, links, `CrossLink`,
  `Mark`, `Cite`, and `Insert` delimiters;
- escaped, unmatched, and option-disabled input;
- inline code, comments, individual HTML tokens, and text between paired HTML
  tags;
- exact `Insert.content` order and delimiter-inclusive `scope` values;
- allocation failure, the configured nesting limit, long unmatched sequences,
  and size-doubling plus runs.

The `markdown-it-ins` result is the syntax oracle. Oracle tokens are mapped to
the semantic `Insert` node above; renderer-specific HTML strings and token
implementation fields are not part of the Markdown Core AST.
