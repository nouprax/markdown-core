# Adjudication of the original 30 benchmark pairs

This closes the **review** of all 30 former pairs. It does not certify their old
counts as a proof, nor claim that every extension is globally unpairable.
Eighteen entries have reconstructed reference workloads. Twelve entries have
an explicit boundary intervention; some also have a proved restricted core.
The old documents and their 30 drift witnesses remain unchanged. They never enter
the formal median. Forty-two new production domains and the existing recursive
insertion domain enter it under separate contracts.

## The languages and the theorem

The executable specification is `scripts/lib/pair-productions.mjs`. A registration
contains two source templates and three **handwritten syntax-directed semantic
actions**, for Core(dialect), Core(reference spelling), and the pinned oracle.
The actions are not snapshots generated from a parser. They enumerate every node,
ordered child and semantic field, including fixed/default fields. Unknown fields,
extra nodes and changed values fail. Only printer coordinates/child counts are
outside this contract. The XML adapter's added destination, task and row-group
representations are also checked and included in the pairing identity.

For each registration let N be exactly six ASCII digits (including leading zeros).
All occurrences of `{n:6}` in one unit denote the **same** N. Different units may
choose different N, repeat N, or appear in any order, except that the named-graph
domain requires unique N (fresh definitions). The domain is any nonempty
finite sequence of these units; no uniqueness or ascending-index condition is
assumed. Each unit ends in a blank-separated `***` thematic break. The templates
are the complete EBNF terminals, not illustrative examples. Thus this is a proof
about a restricted production language with arbitrary repetition, not about the
whole grammar of any named feature. Six digits bound positional columns; payload
width, escapes, arbitrary Unicode, arbitrary nesting and malformed syntax are
outside these new domains. The separate insertion proof covers arbitrary nesting.

For each registration p define Dp(N), Rp(N) by substituting N into its two
templates. The transformation on a document is:

```text
T(Dp(n1) ... Dp(nk)) = Rp(n1) ... Rp(nk), k >= 1
T^-1(Rp(n1) ... Rp(nk)) = Dp(n1) ... Dp(nk)
```

**Unique recognition.** Six digits cannot contain any grammar delimiter. The
prefix/suffix terminals distinguish every capture from text. Repeated captures
must agree. A `***` unit terminator is outside every closed inline/fence/container
and separated by blank lines; it terminates paragraph/list/table/definition
continuation before the next unit. No admitted payload can create a terminator,
new block opener, delimiter run, escape or entity. Only the named-graph
production introduces the explicit bindings specified below. Greedy scanning of one
unit therefore has a unique endpoint. The independent sticky recognizer checks
all captures, full input consumption, forward transformation and inverse.

**Semantic actions.** The following constructor/field correspondences specify Fp.
Each action produces the same ordered abstract operation with the same N-bearing
payloads. Constants belong to that domain's constructor and are checked, never
silently ignored. For example, `embedded` and `standalone` are separate formula
constructors with separate inverses; two different values are never folded into
one mixed-domain mapping. Named/default syntax choices in one domain must not be
used to justify an inverse on their union. Their costs remain separate report rows.

