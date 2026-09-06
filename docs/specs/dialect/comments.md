# Comments

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the `Comment` kind, the HTML-comment rule of the inherited grammar, and
the `%%` comment syntax. Option: `comments` (default `false`) for `%%`; HTML
comments have no option. Sources: CommonMark's HTML comment token and block;
Obsidian's `%%` comments. Executable oracles: cmark for the HTML token and
block boundaries; `@quartz-community/remark-obsidian` for `%%`, whose
stripping is a registered projection. Landing: `M0` for the kind and the HTML
rule, `O3` for `%%`. The `%%` examples in this module run with `comments` on;
the [example format](../dialect.md#examples) is defined by the index.

## Model

```text
Comment(literal: String)
```

`Comment` is a leaf and the one kind that is valid in both block content and
inline content; its parent edge records which, and the node stores no
placement field. `literal` excludes the delimiters and preserves every byte
between them, line endings and indentation included, exactly as written after
container-prefix removal.

Nothing strips a comment. Every recognized comment of either grammar is a
`Comment` node; a consumer that does not want comments drops the nodes. There
is no retention option, and `stripHTMLComments` is removed.

## HTML comments

Under the inherited grammar, with no option, an inline HTML comment token
`<!-- ... -->` is an inline `Comment` whose literal is the bytes between
`<!--` and `-->`:

```````````````````````````````` example
a <!-- b --> c
.
Document scope=1:1..1:14 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:14 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:2 anchor=null attributes={} literal="a " children=0
    ├── Comment scope=1:3..1:12 anchor=null attributes={} literal=" b " children=0
    └── Text scope=1:13..1:14 anchor=null attributes={} literal=" c" children=0
````````````````````````````````

The tokens `<!-->` and `<!--->` are comments with an empty literal:

```````````````````````````````` example
a <!--> b <!---> c
.
Document scope=1:1..1:18 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:18 anchor=null attributes={} children=5
    ├── Text scope=1:1..1:2 anchor=null attributes={} literal="a " children=0
    ├── Comment scope=1:3..1:7 anchor=null attributes={} literal="" children=0
    ├── Text scope=1:8..1:10 anchor=null attributes={} literal=" b " children=0
    ├── Comment scope=1:11..1:16 anchor=null attributes={} literal="" children=0
    └── Text scope=1:17..1:18 anchor=null attributes={} literal=" c" children=0
````````````````````````````````

An HTML block that opens with `<!--` and whose end line holds only whitespace
after the first `-->` is a block `Comment` whose literal is the bytes between
`<!--` and that `-->`, line endings included:

```````````````````````````````` example
<!-- c -->

<!--
multi
-->
.
Document scope=1:1..5:3 anchor=null attributes={} children=2
├── Comment scope=1:1..1:10 anchor=null attributes={} literal=" c " children=0
└── Comment scope=3:1..5:3 anchor=null attributes={} literal="\nmulti\n" children=0
````````````````````````````````

Every other HTML block, including one whose end line carries non-whitespace
after `-->`, stays `HTMLBlock` as written, and every other HTML token stays
`HTML`. The token and block boundaries are the inherited ones; this module
changes only the kind produced.

```````````````````````````````` example
<!-- a --> b
.
Document scope=1:1..1:12 anchor=null attributes={} children=1
└── HTMLBlock scope=1:1..1:12 anchor=null attributes={} literal="<!-- a --> b\n" children=0
````````````````````````````````

## `%%` comments

With `comments=true`, the opener is the first two `%` of a run of percent
signs that is not preceded by an unescaped backslash, and the body ends at
the first later `%%`. Inline recognition is inline step A5.

```````````````````````````````` example comments
a %%hidden%% b
.
Document scope=1:1..1:14 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:14 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:2 anchor=null attributes={} literal="a " children=0
    ├── Comment scope=1:3..1:12 anchor=null attributes={} literal="hidden" children=0
    └── Text scope=1:13..1:14 anchor=null attributes={} literal=" b" children=0
````````````````````````````````

`%%%%` is an empty comment; `%%%a%%%` is `Comment("%a")` followed by text
`%`; `\%%` is text. Backslashes inside the body are ordinary bytes.

```````````````````````````````` example comments
%%%% %%%a%%%

\%%a%%
.
Document scope=1:1..3:6 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:12 anchor=null attributes={} children=4
│   ├── Comment scope=1:1..1:4 anchor=null attributes={} literal="" children=0
│   ├── Text scope=1:5..1:5 anchor=null attributes={} literal=" " children=0
│   ├── Comment scope=1:6..1:11 anchor=null attributes={} literal="%a" children=0
│   └── Text scope=1:12..1:12 anchor=null attributes={} literal="%" children=0
└── Paragraph scope=3:1..3:6 anchor=null attributes={} children=1
    └── Text scope=3:1..3:6 anchor=null attributes={} literal="%%a%%" children=0
````````````````````````````````

An inline body may span the lines of one inline container:

```````````````````````````````` example comments
a %%x
y%% b
.
Document scope=1:1..2:5 anchor=null attributes={} children=1
└── Paragraph scope=1:1..2:5 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:2 anchor=null attributes={} literal="a " children=0
    ├── Comment scope=1:3..2:3 anchor=null attributes={} literal="x\ny" children=0
    └── Text scope=2:4..2:5 anchor=null attributes={} literal=" b" children=0
````````````````````````````````

### Block form

Step 4 of the block-start order: after container prefixes, a line whose
content is `%%` at indentation zero to three followed only by spaces or tabs
opens a candidate. The candidate scans forward, interpreting no block syntax,
for the first later line whose content is exactly `%%` under the same
prefixes and indentation bound; intervening lines carry the prefixes and may
be blank. If found, the candidate commits as a block `Comment` whose literal
is the intervening lines after prefix removal, each with its line ending:

```````````````````````````````` example comments
%%
hidden
block
%%
.
Document scope=1:1..4:2 anchor=null attributes={} children=1
└── Comment scope=1:1..4:2 anchor=null attributes={} literal="hidden\nblock\n" children=0
````````````````````````````````

The rule applies inside any container:

```````````````````````````````` example comments
> %%
> x
> %%
.
Document scope=1:1..3:4 anchor=null attributes={} children=1
└── Callout scope=1:1..3:4 anchor=null attributes={} variant=null fold=none children=1
    └── Comment scope=1:3..3:4 anchor=null attributes={} literal="x\n" children=0
````````````````````````````````

Without a closer line, the opener line is paragraph text and the inline rule
applies to it; an unmatched inline opener is text and hides nothing:

```````````````````````````````` example comments
%%
text
.
Document scope=1:1..2:4 anchor=null attributes={} children=1
└── Paragraph scope=1:1..2:4 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:2 anchor=null attributes={} literal="%%" children=0
    ├── SoftBreak scope=1:3..1:3 anchor=null attributes={} children=0
    └── Text scope=2:1..2:4 anchor=null attributes={} literal="text" children=0
````````````````````````````````

A block candidate may interrupt a paragraph:

```````````````````````````````` example comments
para
%%
c
%%
.
Document scope=1:1..4:2 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:4 anchor=null attributes={} literal="para" children=0
└── Comment scope=2:1..4:2 anchor=null attributes={} literal="c\n" children=0
````````````````````````````````

An inline body cannot cross a block boundary. Here the opener line finds no
closer line, so it is paragraph text; the heading is a heading; and the
`%%` on the last line is unmatched text:

```````````````````````````````` example comments
%%
# h
end %% x
.
Document scope=1:1..3:8 anchor=null attributes={} children=3
├── Paragraph scope=1:1..1:2 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:2 anchor=null attributes={} literal="%%" children=0
├── Heading scope=2:1..2:3 anchor=null attributes={} level=1 children=1
│   └── Text scope=2:3..2:3 anchor=null attributes={} literal="h" children=0
└── Paragraph scope=3:1..3:8 anchor=null attributes={} children=1
    └── Text scope=3:1..3:8 anchor=null attributes={} literal="end %% x" children=0
````````````````````````````````

A comment suppresses all recognition, inherited and extension, until its
closer. Code spans, HTML tokens, and formulas are earlier class-A steps, so a
`%%` inside them is their byte:

```````````````````````````````` example comments cross_links
%% *a* [[b]] `c` %% `%%`
.
Document scope=1:1..1:24 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:24 anchor=null attributes={} children=3
    ├── Comment scope=1:1..1:19 anchor=null attributes={} literal=" *a* [[b]] `c` " children=0
    ├── Text scope=1:20..1:20 anchor=null attributes={} literal=" " children=0
    └── Code scope=1:21..1:24 anchor=null attributes={} literal="%%" children=0
````````````````````````````````

Table boundary scanning does not recognize comments: a `|` inside `%%...%%`
on a table row splits the cell and the unmatched `%%` bytes are text, and a
`\|` inside a comment in a table cell becomes `|` in the literal.

```````````````````````````````` example comments
| a | b |
| - | - |
| c %% | d %% |
.
Document scope=1:1..3:15 anchor=null attributes={} children=1
└── Table scope=1:1..3:15 anchor=null attributes={} columns=[none:null,none:null] children=2
    ├── TableHead children=1
    │   └── TableRow scope=1:1..1:9 anchor=null attributes={} children=2
    │       ├── TableCell scope=1:2..1:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
    │       └── TableCell scope=1:6..1:8 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=1:7..1:7 anchor=null attributes={} literal="b" children=0
    ├── TableBody children=1
    │   └── TableRow scope=3:1..3:15 anchor=null attributes={} children=2
    │       ├── TableCell scope=3:2..3:7 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=3:3..3:6 anchor=null attributes={} literal="c %%" children=0
    │       └── TableCell scope=3:9..3:14 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=3:10..3:13 anchor=null attributes={} literal="d %%" children=0
    └── TableFoot children=0
````````````````````````````````

## Option behavior

With `comments=false`, `%%` is ordinary text everywhere, and HTML comments
are still `Comment` nodes:

```````````````````````````````` example
%%a%%
.
Document scope=1:1..1:5 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:5 anchor=null attributes={} children=1
    └── Text scope=1:1..1:5 anchor=null attributes={} literal="%%a%%" children=0
````````````````````````````````

Every module that says "comment" means a `Comment` node of either grammar.

## Scopes

An inline `Comment.scope` covers both delimiters and the body. A block
`Comment.scope` covers the opener line through the closer line. An HTML
comment's scope is the inherited token or block range.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover adjacent
and escaped comments, runs of four or more percent signs, Markdown-looking
and extension-looking bodies for every merged feature, a comment inside
HTML, an HTML comment beside a `%%` comment, the callout-title and
mark-content compositions named by those modules, `\|` inside a comment in a
cell, exact literals and scopes, allocation failure, and size-doubling
percent runs.
