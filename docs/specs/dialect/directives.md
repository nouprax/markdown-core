# Directives

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `directives` (default `true`). Source: `remark-directive` 4.0.0 and
its `micromark-extension-directive` envelope syntax. Executable oracle: remark
under `specs/oracles/remark/`, for the envelope, label, and attachment
position; the attribute member grammar is the dialect's own, stated by the
[attributes](attributes.md) module. Landing: present; the attribute model
migrates to the universal fields with `M7`.

## Model

```text
Directive(name: String, label: DirectiveLabel?)
DirectiveBlock(name: String, label: DirectiveLabel?, content: [Markup])
DirectiveLabel(content: [Markup])
```

`Directive` is an inline leaf whose only owned markup is its optional label.
`DirectiveBlock` is a block kind whose content is block content; a leaf block
directive and an empty container directive produce identical values with
`content=[]`. `DirectiveLabel` is `Markup` owned only by the typed `label`
field, never an element of `content`, and its scope spans its brackets, so an
empty label is a place. Attributes populate the universal `anchor` and
`attributes` fields once `M7` lands; until then the current contract's
`attributes: [DirectiveAttribute]?` stands.

## Names

A directive name is one or more Unicode scalars. The first scalar is any
scalar above U+0020 that is neither whitespace nor punctuation under the
dialect's Unicode tables; each further scalar is such a scalar or `-` or `_`.
A name does not end with `-` or `_`. Names are stored as written and compared
by no one in the parser.

## Text directives

A text directive is inline step A10:

```text
text-directive = ":" name [ label ] [ attributes ]
label          = "[" label-content "]"
```

The colon may not be preceded or followed by another colon, and the name may
not be followed by a colon, so `x ::a y`, `x:::a`, and `:red:` contain no
directive. The name, label, and attribute container must be adjacent in that
order with no whitespace between them; a `[` after a container is text, and at
most one label and one container attach. `label-content` is balanced-bracket
source: a backslash escapes the next byte, an unescaped `[` increases the
nesting depth and an unescaped `]` decreases it, the label ends at the `]`
that returns the depth to zero, and a label that would reach depth 33 is not a
label. The label may span soft line breaks but not a block boundary. Its
content is parsed by the inline parser as ordinary inline content, so a link,
a span, or another directive may occur inside it.

A directive commits at its name. A `[` that does not complete a label leaves
the directive without a label and is text; a `{` that does not complete a
valid container leaves the directive without attributes and is text. Both
follow the shared failure rule and consume nothing.

## Block directives

Block directives are step 7 of the block-start order and are tested at the
first non-space byte of a line indented at most three spaces:

```text
leaf-opener      = "::" name [ label ] [ attributes ] *WSP EOL
container-opener = 3*":" name [ label ] [ attributes ] *WSP EOL
closer           = *3SP 3*":" *WSP EOL
```

A tab in the leading indentation disqualifies the line. The colon run must be
followed immediately by the name; a run followed by whitespace, `{`, or the
end of line is not a directive and is left to the [fenced divs](fenced-divs.md)
step. The label and container follow the text-directive rules but must close
on the opener line; an unclosed label or container, or any other byte after
the last accepted element, makes the line ordinary content for the following
steps. Both opener forms may interrupt a paragraph.

A leaf directive is complete at its line and has `content=[]`. A container
directive opens a block container whose content is parsed by the ordinary
block parser and closes at the first later closer line whose colon run is at
least as long as the opener's, after the enclosing containers' prefixes are
stripped; a closer line inside fenced code, an HTML block, or another opaque
block is that block's content. An unclosed container ends where its enclosing
container's content ends or at the end of the document and produces the same
node. The closer of a fenced div is the same closer grammar; a bare colon line
closes the innermost open colon container of either kind that it is long
enough to close, as the fenced divs module and the conflicts register state.

## Attributes

The container is the shared attribute grammar of the
[attributes](attributes.md) module, attached at the owner named above. `{}`
attaches successfully and yields `anchor=null` and `Attributes.empty`. In a
text directive a quoted value may span the line endings the grammar permits;
in a block directive the container must close on the opener line.

## Option behavior and fallback

With `directives=false`, no colon has directive meaning and every byte follows
the inherited grammar: `:name` is text, `::name` and `:::name` lines are
paragraph text or, under `fencedDivs`, are tested by that module. With the
option on, a failed candidate consumes nothing. Source owned by code, HTML
tokens and blocks, comments, formulas, and cross links is never a directive.

## Scopes

`Directive.scope` covers the colon through the last byte of the name, label,
or container, whichever was accepted last. `DirectiveBlock.scope` covers the
opener line through the closer line, or through the last consumed content line
when unclosed. `DirectiveLabel.scope` covers its brackets.

## Required conformance cases

The directive fixtures `extensions-directive.txt` and
`extensions-directive-option-gates.txt` stay the oracle of record. Tests cover
Unicode names, names beginning or ending with `-` or `_`, adjacent colons,
labels with escapes, nesting to depths 32 and 33, multi-line labels, every
container success and failure, leaf and empty container equivalence, closer
lengths shorter and longer than the opener, unclosed containers at container
end and document end, closer lines inside opaque blocks, paragraph
interruption, a colon run followed by whitespace under `fencedDivs` on and
off, exact scopes, option-off output, allocation failure, and size-doubling
colon runs, brackets, and braces.