| Domains (all suffixed `-v1`) | Abstract constructor and full correspondence |
| --- | --- |
| run-insertion, run-mark, run-strike | One span owns `bodyN`; Insertion, Mark or Strikethrough corresponds to Strong, in separate languages. Document/Paragraph/Text and order are fixed. |
| run-super, run-sub | One span owns `bodyN`; Superscript or Subscript corresponds to Emphasis. No whitespace in the span, intraword delimiters or adjacent runs. |
| opaque-comment | Comment and Code are opaque inline leaves with exactly `body N`, inside identical surrounding prose. No percent/backtick, newline or edge padding can occur in the body. |
| opaque-formula, opaque-display | Formula(embedded) or Formula(standalone) maps to Code in separate languages. The `probe` prefix and `end` suffix prevent promotion. The mode constant is checked. |
| leaf-comment | Comment and CodeBlock both retain exactly `body N` plus LF. Closing delimiters occupy their own line; the body cannot close them or parse as children. |
| leaf-formula, leaf-fence, leaf-promotion | FormulaBlock stores `body N`; CodeBlock stores `body N` plus one mandatory LF. Appending/removing that single LF is bijective in this domain. The fixed `formula` info token selects the constructor; the twin has no info/language. Both code flags are closed/fenced. Promotion produces a leaf, not a binding. |
| task-value | Ordered list/item/body ownership is identical. The singleton task value `~` maps to `x`/completed=true. Bullet type, tightness and absent start/variant/delimiter are checked. This does not identify arbitrary task markers with a boolean. |
| record-span | Span record (`key`, `valueN`) maps to Link destination `/key` and title `valueN`; both own Text `body N`. The key is fixed but still checked; `/` is a reversible encoding prefix. Exactly one record is admitted. |
| class-span | Span class `classN` maps to Link destination `/classN`, with no title. Both have empty inline content. Exactly one class is admitted. |
| cross-link, cross-embed | The raw path `targetN` maps to URL `/targetN`; raw label `labelN` maps to title. Neither side has inline children. Embed dimensions and cross anchors are null. |
| cross-link-absent, cross-embed-absent | Same path map with absent label/title, separately from the empty-label domains. |
| cross-link-empty, cross-embed-empty | Same path map with explicit empty label/title. The source grammar and exact Core/XML fields distinguish these from absence. |
| cross-anchor, cross-local | Cross (path, anchor) maps to a URL with a unique `#` separator; the path is respectively `targetN` or empty and the anchor `sectionN`. Label/title is absent. The prefix grammar excludes any second `#`. |
| cite-author, cite-suppress, cite-normal | A singleton unresolved bibliography key `keyN`, with the named fixed mode and empty prefix/suffix groups, maps to an explicit URI autolink `https://keyN`. The link's text is a deterministic copy of the same key encoding, not an additional input field. No definition, resolution or citation affix is admitted. |
| inline-directive | Directive name `note` maps to image destination `/note`; DirectiveLabel owns the same Text `body N` as the image. The slot wrapper adds no semantic operation. |
| empty-directive | The inline directive/image label is empty; N occurs in identical surrounding prose. Empty label ownership and absent fields are checked. |
| leaf-directive | The same name/label map at block scope. DirectiveBlock owns its inline label; the twin's single-image Paragraph is its block wrapper. No heading or derived anchor is involved. |
| anonymous-container, named-container | DirectiveBlock owns one Paragraph with Text `body N`, as does an ordinary quote. Name is respectively null or the fixed constructor tag `note`, in separate languages. No label or nonempty attributes. The empty `{}` spelling is an opener disambiguator, not a member list. |
| alpha-list, upper-list, roman-list, upper-roman-list, default-list, enclosed-default-list, decimal-list | Each domain maps its two-item marker spelling to decimal markers while preserving start (1, 4, or 3), body order and tightness. Alphabet/case/delimiter are fixed constructor tags per domain. The default sequence has two implicit increments; no arbitrary mixed marker language is claimed. |
| loose-definition | One loose Definition has term Text `term N` and one body Paragraph `body N`. A loose one-item list has those two ordered roles as its first and second paragraphs. DefinitionTerm/DefinitionBody versus paragraph/item wrappers are specified, not universally erased. Compact=false is fixed and checked. |
| grid-cell | A single 1x1 grid cell owns two ordered Paragraphs `body N`, `tail N`, corresponding to one loose list item. Exact column positions, relative width 1, empty head/foot, rowspan=colspan=1 and two block boundaries are checked. No K-way cut, span, caption or empty row is admitted. |
| specimen-graph | One unique named Specimen/Footnote definition and one call per unit. Definitions are hoisted in document order; calls remain in their paragraphs. ID `spec-N`, body `Example N.`, absent reset, empty affixes and each edge are preserved. XML checks all ordered content, and exact pinned HTML additionally checks IDs, call/back-reference edges and first-reference ordinals. HTML rendering is an audit oracle, not timed work. |
| simple-matrix | Both tables contain ordered header cells Name/Count and one body row body/N. The positional split maps to pipe delimiters. All columns are explicitly left aligned, no widths, spans 1x1, empty foot. GFM XML emits alignment on the header only; the domain's delimiter row determines it for the column. |

**Recognition of the individual productions.** Span runs are isolated: left
space/right letter at opening, left digit/right space at closing. The run lengths
are exactly the required widths; neither side invokes rule-of-three ambiguity.
Opaque bodies contain no delimiter or whitespace normalization boundary. Literal
blocks are closed, unindented and contain a single nonempty line. Bracket/name/
attribute fields use safe ASCII and have no nested bracket or escaped delimiter.
URI autolinks have an explicit scheme and cannot use the GFM heuristic fallback.
List markers are at column one and followed by a space; subsequent items are in
the same declared category and are terminated before the next unit. Containers
are closed or explicitly quote-prefixed. Definition lookahead sees exactly one
blank-separated body. Grid borders close at the declared columns and the blank
cell line divides two paragraphs; simple tables have a complete delimiter row
and fixed two-column cuts. These rules give precisely the semantic actions above.

