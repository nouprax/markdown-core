# Attributes

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the universal `Markup.attributes` field, the one braced attribute grammar
of the dialect, its normalization and merge operations, and every attachment
site. Options: `inlineCodeAttributes`, `headingAttributes`,
`fencedCodeAttributes`, and `linkAttributes` (each default `false`); the
[directives](directives.md), [bracketed spans](bracketed-spans.md), and
[fenced divs](fenced-divs.md) modules attach the same grammar under their own
options. Source: Pandoc 3.11's attribute syntax and its `combineAttr`
operation, and `remark-directive`'s attachment position. Executable oracle:
the Pandoc 3.11 CLI under `specs/oracles/pandoc/`; remark's own attribute
member tokenizer is not an oracle for this grammar, and every difference from
it is a registered delta. Landing: the field and the directive site with `M7`;
the four options with `P2a` through `P2d`.

## Model

```text
Record(name: String, value: String)
Attributes(classes: [String], records: [Record])
Markup(anchor: String?, attributes: Attributes, scope: Scope, ...)
```

Every `Markup` kind has exactly one non-null `attributes` field and exactly
one nullable `anchor` field; the [anchors](anchors.md) module owns the latter.
They are fields of the tagged union, not a wrapper and not an opt-in
capability. `Record` and `Attributes` are values with no scope, children, or
attributes of their own. There is one payload for every kind; no kind has a
private attribute type, and no lookup over `records` is a second stored
authority.

`Attributes.empty` is `classes=[]` and `records=[]`. It is the value when the
kind has no enabled attachment rule, when no container was authored, when an
authored container was `{}`, when a container held only an ID, and when an
attachment failed. The public AST does not record which of these occurred.

- `classes` preserves order and duplicates. A class shorthand appends one
  class; an exact lowercase `class=` assignment splits its value on ASCII
  whitespace and Zs scalars, drops empty words, and appends the words; the
  special member `-` appends `unnumbered`.
- `records` holds every other assignment in source order, duplicates
  included, names and values case-sensitive and as written after decoding.
  The exact names `id` and `class` never appear in `records`.

Attributes are inert metadata. Parsing a value never executes anything, opens
a target, reads a file, validates a unit, or changes layout. `width=50%`,
`target=_blank`, `numberLines`, and an event-handler-looking name are records
like any other.

## Grammar

```text
attributes        = "{" spacing *( attribute spacing ) "}"
attribute         = identifier / class / assignment / special
identifier        = "#" 1*identifier-character
class             = "." name
assignment        = name "=" value
special           = "-"
name              = unicode-letter *name-rest
name-rest         = unicode-letter / unicode-number / "-" / "_" / ":" / "."
identifier-character
                  = unicode-letter / unicode-number / "-" / "_" / ":" / "."
value             = quoted-value / unquoted-value
quoted-value      = DQUOTE *double-quoted-character DQUOTE /
                    "'" *single-quoted-character "'"
unquoted-value    = *unquoted-character
double-quoted-character
                  = escaped-punctuation / character-reference /
                    permitted-line-ending /
                    any scalar except DQUOTE or line-ending
single-quoted-character
                  = escaped-punctuation / character-reference /
                    permitted-line-ending /
                    any scalar except "'" or line-ending
unquoted-character
                  = escaped-punctuation /
                    any scalar except SP, TAB, line-ending, or unescaped "}"
spacing           = *( SP / TAB ) [ line-ending *( SP / TAB ) ]
line-ending       = LF / CR / CRLF
permitted-line-ending
                  = a line ending inside the extent the owning module grants
                    the container and not followed by a blank line
escaped-punctuation
                  = "\" followed by one ASCII punctuation character
character-reference
                  = a CommonMark named or numeric character reference
unicode-letter    = a scalar of category Lu, Ll, Lt, Lm, or Lo
unicode-number    = a scalar of category Nd, Nl, or No
```

The grammar runs over the owning block's inline content string after block
structure has been decided, so container prefixes are already removed. It is
applied to scalars, not bytes. `spacing` admits at most one line ending and
never a blank line; it is optional, so independently delimited members may be
adjacent: `{#a#b}` is two identifiers and the later one wins, and `{-k=v}` is
the special `-` followed by the assignment `k=v`.

An identifier is non-empty, may begin with any identifier character, and
keeps its dots and colons: `{#one.two}` has identifier `one.two` and `{#1}` is
valid. A class or assignment name begins with a letter: `{.one.two}` has the
one class `one.two`, `{.1}` and `{_key=value}` are malformed. A generic bare
name such as `{disabled}` is malformed; the lone `-` is the only value-less
member.

