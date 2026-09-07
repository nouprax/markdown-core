# Specimens

This module owns numbered specimen definitions and their citation references.
Its upstream coverage is Pandoc's `example_lists` extension (the upstream name
is retained only for oracle configuration). The public model is available;
source recognition and the cases below land with `P9b`.

```text
Document(content: [Markup], footnotes: [Footnote], specimens: [Specimen])
Specimen(id: String?, start: Int?, content: [Markup], scope)
CitationReferent = bib(key: String, mode: BibMode) | footnote(id: String) | specimen(id: String)
```

Definitions follow the same ownership boundary as footnotes. They are scoped
values owned by `Document.specimens`, not `List`, `ListItem`, or inline nodes.
They leave no implicit reference in their containing content. The document
retains every definition, including anonymous, duplicate, and unreferenced
ones, in scope-start order across all containers, including footnotes.
Walks and dumps visit content, footnotes, then specimens. Only content counts
as the document's children. A definition's body remains block content.

## Definitions and numbering

Definition markers are `(@)`, `(@label)`, `(N@)`, and `(N@label)`, followed by
marker whitespace. They cannot interrupt a paragraph. Unlike alphabetic and
Roman list markers, they are exempt from restrictions on nested list starts.
A body's continuation column is the container start plus four expanded
columns, independent of marker width. Adjacent definitions at the same
container level form one source group for reset recognition; the group is
not an AST value. An intervening block outside the preceding definition body starts a new
group; blank lines alone do not split a group, and an indented continuation
remains part of the preceding body.

One document-wide counter starts at 1 and advances once per definition,
including duplicates and anonymous definitions. On the first definition of a
source group, an explicit `N` resets it before numbering that definition.
On later definitions of that group, an explicit `N` is ignored. `start`
therefore holds only an effective authored reset, and is otherwise null.
No node or value stores the derived number. A consumer scans `specimens`
once, applies each reset, assigns that definition's number, and advances.
The AST needs no public counter or label side table.

`N` has one to nine decimal digits and a value of at least 1. Zero, ten-digit
runs, incomplete parentheses, and missing marker whitespace are ordinary
text. Accumulation stops at the bound before arithmetic can overflow.
Derived counters use the platform's exact integer range; an implementation
must fail explicitly rather than wrap if a document exceeds that range.

`label` is `alnum-run *( ("_" / "-") alnum-run )` over the dialect's letters
and numbers. `id` retains that exact authored spelling; anonymous definitions
have null ids. Registration takes the first definition for each non-null id;
later equal ids remain definitions and still advance the counter. Specimen
and footnote ids occupy separate referent families.

## References and precedence

Registration and resolution are one document-wide finalization, so references
before definitions resolve identically. In inline content outside opaque
constructs, a registered `(@label)` produces a `Cite` with one `Citation`
whose referent is `specimen(id: label)` and whose affixes are empty. A bare
`@label` with no bracketed tail resolves the same way when registered.
A valid block definition marker wins over inline recognition.

`[@label]` and a bare key with a bracketed tail remain bibliography citations.
An unregistered `(@label)` keeps its parentheses as text and uses the
bibliography `authorInText` branch for the interior `@label`. One Cite never
mixes referent families. A reference stores its id, never a copied body or a
derived display number. Resolving an id selects the first matching definition.

A definition's scope spans its marker and body, using the inherited block
ending rules. A Cite spans its complete reference occurrence, including
parentheses when present; its Citation spans the `@` and label. Definition
and reference scopes never substitute for one another.

## Required conformance cases

The following planned fixtures preserve the existing syntax coverage under
the citation model. Additional cases cover definitions inside footnotes and
other containers, empty bodies, repeated and Unicode labels, resets on later
definitions, long inputs, forward references, exact scopes, allocation failure,
and the distinction between anonymous ids and any authored label.

```````````````````````````````` example
(@) First example.
(@) Second example.

Intervening text.

(@) Third example.
.
Document scope=1:1..6:18 anchor=null attributes={} children=1
├── Paragraph scope=4:1..4:17 anchor=null attributes={} children=1
│   └── Text scope=4:1..4:17 anchor=null attributes={} literal="Intervening text." children=0
├── Specimen scope=1:1..1:18 id=null start=null children=1
│   └── Paragraph scope=1:5..1:18 anchor=null attributes={} children=1
│       └── Text scope=1:5..1:18 anchor=null attributes={} literal="First example." children=0
├── Specimen scope=2:1..3:0 id=null start=null children=1
│   └── Paragraph scope=2:5..2:19 anchor=null attributes={} children=1
│       └── Text scope=2:5..2:19 anchor=null attributes={} literal="Second example." children=0
└── Specimen scope=6:1..6:18 id=null start=null children=1
    └── Paragraph scope=6:5..6:18 anchor=null attributes={} children=1
        └── Text scope=6:5..6:18 anchor=null attributes={} literal="Third example." children=0