**Composition and inverse.** For one unit, unique recognition plus its semantic
action establishes Fp; the field encodings above have explicit inverses and
wrapper roles are fixed by the domain. Concatenation cannot create cross-unit
ownership because of the terminator. In the named-graph domain, maintain a
fresh-key map and an ordered definition list: each unit adds one new definition
and exactly one reference to it, so extending a valid prefix cannot redirect an
earlier edge. Both grammars hoist definitions in the same order here because
definition order and first-reference order coincide. The implicit ordinal is
derived from that order; no explicit reset is admitted. XML omits IDs and edges,
so XML alone cannot pass this contract: the additional HTML graph witness is
mandatory. Other domains contain no named definitions. Induction on k establishes T^-1(T(x))=x and the commuting parse equation for every
finite document in the domain. This is the proof; finite parser executions test
whether the implementations obey it. It does not follow from matching counts.

## Pair-by-pair decisions

`proof-X-*` identifies the newly measured documents for X-v1. The table names
all original pairs without the common `pair-` prefix and `-dialect/common` suffix.
“Reconstruct” certifies the newly specified language, not every byte pattern in
the original mixed sample. Original raw data remain diagnostic. “Boundary” also
retains an unmatched operation, with a counterfactual baseline described below.

| Original | Decision | New proof domains | Reason / remaining operation |
| --- | --- | --- | --- |
| runs | Reconstruct | `run-insertion-v1`, `run-mark-v1`, `run-strike-v1`, `run-super-v1`, `run-sub-v1` | Split the many-to-one substitution into five marker languages; never pool their inverses. |
| comment | Reconstruct | `opaque-comment-v1` | Reconstruct a nonempty, single-line opaque body without padding or delimiter runs. |
| formula | Reconstruct | `opaque-formula-v1`, `opaque-display-v1` | Separate embedded and standalone modes; each has an explicit constructor mapping. |
| anchor | Boundary | — | A block-owned identity is not a free name-to-URL definition; cut the explicit declaration, retaining its host. |
| specimen | Reconstruct | `specimen-graph-v1` | Reconstruct unique labelled definitions/calls; verify full XML structure plus HTML IDs, forward/back edges and first-reference numbering. |
| task | Reconstruct | `task-value-v1` | Map the singleton marker constructors ~ and x and preserve the ordered item body. |
| span | Reconstruct | `record-span-v1`, `class-span-v1` | Use separate one-record and one-class domains; preserve key/value and class fields rather than replacing their keys by c. |
| xlink | Reconstruct | `cross-link-v1`, `cross-link-absent-v1`, `cross-link-empty-v1`, `cross-anchor-v1`, `cross-local-v1` | Split null, empty, populated and anchor forms; preserve target and label through explicit field mappings. |
| citegroup | Boundary | `cite-normal-v1` | The former merged link label loses the prefix/suffix boundary; cut affixes and prove the singleton empty-affix citation. |
| cite | Reconstruct | `cite-author-v1`, `cite-suppress-v1` | Separate citation modes and reconstruct explicit URI autolinks with a reversible key encoding. |
| formulablock | Reconstruct | `leaf-formula-v1` | Make the mandatory code-body terminal newline an explicit reversible representation map. |
| embed | Boundary | `cross-embed-v1`, `cross-embed-absent-v1`, `cross-embed-empty-v1` | Preserve label states; image has no numeric dimension field, so cut dimensions at their suffix boundary. |
| simpletable | Reconstruct | `simple-matrix-v1` | Repair none versus left alignment with explicit :---; preserve ordered header/body cells. |
| blockcomment | Reconstruct | `leaf-comment-v1` | A fenced literal leaf with its final newline maps to the same literal payload in a code block. |
| formulafence | Reconstruct | `leaf-fence-v1` | Use an empty code info field and map the fixed formula fence tag to the leaf constructor. |
| metadataempty | Boundary | — | Discarded unknown members cannot be recovered from an empty field map; remove the envelope and retain the following document. |
| metadata | Boundary | — | Typed scalars, lists and duplicate-key overwrite are absent from a code literal; cut the envelope from the body. |
| tcaption | Boundary | `simple-matrix-v1` | A loose continuation has no table/caption ownership; retain the table and cut only its trailing caption. |
| idirective | Reconstruct | `inline-directive-v1`, `empty-directive-v1` | Repair the spurious destination index: the fixed directive name maps to /note and label inlines remain children. |
| fancylist | Reconstruct | `alpha-list-v1`, `upper-list-v1`, `roman-list-v1`, `upper-roman-list-v1`, `default-list-v1`, `enclosed-default-list-v1`, `decimal-list-v1` | Separate marker alphabets/delimiters and preserve start values (including 3 and 4). |
| formulapromo | Reconstruct | `leaf-promotion-v1` | Replace the annihilated reference definition with a literal code block; both sides now construct the corresponding leaf. |
| specimenstart | Boundary | `specimen-graph-v1` | The explicit sequence reset has no footnote counterpart; remove only the reset digit, retaining the proved definition/call graph. |
| ldirective | Reconstruct | `leaf-directive-v1` | Replace headings and their derived anchors with a one-image paragraph; the block and label wrappers have explicit roles. |
| cdirective | Reconstruct | `anonymous-container-v1`, `named-container-v1` | Split fixed named and anonymous container languages; preserve the owned block sequence. |
| callout | Boundary | — | Three collapsed states and title ownership collapse to one completed task value; remove the callout header metadata within the quote. |
| headless | Boundary | `simple-matrix-v1` | The former first-row text and header role both differ; compare a reconstructed headed matrix and isolate headless syntax by removing table borders. |
| sparsegrid | Boundary | `grid-cell-v1` | The old list reordered h/a/b/c and lost spans, empty rows, caption and foot; isolate geometry with border/cell-marker ablation and prove the one-cell restriction. |
| gridcell | Reconstruct | `grid-cell-v1` | Reconstruct complete fixed-width cells, preserve the two ordered paragraphs, and delimit independent units. |
| caption | Boundary | `simple-matrix-v1` | A caption role is not a header cell role; cut the leading caption while retaining the table's h/v matrix. |
| deflist | Boundary | `loose-definition-v1` | The old list loses per-definition compactness and empty-body identity; prove one loose term/body domain and cut definition ownership for the full mixed workload. |