A value begins after `=`. If the first scalar is `"` or `'` and a matching
closing quote occurs before the end of the container, the value is the quoted
value; otherwise it is the unquoted value, and no other backtracking occurs.
A quoted value may be empty, decodes its escapes and its semicolon-terminated
character references, and normalizes each permitted line ending to one ASCII
space. An unquoted value may be empty, extends to ASCII space, tab, a line
ending, or an unescaped `}`, decodes escapes but not character references, and
admits quotes, `<`, `=`, `>`, and backticks as ordinary content. A backslash
before a non-punctuation scalar is literal. Non-ASCII whitespace is not
`spacing`: it may occur inside a value but cannot separate two members.
Escaped and referenced scalars inside identifier and class shorthands are not
decoded; those members are as written.

## Normalization

Normalization runs exactly once, when an attachment commits, and yields one
anchor candidate and one `Attributes` value:

1. The anchor candidate is the value of the last identifier or exact lowercase
   `id=` assignment in source order; an empty final `id=` makes it `null`.
2. Each `.value` appends one class; each `class=value` appends its words; each
   `-` appends `unnumbered`.
3. Every other assignment appends one `Record` in source order.

`merge(primary, inherited)` combines an occurrence's own values with the
values inherited from a reference definition under `linkAttributes`:

1. The anchor is the primary anchor when non-null, otherwise the inherited
   anchor.
2. The classes are the primary classes followed by the inherited classes, then
   stable-deduplicated by exact value.
3. The records are, first, the inherited records whose names do not occur in
   the primary sequence, keeping only the last inherited occurrence of each
   such name in source order; then the primary records as written.

These three numbered steps are normative; Pandoc's `combineAttr` is their
provenance and evidence. Merge transfers semantic values only and never
changes the occurrence's scope.

## Attachment sites

Only these rules may populate `attributes` and the attribute-side anchor
candidate. Every other kind keeps `Attributes.empty`, and under these rules no
table, row, cell, or caption receives attributes or an anchor; an enabled
[block identifier](block-identifiers.md) may still populate `Table.anchor`.

| Site                          | Owner            | Option                   | Position                                                                           |
| ----------------------------- | ---------------- | ------------------------ | ---------------------------------------------------------------------------------- |
| text, leaf, and container directive | `Directive`, `DirectiveBlock` | `directives`  | immediately after the name or its label                                            |
| inline code                   | `Code`           | `inlineCodeAttributes`   | immediately after the complete closing backtick run                                |
| ATX and Setext heading        | `Heading`        | `headingAttributes`      | the last non-whitespace bytes of the heading's content line                        |
| fenced code                   | `CodeBlock`      | `fencedCodeAttributes`   | the last non-whitespace content of the opening fence line                          |
| direct, reference, and autolink link and image | `Link`, `Image` | `linkAttributes` | immediately after the occurrence, or inherited from its definition        |
| bracketed span                | `Span`           | `bracketedSpans`         | immediately after the balanced closing `]`                                         |
| fenced div                    | `Div`            | `fencedDivs`             | on the opening colon fence                                                         |

Automatic anchors are synthesized by the [anchors](anchors.md) module and are
not an attachment site. A successfully attached container is lexically part of
its owner and inside the owner's scope; the owner's visible content excludes it.

### Inline code

With `inlineCodeAttributes=true`, a container beginning at the byte after the
closing backtick run attaches to that `Code`:

```markdown
`printf()`{.c}
`<$>`{#operator .haskell role="function"}
```

The container is excluded from `Code.literal` and included in `Code.scope`.
Whitespace before `{` prevents attachment. A malformed container leaves the
completed `Code` unchanged and releases the `{` to ordinary inline parsing.
Classes on `Code` have no parser-side meaning; no surface derives a language
from them.

### Headings

With `headingAttributes=true`, for an ATX heading: if the line's last
non-whitespace bytes form a valid container, remove it and the whitespace
before it, then apply the inherited closing-sequence rule to the remainder;
attach only when the first step succeeded. For a Setext heading the first step
applies to the last content line. `# {#x}` is a `Heading` with empty content
and `anchor="x"`; `# Compact{#compact}` and `## Chapter ## {#other}` attach.
An invalid suffix remains heading content. A non-empty explicit anchor wins
over automatic synthesis. With the option off, the container is heading text.

