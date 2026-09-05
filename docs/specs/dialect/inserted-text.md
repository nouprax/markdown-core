# Inserted text

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `insertedText` (default `false`). Source: `markdown-it-ins` 4.0.0 at
commit `d1a13b290c944e8f212d3a6bd2de2f70b751c924`, whose README owns the
valid source form and `<ins>` meaning. Executable oracle: `markdown-it`
13.0.2 with the plugin registered, under `specs/oracles/markdown-it-ins/`,
which lands with `I0`. Landing: `I1`.

## Model

```text
Insert(content: [Markup])
```

`Insert` is an inline kind: `++content++` is text inserted into, or added
to, a document. Its content is parsed by the shared inline parser and may
contain any inline construct whose delimiters nest legally inside it. The
node is `Insert`, not `Underline`: underlining is one presentation of `<ins>`,
and a purely presentational underline, if ever supported, is not encoded as
`Insert`. A matched pair has non-empty source between its delimiter runs; its
final content may nevertheless be empty when another feature semantically
removes every child in that region.

## Delimiter runs

A plus run is a maximal sequence of one or more unescaped `+` scalars in one
inline container. A run of one `+` is text. A longer run is tokenized:

1. If its length is odd, one `+` is literal.
2. The remainder is partitioned from left to right into two-character `++`
   units.
3. Every unit inherits the opening and closing eligibility of the complete
   run.

For a complete run let `before` and `after` be the adjacent scalars outside
the run; the beginning and end of the inline container count as whitespace.
With the CommonMark 0.31.2 definitions of Unicode whitespace and punctuation:

```text
leftFlanking  = after is not whitespace and
                (after is not punctuation or before is whitespace or
                 before is punctuation)
rightFlanking = before is not whitespace and
                (before is not punctuation or after is whitespace or
                 after is punctuation)
```

A unit may open exactly when the run is left-flanking and close exactly when
it is right-flanking. Intraword opening and closing are allowed, and the rule
of three is not applied. Eligible units enter the shared delimiter stack at
inline step C6 in source order. A unit that can both open and close is first
tried as a closer against the nearest legal unmatched opener; if none matches
it stays on the stack as a potential opener. A closer never crosses an already
established inline boundary, and units of the same run cannot match one
another, so `++++` and `a++++b` are entirely literal while separate runs
around content match.

Multiple matching units nest rather than merge: `++++text++++` is
`Insert(Insert(text))`. The literal `+` of an odd run is placed after all of
that run's closing units and before all of its opening units, so
`+++text+++` is literal `+`, `Insert(text)`, literal `+`. Units that match
nothing are text and merge with adjacent text. An empty candidate produces no
`Insert`.

## Composition and opacity

Plus delimiters use the same machinery and precedence boundary as emphasis.
Properly nested markup is parsed into the content; crossed delimiters are not
repaired, so `[++link++]()` holds an `Insert` inside the link, `++**text**++`
holds `Strong`, and `**++text**++` forms no insertion across the strong
boundary. Backslash-escaped plus signs are text and join no run. Code spans,
comments, HTML tokens, formulas, cross links, and autolinks are opaque; text
between paired HTML tags is eligible. A soft line break may occur inside an
insertion, and a line ending beside a candidate is whitespace for the
flanking tests. Pairing is local to the current inline container.

## Option behavior and fallback

With `insertedText=false`, no plus run has delimiter meaning. With the option
on, an unmatched or ineligible unit is text, and failed recognition consumes
no escape, bracket, or plus sign a later construct needs. Each run is scanned
once, parsing stays linear for long runs and many unmatched candidates, the
inline nesting limit applies, and allocation failure follows the shared rule.

## Oracle

The deterministic syntax oracle is `markdown-it@13.0.2` with
`markdown-it-ins@4.0.0` registered through `use`:

```text
markdown-it@13.0.2
  gitHead: e476f78bc3ea3576beb61bdc94322d0a6b2d85cc
  integrity: sha512-FtwnEuuK+2yVU7goGn/MJ0WBZMM9ZPgU9spqlFs7/A/pDIUNSOQZhUgOqYCficIuR2QaFnrt8LHqBWsbTAoI5w==

markdown-it-ins@4.0.0
  gitHead: d1a13b290c944e8f212d3a6bd2de2f70b751c924
  integrity: sha512-sWbjK2DprrkINE4oYDhHdCijGT+MIDhEupjSHLXe5UXeVr5qmVxs/nTUVtgi0Oh/qtF+QKV0tNWDhQBEPxiMew==
```

The comparison maps each matched `ins_open`/`ins_close` pair to one `Insert`
and compares placement and nesting, not rendered HTML. A canary requires
`++inserted++` to contain exactly one matched pair before any result is
accepted. The rules above are normative; a disagreement with the plugin is a
registered delta.

## Scopes

`Insert.scope` covers both delimiter runs and the body.

## Required conformance cases

Tests replay the pinned upstream cases and cover plain, adjacent, nested, and
intraword insertions; one- through four-character, odd, and even runs
including `++++` and `a++++b`; whitespace, punctuation, Unicode, and
soft-line-break flanking; nested and crossed emphasis, strong, strikethrough,
links, cross links, marks, cites, and insertions; escaped, unmatched, and
option-disabled input; every opaque context and text between paired tags;
exact content order and scopes; allocation failure; the nesting limit; long
unmatched sequences; and size-doubling plus runs.