## Boundaries and what their measurements mean

`pair-review.mjs` defines twelve source interventions. The generator builds the
baseline **from the actual full document at the same scale**, rather than sizing
another document independently. Selection pulls in the original, its historical
reference, applicable proof domains and baseline. The audit checks the exact
transformation and parses both scales. The report prints Full, Without, signed
Delta and Delta per original unit. These interventions can change nodes and byte
length, so Delta includes composition effects; it is not an isolated feature cost,
a Same-job ratio, or a lower bound. In particular, stripping a marker can change
recognition of the remaining text. Negative deltas are retained.

| Boundary | Exact intervention | What remains / interpretation limit |
| --- | --- | --- |
| anchor | Delete the standalone `#anchor-N#` line. | The quote and paragraph remain; declaration/attachment/lookup effects are inside the delta. |
| specimenstart | Replace `(5@spec-N)` by `(@spec-N)`, removing only the explicit reset digit. | IDs, calls, hoisting and body ownership remain. The sequence-reset delta includes its interaction with the otherwise corresponding reference graph. |
| citegroup | Replace `[see @key-N, p. 3]` by `[@key-N]`. | Same key and normal mode; prefix/suffix parsing and their interaction with the group are removed. |
| embed | Remove numeric `100` / `100x200` from the label suffix, retaining the empty label. | Path and null/empty/populated labels remain. Dimension recognition/decoding is the intervention. |
| metadataempty, metadata | Remove the opening envelope through its closing delimiter. | The following Body paragraph remains. This bounds the experiment at the envelope/body seam, not at individual metadata fields; the larger removed byte count is visible. |
| tcaption | Delete the blank-separated `: Caption N` line. | The same pipe table and thematic break remain. Caption lookbehind/adoption and inline work are in the delta. |
| callout | Delete `[!note]` and the optional collapsed marker within each quote. | Quote content remains. Title-to-paragraph ownership also changes, so the delta is not just the enum scanner. |
| headless | Delete the opening and closing segmented border lines. | Every cell's source text remains as prose. Table structure and positional recognition are jointly removed; no claim that a GFM header is a headless row. |
| sparsegrid | Delete border lines, cell bars and the empty `Table:` caption. | Literal row text remains in source order. Geometry, spans, ownership and caption/foot interpretation are jointly removed; this is deliberately broader than a “span-only” price. |
| caption | Delete the `Table: Caption N` line. | Original h/v pipe table remains, unlike the old twin that reassigned caption text to a header cell. |
| deflist | Delete leading colon markers. | Term and body source text remain. Definition grouping/compactness and paragraph recognition interact. |

These failures are **local to the proposed mapping**, not exhaustive impossibility
proofs over CommonMark/GFM. The specimen investigation demonstrates why that
distinction matters: supplementing the XML with HTML IDs/edges made a restricted
graph proof possible and narrowed the reset boundary to one field. Duplicate
definitions, multiple calls, unreferenced definitions and differing definition/
call order remain outside this proof. A code literal
cannot reconstruct discarded metadata keys, one completed boolean cannot invert
three callout states, and a seven-item list without coordinates cannot recover
arbitrary grid spans. Encoding those fields as unparsed text would preserve bytes
but would not match their decoding/binding operations.

The reach audit distinguishes directly compared states, states reached by these
reviewed partitions, pending proposals and separately justified bounds. A state
appearing in an old sample does not become proved merely because a restricted
replacement for that sample exists. All proof sources, actions, adapter mappings,
review decisions and boundary transforms participate in report identity. Do not
compare the old 30-pair median to the 43-domain median as a performance change.