### Fenced code

With `fencedCodeAttributes=true`, the inherited fence rule is applied to the
complete opening line first; a container is then recognized only as the last
non-whitespace content of that line. A bare word before the container is never
an attribute member: in

````markdown
```python {.numberLines startFrom="10"}
print("hello")
```
````

the container attaches `classes=["numberLines"]` and
`records=[Record("startFrom", "10")]`, `CodeBlock.info` is the inherited info
string over the line with the container and the whitespace before it removed,
here `python`, and `language` is its first token. Nothing is lowercased,
aliased, or derived from a class; `numberLines`, `number-lines`,
`lineAnchors`, `line-anchors`, and `startFrom` are ordinary values. A
container followed by other bytes, or a malformed container, is ordinary
info-string text and attaches nothing; the body and closing fence are never
reinterpreted. With the option off, the inherited info contract applies to the
whole line.

### Links and images

With `linkAttributes=true`, a container beginning at the byte after a complete
direct link or image tail, a full or collapsed reference tail that resolves, a
shortcut reference that resolves while `bracketedSpans` is off, or an
angle-bracket autolink attaches to the resulting `Link` or `Image`:

```markdown
[text](https://example.com){target="_blank"}
![image](foo.jpg){#hero .wide width=50%}
<https://example.com>{.external}
```

Whitespace prevents attachment. A bare GFM autolink never accepts a container,
and its inherited termination rule applies to the braces. When the link fails,
the container is released and decided by the bracket procedure of the
[links and images](links-and-images.md) module. An occurrence-local container
is outside label or alt content and inside that occurrence's scope. A malformed
container leaves the completed node unchanged.

A container may also follow a reference definition. With the option on, the
definition grammar is the destination, an optional title, optional spaces or
tabs, at most one line ending plus optional spaces or tabs, one container, and
then only whitespace to the line ending; a malformed container, or the option
off, leaves the inherited definition grammar unchanged, so such a line is then
not a definition. The container is normalized once when the definition is
stored in the parser's reference map and is part of the shared resource that
every occurrence of the definition references. A resolved occurrence receives
`merge(occurrence, definition)`; explicit duplicate-definition precedence is
unchanged and an unresolved reference inherits nothing. The emitted node keeps
the source-faithful range of its own occurrence:

```markdown
[x][r]

[r]: /target {#foo}
```

emits `Link(anchor="foo", ...)` whose scope is exactly `[x][r]`, while in
`[x][r]{#bar}` the occurrence-local container is inside the link's scope.

Image `width` and `height` assignments are records stored verbatim,
`width=50%` and `height=2in` alike; the parser validates no unit. The typed
`Image.width` and `Image.height` fields are populated only by the
[image dimensions](links-and-images.md) rule, never by a record, and vice
versa.

### Directives, spans, and divs

The directive container follows the name or the label with no whitespace, at
most one container attaches, and a container in a block directive must close
on the opener line; the [directives](directives.md) module states the rest. A
bracketed span's container follows its closing `]` immediately, and a fenced
div's container or class word sits on the opening fence; the
[bracketed spans](bracketed-spans.md) and [fenced divs](fenced-divs.md)
modules state the rest.

## Failure and complexity

A candidate commits atomically only after its closing `}` and every member
have parsed. An invalid or unclosed candidate attaches nothing, emits no
partial value, and releases its source under the owning construct's fallback;
the failed `{` is text. Scanning is a single forward pass, and normalization
and merging are linear in source bytes plus output. There is one attribute
scanner in the C core, shared by every site; no site keeps a private tokenizer
or storage shape.

## Required conformance cases

Tests cover the universal field on every `Markup` kind; `Attributes.empty`
beside a non-null anchor; every projection in the model section; dots and
colons in shorthands; numeric identifier starts; letter-only name starts;
`{-}`; bare-name rejection; empty, quoted, and unquoted values; escapes;
quoted-only character-reference decoding; line-ending normalization;
non-ASCII whitespace positions; identifier replacement and clearing; ordered
duplicate classes and records; `id=` and `class=` projection; merge without
scope mutation; inert unsafe-looking metadata; malformed and unclosed
fallback; every attachment site with immediate and spaced containers, its
option on and off, and its owner's exact scope; the heading order rule; the
fenced-code `info` rule; definition-side containers and inheritance without
definition-range inheritance; allocation failure; and size-doubling valid,
duplicate, malformed, and unclosed containers.