````````````````````````````````

```````````````````````````````` example
(@good) This is a good example.

As (@good) illustrates, the label resolves.

(@a) x
(@a) y
.
Document scope=1:1..6:6 anchor=null attributes={} children=1
├── Paragraph scope=3:1..3:43 anchor=null attributes={} children=3
│   ├── Text scope=3:1..3:3 anchor=null attributes={} literal="As " children=0
│   ├── Cite scope=3:4..3:10 anchor=null attributes={} children=1
│   │   └── Citation scope=3:5..3:9 referent=specimen(id="good") children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   └── Text scope=3:11..3:43 anchor=null attributes={} literal=" illustrates, the label resolves." children=0
├── Specimen scope=1:1..2:0 id="good" start=null children=1
│   └── Paragraph scope=1:9..1:31 anchor=null attributes={} children=1
│       └── Text scope=1:9..1:31 anchor=null attributes={} literal="This is a good example." children=0
├── Specimen scope=5:1..5:6 id="a" start=null children=1
│   └── Paragraph scope=5:6..5:6 anchor=null attributes={} children=1
│       └── Text scope=5:6..5:6 anchor=null attributes={} literal="x" children=0
└── Specimen scope=6:1..6:6 id="a" start=null children=1
    └── Paragraph scope=6:6..6:6 anchor=null attributes={} children=1
        └── Text scope=6:6..6:6 anchor=null attributes={} literal="y" children=0
````````````````````````````````

```````````````````````````````` example
See (@later).

(@later) Defined afterwards.
.
Document scope=1:1..3:28 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:13 anchor=null attributes={} children=3
│   ├── Text scope=1:1..1:4 anchor=null attributes={} literal="See " children=0
│   ├── Cite scope=1:5..1:12 anchor=null attributes={} children=1
│   │   └── Citation scope=1:6..1:11 referent=specimen(id="later") children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   └── Text scope=1:13..1:13 anchor=null attributes={} literal="." children=0
└── Specimen scope=3:1..3:28 id="later" start=null children=1
    └── Paragraph scope=3:10..3:28 anchor=null attributes={} children=1
        └── Text scope=3:10..3:28 anchor=null attributes={} literal="Defined afterwards." children=0
````````````````````````````````

```````````````````````````````` example
(5@) x
(@) y

(0@) z
.
Document scope=1:1..4:6 anchor=null attributes={} children=1
├── Paragraph scope=4:1..4:6 anchor=null attributes={} children=1
│   └── Text scope=4:1..4:6 anchor=null attributes={} literal="(0@) z" children=0
├── Specimen scope=1:1..1:6 id=null start=5 children=1
│   └── Paragraph scope=1:6..1:6 anchor=null attributes={} children=1
│       └── Text scope=1:6..1:6 anchor=null attributes={} literal="x" children=0
└── Specimen scope=2:1..3:0 id=null start=null children=1
    └── Paragraph scope=2:5..2:5 anchor=null attributes={} children=1
        └── Text scope=2:5..2:5 anchor=null attributes={} literal="y" children=0
````````````````````````````````

```````````````````````````````` example
(@a) x

@a and [@a] and @nope
.
Document scope=1:1..3:21 anchor=null attributes={} children=1
├── Paragraph scope=3:1..3:21 anchor=null attributes={} children=5
│   ├── Cite scope=3:1..3:2 anchor=null attributes={} children=1
│   │   └── Citation scope=3:1..3:2 referent=specimen(id="a") children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   ├── Text scope=3:3..3:7 anchor=null attributes={} literal=" and " children=0
│   ├── Cite scope=3:8..3:11 anchor=null attributes={} children=1
│   │   └── Citation scope=3:9..3:10 referent=bib(key="a",mode=normal) children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   ├── Text scope=3:12..3:16 anchor=null attributes={} literal=" and " children=0
│   └── Cite scope=3:17..3:21 anchor=null attributes={} children=1
│       └── Citation scope=3:17..3:21 referent=bib(key="nope",mode=authorInText) children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
└── Specimen scope=1:1..2:0 id="a" start=null children=1
    └── Paragraph scope=1:6..1:6 anchor=null attributes={} children=1
        └── Text scope=1:6..1:6 anchor=null attributes={} literal="x" children=0
````````````````````````````````

```````````````````````````````` example
See (@nope).
.
Document scope=1:1..1:12 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:12 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:5 anchor=null attributes={} literal="See (" children=0
    ├── Cite scope=1:6..1:10 anchor=null attributes={} children=1
    │   └── Citation scope=1:6..1:10 referent=bib(key="nope",mode=authorInText) children=0
    │       ├── CitationPrefix children=0
    │       └── CitationSuffix children=0
    └── Text scope=1:11..1:12 anchor=null attributes={} literal=")." children=0
````````````````````````````````
