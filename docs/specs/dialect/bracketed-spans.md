# Bracketed spans

Status: normative module of the [Markdown Core dialect](../dialect.md).
Source: Pandoc's `bracketed_spans`. Executable oracle: the Pandoc 3.11 CLI
under `specs/oracles/pandoc/`. Landing: `P5`. The
[example format](../dialect.md#examples) is defined by the index.

## Model and syntax

```text
bracketed-span = "[" inline-content "]" attributes

Span(content: [Markup])
```

`Span` is an inline kind. Its content is inline content and may be empty; its
attribute container populates the universal `anchor` and `attributes` fields
under the [attributes](attributes.md) module:

```````````````````````````````` example
[text]{.class} [x]{#id .c k="v"}
.
Document scope=1:1..1:32 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:32 anchor=null attributes={} children=3
    ├── Span scope=1:1..1:14 anchor=null attributes={.class} children=1
    │   └── Text scope=1:2..1:5 anchor=null attributes={} literal="text" children=0
    ├── Text scope=1:15..1:15 anchor=null attributes={} literal=" " children=0
    └── Span scope=1:16..1:32 anchor="id" attributes={.c k="v"} children=1
        └── Text scope=1:17..1:17 anchor=null attributes={} literal="x" children=0
````````````````````````````````

`{}` yields a `Span` with `anchor=null` and `Attributes.empty`, and the
content may be empty:

```````````````````````````````` example
[]{}
.
Document scope=1:1..1:4 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
    └── Span scope=1:1..1:4 anchor=null attributes={} children=0
````````````````````````````````

Recognition is alternative 3 of the bracket procedure of the
[links and images](links-and-images.md) module: at the `]` that balances the
opener, after a valid direct tail and a resolving reference tail have been
excluded, a valid attribute container beginning at the byte
after `]` produces the span. The brackets use the inherited balanced-bracket
scanner: escaped brackets and brackets owned by code or by a completed inline
construct do not close the span, and the body follows the shared inline
rules:

```````````````````````````````` example
[*a* b]{.c} [a [b] c]{.c}
.
Document scope=1:1..1:25 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:25 anchor=null attributes={} children=3
    ├── Span scope=1:1..1:11 anchor=null attributes={.c} children=2
    │   ├── Emphasis scope=1:2..1:4 anchor=null attributes={} children=1
    │   │   └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
    │   └── Text scope=1:5..1:6 anchor=null attributes={} literal=" b" children=0
    ├── Text scope=1:12..1:12 anchor=null attributes={} literal=" " children=0
    └── Span scope=1:13..1:25 anchor=null attributes={.c} children=1
        └── Text scope=1:14..1:20 anchor=null attributes={} literal="a [b] c" children=0
````````````````````````````````

`[text]{.key}` is a `Span`, not a shortcut reference, even when a definition
`text` exists:

```````````````````````````````` example
[text]{.key}

[text]: /u
.
Document scope=1:1..3:10 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:12 anchor=null attributes={} children=1
    └── Span scope=1:1..1:12 anchor=null attributes={.key} children=1
        └── Text scope=1:2..1:5 anchor=null attributes={} literal="text" children=0
````````````````````````````````

A direct tail and a resolving full or collapsed reference tail are tested
first, so a container after such a link attaches to the link under the
[attributes](attributes.md) module and makes no span:

```````````````````````````````` example
[text](/u){.c} [text][r]{.c}

[r]: /r
.
Document scope=1:1..3:7 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:28 anchor=null attributes={} children=3
    ├── Link scope=1:1..1:14 anchor=null attributes={.c} dest=url("/u") title=null children=1
    │   └── Text scope=1:2..1:5 anchor=null attributes={} literal="text" children=0
    ├── Text scope=1:15..1:15 anchor=null attributes={} literal=" " children=0
    └── Link scope=1:16..1:28 anchor=null attributes={.c} dest=url("/r") title=null children=1
        └── Text scope=1:17..1:20 anchor=null attributes={} literal="text" children=0
````````````````````````````````

Because a `Span` is not a link, complete links may occur inside it, each
subject to its own no-link-inside-link restriction:

```````````````````````````````` example
[see [a](/u) here]{.c}
.
Document scope=1:1..1:22 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:22 anchor=null attributes={} children=1
    └── Span scope=1:1..1:22 anchor=null attributes={.c} children=3
        ├── Text scope=1:2..1:5 anchor=null attributes={} literal="see " children=0
        ├── Link scope=1:6..1:12 anchor=null attributes={} dest=url("/u") title=null children=1
        │   └── Text scope=1:7..1:7 anchor=null attributes={} literal="a" children=0
        └── Text scope=1:13..1:17 anchor=null attributes={} literal=" here" children=0
````````````````````````````````

`[@foo]{.key}` is a `Span` containing an author-in-text `Cite`; the
[citations](citations.md) module shows it. A cross link is complete at its
`]]`, so `[[wiki]]{.x}` is a cross link followed by text:

```````````````````````````````` example
[[wiki]]{.x}
.
Document scope=1:1..1:12 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:12 anchor=null attributes={} children=2
    ├── CrossLink scope=1:1..1:8 anchor=null attributes={} dest=cross(path="wiki",anchor=null) label=null children=0
    └── Text scope=1:9..1:12 anchor=null attributes={} literal="{.x}" children=0
````````````````````````````````

A text directive's label is claimed by the directive scanner before this
procedure, and a `Span` may occur inside such a label:

```````````````````````````````` example
:a[[x]{.c}]
.
Document scope=1:1..1:11 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:11 anchor=null attributes={} children=1
    └── Directive scope=1:1..1:11 anchor=null attributes={} name="a" children=0
        └── DirectiveLabel scope=1:3..1:11 anchor=null attributes={} children=1
            └── Span scope=1:4..1:10 anchor=null attributes={.c} children=1
                └── Text scope=1:5..1:5 anchor=null attributes={} literal="x" children=0
````````````````````````````````

The attribute alternative also applies to an image opener that has no valid
media tail: the `!` remains a separate Text sibling and the `Span.scope`
begins at `[`, as the shared bracket procedure requires:

```````````````````````````````` example
![x]{.c} ![]{}
.
Document scope=1:1..1:14 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:14 anchor=null attributes={} children=4
    ├── Text scope=1:1..1:1 anchor=null attributes={} literal="!" children=0
    ├── Span scope=1:2..1:8 anchor=null attributes={.c} children=1
    │   └── Text scope=1:3..1:3 anchor=null attributes={} literal="x" children=0
    ├── Text scope=1:9..1:10 anchor=null attributes={} literal=" !" children=0
    └── Span scope=1:11..1:14 anchor=null attributes={} children=0
````````````````````````````````

## Fallback

An invalid or unclosed container fails this alternative without invalidating
the bracket pair: the cite, shortcut, and literal alternatives then apply to
the same pair and the `{` is text. Whitespace between `]` and `{` prevents
attachment. No partial `Span` is emitted:

```````````````````````````````` example
[text]{.c [text] {.c} [text]{.1}
.
Document scope=1:1..1:32 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:32 anchor=null attributes={} children=1
    └── Text scope=1:1..1:32 anchor=null attributes={} literal="[text]{.c [text] {.c} [text]{.1}" children=0
````````````````````````````````

Code spans, comments, HTML tokens, formulas, and cross links are opaque;
recognition uses the shared bracket stack and never searches forward from
every `[`.

## Scopes

`Span.scope` covers both brackets, the body, and the complete container.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover every
shared attribute form, citation and link attribute precedence, escaped
brackets, code, comments, HTML, and formulas, exact scopes, deep nesting,
allocation failure, and size-doubling bracket and brace runs.
