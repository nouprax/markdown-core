# Pandoc definition lists

Status: normative target module for `definition_lists` and its compact source
form. Authority:
[Pandoc User's Guide — definition lists](https://pandoc.org/MANUAL.html#extension-definition_lists)
at the snapshot pinned by the [Pandoc extension index](../pandoc.md).

## Consumer model

```text
DefinitionList(
  definitions: [Definition],
  scope: Scope
)

Definition(
  term: [Markup],
  content: [[Markup]],
  compact: Bool,
  scope: Scope
)
```

`DefinitionList.definitions` and every `Definition.content` outer collection
are non-empty. A `Definition` is the complete consumer-visible association
between one term and its definitions; there is no intermediate item node. Its
term is inline content. Each inner `content` collection is one definition body
and is arbitrary block content, so it may contain multiple paragraphs, code
blocks, lists, tables, or other enabled blocks. The outer collection preserves
the distinction and source order when one term has multiple definition bodies.

`compact` belongs to the `Definition`, not the whole list or an individual
body: Pandoc permits compact and loose term-definition associations in the same
list, but the term-to-first-body boundary determines one value for all bodies
of that term. It controls presentation of paragraph blocks without replacing
them with a second `Plain` AST kind.

## Recognition grammar

The following line grammar defines recognition. `EOL` is one physical line
ending, `BLANK` is a line containing only spaces or tabs followed by `EOL`, and
`LINE` excludes its ending. `NONINDENT` is zero through three leading space
columns under the inherited four-column tab stops; a leading tab is not
`NONINDENT`. Repetition of definition bodies and definitions is subject to the
ownership and continuation rules below.

```text
definition-list     ::= definition (definition-separator definition)*
definition          ::= term-line term-gap definition-body+
term-line           ::= NONBLANK_LINE EOL
term-gap            ::= ε | BLANK
definition-body     ::= marker-line body-continuation*
marker-line         ::= NONINDENT (":" | "~") marker-tail
marker-tail         ::= EOL | marker-padding LINE EOL
marker-padding      ::= one indentation column
definition-separator ::= one-or-more BLANKs
```

After the required `marker-padding` column, the marker scanner also consumes
up to three further indentation columns when doing so reaches a non-whitespace
character or `EOL`. Thus one through four columns after the marker are list
padding. If four or more columns remain after the first required column, those
remaining columns begin the body instead and can produce an indented code
block. Tabs are expanded by the inherited tab-stop rules before this decision.
The marker may end at `EOL`; its first content then comes from a continuation.

`term-line` is parsed as inline markup exactly once and cannot continue onto a
second physical line. `term-gap` permits at most one blank line. The grammar's
`definition-separator` is not consumed eagerly: a blank run ends the current
body, and the following unindented line becomes a new term only if lookahead
finds its complete `term-gap definition-body` suffix. Otherwise the definition
list ends before that line.

## Parsing intent

At the beginning of a block candidate, with `definition_lists=true`, the
parser performs non-consuming lookahead for one `term-line`, its optional
single `term-gap`, and a valid `marker-line`. It commits only after that entire
prefix succeeds. A complete table candidate has higher block precedence;
specifically, a definition-list lookahead across `term-gap` must not claim a
line that the table rule owns as its caption.

After commitment, parsing proceeds as follows:

1. Parse the one-line term with the inline parser and set `compact` from the
   absence or presence of `term-gap`.
2. Extract one or more bodies with the shared list-item scanner. The first
   body's continuation indent is the distance from the current container's
   start to the position immediately after marker padding, or four columns
   when `four_space_rule` is enabled.
3. Feed the lines owned by each body directly to the ordinary block-container
   parser and append its result as one inner `Definition.content` collection.
   Do not materialize and reparse a body substring. A following definition
   marker at the same container depth closes the current body and opens another
   for the same term. An indented marker is ordinary nested body content when
   the enclosing continuation rules own it.
4. After a separating blank run, tentatively recognize another complete
   definition. Success appends it to the same `DefinitionList`; failure closes
   the list without consuming the candidate line.

Within a body, a nonblank line indented to the continuation column starts or
continues block content. Paragraph continuation may omit that indentation only
where the inherited list-item lazy-continuation rule permits it. A blank line
followed by content indented to the continuation column remains in the body,
allowing multiple paragraphs, code blocks, block quotes, tables, and nested
lists. At the current container depth, another list marker, a fenced-code
opener, an enclosing HTML close tag, or an enclosing fenced-div close prevents
the raw line scanner from absorbing that line; the ordinary block parser then
decides its owner.

Recognition is integrated before paragraph fallback. It does not reinterpret
a completed paragraph or group already-produced nodes. Term lookahead is
bounded to the candidate prefix; body ownership and nested block parsing share
the main block-parser cursor so every source region is scanned a constant
number of times.

## Compact definitions

`compact_definition_lists` is not an extension in Pandoc 3.11 and therefore is
not a Markdown Core parser option. Compact syntax is part of
`definition_lists`: omitting the blank line between term and first definition
body sets `Definition.compact=true`; including it sets `false`. Every
paragraph-like block in every body retains normal paragraph content while
consumers use the flag to select compact spacing.

This single gate and per-definition fact exactly represent Pandoc's `Plain`
versus `Para` distinction without creating two parsing algorithms or a
document-wide tightness flag.

## Fallback and scopes

A marker without a preceding one-line term, without the required following
indentation column or immediate `EOL`, or with invalid leading indentation does
not create a definition list. A term with no complete first body remains
available to the inherited paragraph parser. After commitment, an invalid
later marker ends the current body or list according to the ordinary block
boundary rules; it is not repaired into a definition marker.

`DefinitionList.scope` covers all definitions. Each `Definition.scope` covers
its term, every definition marker, indentation, and all bodies. Term and body
child scopes exclude definition markers.

Block parsing inside definitions uses the shared nesting/indentation engine.
It must not reparse the document or repeatedly rescan candidate terms.

## Required conformance cases

Tests must cover inline-formatted terms; colon/tilde markers; zero-, one-, two-,
and three-space marker indentation; one-through-four-column marker padding,
tabs, marker-only lines, and excess padding that becomes indented code; compact
and loose definitions in one list; multiple definition bodies per term and
their ordering; lazy and indented continuation; multiple paragraphs, code,
block quotes, lists, tables, and nested definition lists; required definition
separation; table-caption precedence; same-depth list and fence boundaries;
missing space/term/definition fallback; exact scopes; option-off behavior;
allocation failure; nesting limits; and size-doubling term/definition inputs.
