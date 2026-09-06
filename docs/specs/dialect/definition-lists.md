# Definition lists

Status: normative module of the [Markdown Core dialect](../dialect.md).
Source: Pandoc's `definition_lists`, including its compact form. Executable
oracle: the Pandoc 3.11 CLI under `specs/oracles/pandoc/`. Landing: `P10`.
The [example format](../dialect.md#examples) is defined by the index.

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
term's several bodies. In the canonical dump the term prints as a
`DefinitionTerm` group and each body as a `DefinitionBody` group;
`Definition.children` counts the bodies. `compact` belongs to the
`Definition`: the boundary between the term and its first body decides one
value for all of that term's bodies, and paragraphs inside bodies are
ordinary `Paragraph` nodes.

```````````````````````````````` example
Term
: definition
.
Document scope=1:1..2:12 anchor=null attributes={} children=1
└── DefinitionList scope=1:1..2:12 anchor=null attributes={} children=1
    └── Definition scope=1:1..2:12 anchor=null attributes={} compact=true children=1
        ├── DefinitionTerm children=1
        │   └── Text scope=1:1..1:4 anchor=null attributes={} literal="Term" children=0
        └── DefinitionBody children=1
            └── Paragraph scope=2:3..2:12 anchor=null attributes={} children=1
                └── Text scope=2:3..2:12 anchor=null attributes={} literal="definition" children=0
````````````````````````````````

```````````````````````````````` example
Term

: definition
.
Document scope=1:1..3:12 anchor=null attributes={} children=1
└── DefinitionList scope=1:1..3:12 anchor=null attributes={} children=1
    └── Definition scope=1:1..3:12 anchor=null attributes={} compact=false children=1
        ├── DefinitionTerm children=1
        │   └── Text scope=1:1..1:4 anchor=null attributes={} literal="Term" children=0
        └── DefinitionBody children=1
            └── Paragraph scope=3:3..3:12 anchor=null attributes={} children=1
                └── Text scope=3:3..3:12 anchor=null attributes={} literal="definition" children=0
````````````````````````````````

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

A term may have several bodies, and a blank line is required before a new
term:

```````````````````````````````` example
T1
: one
: two

T2
: d2
.
Document scope=1:1..6:4 anchor=null attributes={} children=1
└── DefinitionList scope=1:1..6:4 anchor=null attributes={} children=2
    ├── Definition scope=1:1..3:5 anchor=null attributes={} compact=true children=2
    │   ├── DefinitionTerm children=1
    │   │   └── Text scope=1:1..1:2 anchor=null attributes={} literal="T1" children=0
    │   ├── DefinitionBody children=1
    │   │   └── Paragraph scope=2:3..2:5 anchor=null attributes={} children=1
    │   │       └── Text scope=2:3..2:5 anchor=null attributes={} literal="one" children=0
    │   └── DefinitionBody children=1
    │       └── Paragraph scope=3:3..3:5 anchor=null attributes={} children=1
    │           └── Text scope=3:3..3:5 anchor=null attributes={} literal="two" children=0
    └── Definition scope=5:1..6:4 anchor=null attributes={} compact=true children=1
        ├── DefinitionTerm children=1
        │   └── Text scope=5:1..5:2 anchor=null attributes={} literal="T2" children=0
        └── DefinitionBody children=1
            └── Paragraph scope=6:3..6:4 anchor=null attributes={} children=1
                └── Text scope=6:3..6:4 anchor=null attributes={} literal="d2" children=0
````````````````````````````````

After the required padding column the marker scanner consumes up to three
further columns when doing so reaches a non-whitespace scalar or the line
ending, so one through four columns after the marker are padding; four or
more remaining columns begin the body and may produce an indented code block.
Tabs are expanded before this decision. A marker followed only by whitespace
is the marker-only form, whose first content comes from a continuation:

```````````````````````````````` example
Term
~
    body
.
Document scope=1:1..3:8 anchor=null attributes={} children=1
└── DefinitionList scope=1:1..3:8 anchor=null attributes={} children=1
    └── Definition scope=1:1..3:8 anchor=null attributes={} compact=true children=1
        ├── DefinitionTerm children=1
        │   └── Text scope=1:1..1:4 anchor=null attributes={} literal="Term" children=0
        └── DefinitionBody children=1
            └── Paragraph scope=3:5..3:8 anchor=null attributes={} children=1
                └── Text scope=3:5..3:8 anchor=null attributes={} literal="body" children=0
````````````````````````````````

A term line is admissible only where the inherited parser would open a
paragraph: it is never a line a higher-precedence block start claims, is not
itself a marker line, has at most three columns of indentation, and is not a
line the inherited grammar extracts as a link reference or footnote
definition. It is parsed once as a one-line paragraph with leading and
trailing whitespace removed; a trailing backslash or trailing spaces produce
no `LineBreak`. `term-gap` permits at most one blank line and sets
`compact`: absent gives `true`, present gives `false`.

```````````````````````````````` example
*Term* `code`
: d
.
Document scope=1:1..2:3 anchor=null attributes={} children=1
└── DefinitionList scope=1:1..2:3 anchor=null attributes={} children=1
    └── Definition scope=1:1..2:3 anchor=null attributes={} compact=true children=1
        ├── DefinitionTerm children=3
        │   ├── Emphasis scope=1:1..1:6 anchor=null attributes={} children=1
        │   │   └── Text scope=1:2..1:5 anchor=null attributes={} literal="Term" children=0
        │   ├── Text scope=1:7..1:7 anchor=null attributes={} literal=" " children=0
        │   └── Code scope=1:8..1:13 anchor=null attributes={} literal="code" children=0
        └── DefinitionBody children=1
            └── Paragraph scope=2:3..2:3 anchor=null attributes={} children=1
                └── Text scope=2:3..2:3 anchor=null attributes={} literal="d" children=0
````````````````````````````````

## Recognition

At block-start step 13, after every other enabled block start has declined
the line and before paragraph fallback, the parser performs non-consuming
lookahead for one term line, its optional gap, and one valid marker line, and
commits only after that whole prefix succeeds. A complete table candidate
has precedence, and during lookahead the candidate fails when the candidate
marker line is a caption line whose paragraph is followed by blank lines and
a line that opens a table syntax; only the blank-gap form is affected.

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

A body holds arbitrary block content:

```````````````````````````````` example
Term
: first

  second
.
Document scope=1:1..4:8 anchor=null attributes={} children=1
└── DefinitionList scope=1:1..4:8 anchor=null attributes={} children=1
    └── Definition scope=1:1..4:8 anchor=null attributes={} compact=true children=1
        ├── DefinitionTerm children=1
        │   └── Text scope=1:1..1:4 anchor=null attributes={} literal="Term" children=0
        └── DefinitionBody children=2
            ├── Paragraph scope=2:3..2:7 anchor=null attributes={} children=1
            │   └── Text scope=2:3..2:7 anchor=null attributes={} literal="first" children=0
            └── Paragraph scope=4:3..4:8 anchor=null attributes={} children=1
                └── Text scope=4:3..4:8 anchor=null attributes={} literal="second" children=0
````````````````````````````````

An indented marker inside a body, preceded by an admissible term line of the
body, forms a nested definition list:

```````````````````````````````` example
T
: inner
  : d
.
Document scope=1:1..3:5 anchor=null attributes={} children=1
└── DefinitionList scope=1:1..3:5 anchor=null attributes={} children=1
    └── Definition scope=1:1..3:5 anchor=null attributes={} compact=true children=1
        ├── DefinitionTerm children=1
        │   └── Text scope=1:1..1:1 anchor=null attributes={} literal="T" children=0
        └── DefinitionBody children=1
            └── DefinitionList scope=2:3..3:5 anchor=null attributes={} children=1
                └── Definition scope=2:3..3:5 anchor=null attributes={} compact=true children=1
                    ├── DefinitionTerm children=1
                    │   └── Text scope=2:3..2:7 anchor=null attributes={} literal="inner" children=0
                    └── DefinitionBody children=1
                        └── Paragraph scope=3:5..3:5 anchor=null attributes={} children=1
                            └── Text scope=3:5..3:5 anchor=null attributes={} literal="d" children=0
````````````````````````````````

Lazy continuation applies in compact and loose definitions alike, and a lazy
line is never re-examined as a term:

```````````````````````````````` example
Term1
: d1
Term2
: d2
.
Document scope=1:1..4:4 anchor=null attributes={} children=1
└── DefinitionList scope=1:1..4:4 anchor=null attributes={} children=1
    └── Definition scope=1:1..4:4 anchor=null attributes={} compact=true children=2
        ├── DefinitionTerm children=1
        │   └── Text scope=1:1..1:5 anchor=null attributes={} literal="Term1" children=0
        ├── DefinitionBody children=1
        │   └── Paragraph scope=2:3..3:5 anchor=null attributes={} children=3
        │       ├── Text scope=2:3..2:4 anchor=null attributes={} literal="d1" children=0
        │       ├── SoftBreak scope=2:5..2:5 anchor=null attributes={} children=0
        │       └── Text scope=3:1..3:5 anchor=null attributes={} literal="Term2" children=0
        └── DefinitionBody children=1
            └── Paragraph scope=4:3..4:4 anchor=null attributes={} children=1
                └── Text scope=4:3..4:4 anchor=null attributes={} literal="d2" children=0
````````````````````````````````

A line at the current depth that starts another list item, a fenced-code
opener, or an enclosing container-directive closer ends lazy absorption, and
the block parser decides its owner. A definition list cannot interrupt a
paragraph: a marker line after a paragraph line that is not the candidate term
is paragraph text, while a term after a blank line opens a list:

```````````````````````````````` example
para
more
: x

para

Term
: x
.
Document scope=1:1..8:3 anchor=null attributes={} children=3
├── Paragraph scope=1:1..3:3 anchor=null attributes={} children=5
│   ├── Text scope=1:1..1:4 anchor=null attributes={} literal="para" children=0
│   ├── SoftBreak scope=1:5..1:5 anchor=null attributes={} children=0
│   ├── Text scope=2:1..2:4 anchor=null attributes={} literal="more" children=0
│   ├── SoftBreak scope=2:5..2:5 anchor=null attributes={} children=0
│   └── Text scope=3:1..3:3 anchor=null attributes={} literal=": x" children=0
├── Paragraph scope=5:1..5:4 anchor=null attributes={} children=1
│   └── Text scope=5:1..5:4 anchor=null attributes={} literal="para" children=0
└── DefinitionList scope=7:1..8:3 anchor=null attributes={} children=1
    └── Definition scope=7:1..8:3 anchor=null attributes={} compact=true children=1
        ├── DefinitionTerm children=1
        │   └── Text scope=7:1..7:4 anchor=null attributes={} literal="Term" children=0
        └── DefinitionBody children=1
            └── Paragraph scope=8:3..8:3 anchor=null attributes={} children=1
                └── Text scope=8:3..8:3 anchor=null attributes={} literal="x" children=0
````````````````````````````````

## Fallback

A marker without a preceding admissible term, without
the required padding column or immediate line ending, or with invalid
indentation creates no list, and a term with no complete first body stays
available to the paragraph parser:

```````````````````````````````` example
Term
:x
.
Document scope=1:1..2:2 anchor=null attributes={} children=1
└── Paragraph scope=1:1..2:2 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:4 anchor=null attributes={} literal="Term" children=0
    ├── SoftBreak scope=1:5..1:5 anchor=null attributes={} children=0
    └── Text scope=2:1..2:2 anchor=null attributes={} literal=":x" children=0
````````````````````````````````

After commitment an invalid later marker ends the body or list under the
ordinary block rules; it is never repaired. Block parsing inside bodies uses
the shared nesting engine; the document is never reparsed and candidate terms
are never rescanned.

## Scopes

`DefinitionList.scope` and each `Definition.scope` end at the end of the last
nonblank line of the last body. A `Definition.scope` covers its term, every
marker, the padding, and all bodies; term and body child scopes exclude the
markers.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover zero
through three columns of marker indentation; one through four padding
columns, tabs, and excess padding that becomes indented code; code,
callouts, lists, and tables in bodies; the caption exclusion; same-depth
list, fence, and div boundaries; missing term or body fallback; exact
scopes; allocation failure; nesting limits; and size-doubling term and body
inputs.
