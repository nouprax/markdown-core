# Task lists

Status: normative module of the [Markdown Core dialect](../dialect.md).
Sources: cmark-gfm's task-list
extension; Obsidian's custom task characters. Executable oracles: cmark-gfm
for the inherited markers, `@quartz-community/remark-obsidian` for custom
markers. Landing: the `marker` field landed with `M5`; custom markers land with `O5`. The
[example format](../dialect.md#examples) is defined by the index.

## Model

```text
ListItem(marker: String?, exampleLabel: String?, content: [Markup])
```

- `marker == null` means the item is not a task.
- `marker == " "` means an incomplete task.
- Every other marker is a completed or custom-state task; the exact scalar is
  preserved, `x` and `X` included, and the parser assigns no meaning to it.

Bindings may expose the derived conveniences `tasked = marker != null` and
`completed = marker != null && marker != " "`. No native node, wire payload,
or binding stores a checked boolean beside `marker`. `exampleLabel` belongs
to the [lists](lists.md) module.

```````````````````````````````` example
- [ ] open
- [x] done
- [X] also done
.
Document scope=1:1..3:15 anchor=null attributes={} children=1
└── List scope=1:1..3:15 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=true children=3
    ├── ListItem scope=1:1..1:10 anchor=null attributes={} marker=" " exampleLabel=null children=1
    │   └── Paragraph scope=1:7..1:10 anchor=null attributes={} children=1
    │       └── Text scope=1:7..1:10 anchor=null attributes={} literal="open" children=0
    ├── ListItem scope=2:1..2:10 anchor=null attributes={} marker="x" exampleLabel=null children=1
    │   └── Paragraph scope=2:7..2:10 anchor=null attributes={} children=1
    │       └── Text scope=2:7..2:10 anchor=null attributes={} literal="done" children=0
    └── ListItem scope=3:1..3:15 anchor=null attributes={} marker="X" exampleLabel=null children=1
        └── Paragraph scope=3:7..3:15 anchor=null attributes={} children=1
            └── Text scope=3:7..3:15 anchor=null attributes={} literal="also done" children=0
````````````````````````````````

## Syntax

```text
task-prefix    = "[" task-marker "]" 1*task-separator
task-separator = SP / TAB / VT / FF
```

The prefix is recognized at the first non-space byte of a list item's first
line, after the inherited list marker and its padding, before the item's first
block is decided; the remainder of the line begins that block. It applies to
every list item the inherited list algorithm accepts, bullet or ordered, at
any nesting depth:

```````````````````````````````` example
1. [x] a
   - [ ] b
.
Document scope=1:1..2:10 anchor=null attributes={} children=1
└── List scope=1:1..2:10 anchor=null attributes={} flavor=ordered start=1 variant=decimal delimiter=period tight=true children=1
    └── ListItem scope=1:1..2:10 anchor=null attributes={} marker="x" exampleLabel=null children=2
        ├── Paragraph scope=1:8..1:8 anchor=null attributes={} children=1
        │   └── Text scope=1:8..1:8 anchor=null attributes={} literal="a" children=0
        └── List scope=2:4..2:10 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=true children=1
            └── ListItem scope=2:4..2:10 anchor=null attributes={} marker=" " exampleLabel=null children=1
                └── Paragraph scope=2:10..2:10 anchor=null attributes={} children=1
                    └── Text scope=2:10..2:10 anchor=null attributes={} literal="b" children=0
````````````````````````````````

The separator is structural and is removed with the prefix. A prefix at the
end of its line, with no separator, is not a task prefix; the line ending is
not a separator, and neither is a letter:

```````````````````````````````` example
- [x]	tab
- [x]
- [x]none
.
Document scope=1:1..3:9 anchor=null attributes={} children=1
└── List scope=1:1..3:9 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=true children=3
    ├── ListItem scope=1:1..1:9 anchor=null attributes={} marker="x" exampleLabel=null children=1
    │   └── Paragraph scope=1:7..1:9 anchor=null attributes={} children=1
    │       └── Text scope=1:7..1:9 anchor=null attributes={} literal="tab" children=0
    ├── ListItem scope=2:1..2:5 anchor=null attributes={} marker=null exampleLabel=null children=1
    │   └── Paragraph scope=2:3..2:5 anchor=null attributes={} children=1
    │       └── Text scope=2:3..2:5 anchor=null attributes={} literal="[x]" children=0
    └── ListItem scope=3:1..3:9 anchor=null attributes={} marker=null exampleLabel=null children=1
        └── Paragraph scope=3:3..3:9 anchor=null attributes={} children=1
            └── Text scope=3:3..3:9 anchor=null attributes={} literal="[x]none" children=0
````````````````````````````````

Only the first bytes of the item are tested; a later `[x]` is text:

```````````````````````````````` example
- a [x] b
.
Document scope=1:1..1:9 anchor=null attributes={} children=1
└── List scope=1:1..1:9 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=true children=1
    └── ListItem scope=1:1..1:9 anchor=null attributes={} marker=null exampleLabel=null children=1
        └── Paragraph scope=1:3..1:9 anchor=null attributes={} children=1
            └── Text scope=1:3..1:9 anchor=null attributes={} literal="a [x] b" children=0
````````````````````````````````

The rule applies inside any container:

```````````````````````````````` example
> - [ ] a
.
Document scope=1:1..1:9 anchor=null attributes={} children=1
└── Callout scope=1:1..1:9 anchor=null attributes={} variant=null collapsed=null children=1
    └── List scope=1:3..1:9 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=true children=1
        └── ListItem scope=1:3..1:9 anchor=null attributes={} marker=" " exampleLabel=null children=1
            └── Paragraph scope=1:9..1:9 anchor=null attributes={} children=1
                └── Text scope=1:9..1:9 anchor=null attributes={} literal="a" children=0
````````````````````````````````

`task-marker` is exactly one Unicode scalar of any value: a space, `x`, and
`X` are the inherited markers, and every other scalar is a custom marker:

```````````````````````````````` example
- [?] a
- [-] b
- [✓] c
.
Document scope=1:1..3:9 anchor=null attributes={} children=1
└── List scope=1:1..3:9 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=true children=3
    ├── ListItem scope=1:1..1:7 anchor=null attributes={} marker="?" exampleLabel=null children=1
    │   └── Paragraph scope=1:7..1:7 anchor=null attributes={} children=1
    │       └── Text scope=1:7..1:7 anchor=null attributes={} literal="a" children=0
    ├── ListItem scope=2:1..2:7 anchor=null attributes={} marker="-" exampleLabel=null children=1
    │   └── Paragraph scope=2:7..2:7 anchor=null attributes={} children=1
    │       └── Text scope=2:7..2:7 anchor=null attributes={} literal="b" children=0
    └── ListItem scope=3:1..3:9 anchor=null attributes={} marker="✓" exampleLabel=null children=1
        └── Paragraph scope=3:9..3:9 anchor=null attributes={} children=1
            └── Text scope=3:9..3:9 anchor=null attributes={} literal="c" children=0
````````````````````````````````

`[]` and `[ab]` are not task prefixes. The scanner decodes at most the
candidate marker before rejecting a malformed prefix:

```````````````````````````````` example
- [] a
- [ab] b
.
Document scope=1:1..2:8 anchor=null attributes={} children=1
└── List scope=1:1..2:8 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=true children=2
    ├── ListItem scope=1:1..1:6 anchor=null attributes={} marker=null exampleLabel=null children=1
    │   └── Paragraph scope=1:3..1:6 anchor=null attributes={} children=1
    │       └── Text scope=1:3..1:6 anchor=null attributes={} literal="[] a" children=0
    └── ListItem scope=2:1..2:8 anchor=null attributes={} marker=null exampleLabel=null children=1
        └── Paragraph scope=2:3..2:8 anchor=null attributes={} children=1
            └── Text scope=2:3..2:8 anchor=null attributes={} literal="[ab] b" children=0
````````````````````````````````

## Fallback

A malformed prefix is inline text of the item's first block.
Inline code and other opaque constructs cannot affect recognition, because
the prefix is decided before inline parsing.

## Scopes

`ListItem.scope` covers the prefix; the item's first block and its
descendants begin after the separator.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover every
separator, deeper nesting, wire round-trips of the marker on every binding,
allocation failure, and long malformed bracket runs.
