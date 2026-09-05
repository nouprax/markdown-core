# Definition lists

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `definitionLists` (default `false`). Source: Pandoc's
`definition_lists`, including its compact form. Executable oracle: the Pandoc
3.11 CLI under `specs/oracles/pandoc/`. Landing: `P10`.

## Model

```text
DefinitionList(definitions: [Definition])
Definition(term: [Markup], content: [[Markup]], compact: Bool)
```

Both are block `Markup` kinds. `definitions` and every outer `content`
collection are non-empty. A `Definition` is the complete association between
one term and its definition bodies; there is no intermediate item node. The
term is inline content. Each inner collection is one definition body of
arbitrary block content, and the outer collection preserves the order of a
term's several bodies. In the canonical dump each body prints as a nested
`DefinitionBody` line. `compact` belongs to the `Definition`: the boundary
between the term and its first body decides one value for all of that term's
bodies, and paragraphs inside bodies are ordinary `Paragraph` nodes.

## Grammar

`EOL` is one line ending, `BLANK` a line of only spaces and tabs, `LINE` a
line without its ending, and `NONINDENT` zero through three leading space
columns under the inherited four-column tab stops; a leading tab is not
`NONINDENT`.

```text
definition-list      = definition *( definition-separator definition )
definition           = term-line term-gap 1*definition-body
term-line            = NONBLANK-LINE EOL
term-gap             = "" / BLANK
definition-body      = marker-line *body-continuation
marker-line          = NONINDENT ( ":" / "~" ) marker-tail
marker-tail          = EOL / marker-padding LINE EOL
marker-padding       = one indentation column
definition-separator = 1*BLANK
```

After the required padding column the marker scanner consumes up to three
further columns when doing so reaches a non-whitespace scalar or the line
ending, so one through four columns after the marker are padding; four or
more remaining columns begin the body and may produce an indented code block.
Tabs are expanded before this decision. A marker followed only by whitespace
is the marker-only form, whose first content comes from a continuation.

A term line is admissible only where the inherited parser would open a
paragraph: it is never a line a higher-precedence block start claims, is not
itself a marker line, has at most three columns of indentation, and is not a
line the inherited grammar extracts as a link reference or footnote
definition. It is parsed once as a one-line paragraph with leading and
trailing whitespace removed; a trailing backslash or trailing spaces produce
no `LineBreak`. `term-gap` permits at most one blank line and sets
`compact`: absent gives `true`, present gives `false`.

## Recognition

At block-start step 14, after every other enabled block start has declined
the line and before paragraph fallback, the parser performs non-consuming
lookahead for one term line, its optional gap, and one valid marker line, and
commits only after that whole prefix succeeds. A complete table candidate
has precedence, and during lookahead the candidate fails when `tableCaptions`
and at least one table option are on and the candidate marker line is a
caption line whose paragraph is followed by blank lines and a line that
opens an enabled table syntax; only the blank-gap form is affected. A
definition list cannot interrupt a paragraph: a marker line after a paragraph
line that is not the candidate term is paragraph text.

After commitment:

1. The term is parsed and `compact` set.
2. Bodies are extracted with the shared list-item scanner. A body's
   continuation column is the distance from the enclosing container's content
   start to the position after the marker padding. A body continuation is a
   nonblank line indented to that column, a blank line followed by such a
   line, or a lazy paragraph-continuation line; blank lines between two marker
   lines of the same term belong to the definition.
3. The lines each body owns are fed in place to the ordinary block parser
   and appended as one inner collection; no substring is materialized and
   reparsed. A marker line at the same container depth, measured from the
   enclosing container's content start, closes the current body and opens
   another for the same term; an indented marker is nested body content when
   the continuation rules own it.
4. After a separating blank run, another complete definition is recognized
   tentatively. Success appends it; failure closes the list without consuming
   the line.

A line at the current depth that starts another list item, a fenced-code
opener, or an enclosing fenced-div closer ends lazy absorption, and the block
parser decides its owner. Lazy continuation applies in compact and loose
definitions alike, and a lazy line is never re-examined as a term: `Term1`,
`: d1`, `Term2`, `: d2` is one term whose first body paragraph is `d1 Term2`
and whose second body is `d2`. A blank line is required before a new term.

## Option behavior and fallback

With `definitionLists=false`, every line above is paragraph text under the
inherited grammar. With the option on, a marker without a preceding admissible
term, without the required padding column or immediate line ending, or with
invalid indentation creates no list, and a term with no complete first body
stays available to the paragraph parser. After commitment an invalid later
marker ends the body or list under the ordinary block rules; it is never
repaired. Block parsing inside bodies uses the shared nesting engine; the
document is never reparsed and candidate terms are never rescanned.

## Scopes

`DefinitionList.scope` and each `Definition.scope` end at the end of the last
nonblank line of the last body. A `Definition.scope` covers its term, every
marker, the padding, and all bodies; term and body child scopes exclude the
markers.

## Required conformance cases

Tests cover inline-formatted terms; colon and tilde markers; zero through
three columns of marker indentation; one through four padding columns, tabs,
marker-only lines, and excess padding that becomes indented code; compact and
loose definitions in one list; multiple bodies per term and their order; lazy
and indented continuation; multiple paragraphs, code, callouts, lists, tables,
and nested definition lists in bodies; required separation; the caption
exclusion; same-depth list, fence, and div boundaries; a marker after a
paragraph; missing term, padding, or body fallback; exact scopes; option-off
output; allocation failure; nesting limits; and size-doubling term and body
inputs.
