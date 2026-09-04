# Pandoc attribute attachment

Status: normative target profile contract for `bracketed_spans`,
`inline_code_attributes`, `header_attributes`, `fenced_code_attributes`,
`link_attributes`, `fenced_divs`, and the anchor result of Markdown Core
`auto_anchors`.

The shared [anchor](../anchors.md) and [attributes](../attributes.md) contracts
own the universal `Markup.anchor` and `Markup.attributes` fields, the sole
braced grammar, normalization, and merge operation. This module owns only
attachment positions, precedence, scope, and option behavior. Authority is the
Pandoc User's Guide for
[attributes](https://pandoc.org/MANUAL.html#extension-attributes),
[bracketed spans](https://pandoc.org/MANUAL.html#extension-bracketed_spans),
[inline code attributes](https://pandoc.org/MANUAL.html#extension-inline_code_attributes),
[heading attributes](https://pandoc.org/MANUAL.html#extension-header_attributes),
[fenced code attributes](https://pandoc.org/MANUAL.html#extension-fenced_code_attributes),
[link attributes](https://pandoc.org/MANUAL.html#extension-link_attributes), and
[fenced divs](https://pandoc.org/MANUAL.html#extension-fenced_divs), at the
snapshot pinned by the [Pandoc extension index](../pandoc.md).

## Accepted grammar

Every braced attachment position accepts exactly the shared Pandoc 3.11
grammar. Representative projections are:

| Source                                    | Markdown Core result                                                                 |
| ----------------------------------------- | ------------------------------------------------------------------------------------ |
| `{#one.two}`                              | `anchor="one.two"`                                                                   |
| `{.one.two}`                              | `classes=["one.two"]`                                                                |
| `{#one:two .three:four}`                  | `anchor="one:two"`, `classes=["three:four"]`                                         |
| `{-}`                                     | `classes=["unnumbered"]`                                                             |
| `{id=one class="wide warning" key=value}` | `anchor="one"`, `classes=["wide", "warning"]`, `records=[Record("key", "value")]` |
| `{disabled}`                              | malformed; no anchor or attribute attachment                                         |

An unbraced fenced-Div word is a separate production rather than an attribute
container; it remains shorthand for one class. Thus `::: -` produces class
`-`, while `::: {-}` produces class `unnumbered`.

## Attachment registry

Only these enabled Pandoc rules can populate the universal anchor or
attributes fields:

| Extension/source rule    | Owner           | Attachment position                                                         |
| ------------------------ | --------------- | --------------------------------------------------------------------------- |
| `bracketed_spans`        | `Span`          | immediately after the balanced closing `]`                                  |
| `inline_code_attributes` | `Code`          | immediately after the complete closing backtick run                         |
| `header_attributes`      | `Heading`       | at the end of ATX or Setext heading text                                    |
| `auto_anchors`           | `Heading`       | synthesized `anchor` when no non-empty explicit value exists                |
| `fenced_code_attributes` | `CodeBlock`     | the opening fence's info region                                             |
| `link_attributes`        | `Link`, `Image` | immediately after an occurrence, or inherited from its reference definition |
| `fenced_divs`            | `Div`           | the opening colon fence                                                     |

Every other Markup kind retains `anchor=null` and `Attributes.empty` unless
another profile contributes an independent rule. The requested table
extensions do not define table-attribute syntax, so Table, row, and cell
anchors are null and their attributes remain empty.

Successfully authored attachment syntax that is lexically part of an
occurrence is inside that occurrence's source-faithful scope. A synthesized
anchor neither extends scope nor receives a fictional source position.

## Spans and divs

`bracketed_spans` requires a successfully parsed container immediately after
the balanced `]`. `{}` still creates a Span with `anchor=null` and
`Attributes.empty`. Span content, bracket precedence, and fallback are defined
by [bracketed spans](bracketed-spans.md).

A `fenced_divs` opener carries either one braced shared attribute list or one
non-whitespace unbraced word interpreted as a class. `{}` is a valid empty
opener. Fence recognition, nesting, and closing are defined by
[fenced divs](fenced-divs.md).

## Inline code

With `inline_code_attributes=true`, a container must begin immediately after a
complete code span:

```text
`printf()`{.c}
`<$>`{#operator .haskell role="function"}
```

The suffix is excluded from `Code.literal` and included in `Code.scope`.
Whitespace before `{` prevents attachment. A malformed suffix leaves the
completed Code unchanged and releases the suffix to ordinary inline parsing.
The first class is the conventional language and its sole stored authority; a
binding may expose a derived language convenience property.

## Headings and automatic anchors

With `header_attributes=true`, a list at the end of an ATX or Setext heading
attaches to that Heading. It may immediately follow visible heading text or be
separated by whitespace. Optional ATX closing hashes precede it:

```text
# Chapter {#sec:intro .unnumbered}
## Chapter ## {#other key="value"}
# Compact{#compact}

Setext heading {#setext}
--------------
```

The suffix is removed from heading content and included in Heading scope. An
invalid suffix remains visible content. A non-empty explicit anchor wins over
automatic generation. `auto_anchors` writes its generated value into the
universal `anchor` field; the algorithm and implicit references are defined by
[heading anchors](headings-and-anchors.md).

## Fenced code

With `fenced_code_attributes=true`, tilde and backtick code fences accept a
braced shared attribute list in the opening info region. A single bare
language word is shorthand for the first class and may precede another list:

````markdown
```python {.numberLines startFrom="10"}
print("hello")
```
````

The bare language is lowercased; `c++` maps to `cpp` and `objective-c` to
`objectivec`. The first class is the sole stored language authority.
`numberLines`, `number-lines`, `lineAnchors`, `line-anchors`, and `startFrom`
remain inert metadata. A malformed list attaches nothing and does not
reinterpret the code body or closing fence. With the option disabled, the
inherited code-block info contract applies and the rule populates nothing.

## Links and images

With `link_attributes=true`, a list beginning immediately after a complete
direct link, image, resolved reference occurrence, or angle-bracket autolink
attaches to the resulting `Link` or `Image`:

```markdown
[text](https://example.com){target="_blank"}
![image](foo.jpg){#hero .wide width=50%}
<https://example.com>{.external}
```

Whitespace prevents attachment. An occurrence-local suffix is outside
label/alt content and inside that Link or Image occurrence's scope. A malformed
suffix leaves the completed node unchanged.

A valid list may also follow a reference definition's destination/title,
including on its allowed continuation line. Its anchor candidate and
attributes are stored in the parser's reference map rather than emitted as a
public node. A resolved occurrence uses the shared
`merge(occurrence, definition)` operation. Explicit
duplicate-definition precedence is unchanged; unresolved references inherit
nothing. Merge transfers semantic values only: the definition owns the authored
suffix and its parser-internal source range, while the emitted Link or Image
retains the source-faithful range of its own occurrence.

For example:

```markdown
[x][r]

[r]: /target {#foo}
```

emits `Link(anchor="foo", ...)` whose scope is exactly the `[x][r]`
occurrence. It does not include or point at any byte of the definition. By
contrast, in `[x][r]{#bar}`, the occurrence-local suffix is part of that
Link's scope because those bytes lexically form the emitted occurrence.

Image `width` and `height` values may be unitless pixels or use `px`, `cm`,
`mm`, `in`, `inch`, or `%` without internal whitespace. They remain strings;
the parser performs no file inspection or unit conversion.

## Precedence, meaning, and failure

The completed outer construct owns a candidate: Link/Image attachment precedes
Span fallback, bracketed Span precedes shortcut-reference fallback, and a fence
owner precedes paragraph text. Containers inside code literals, labels,
destinations, titles, comments, or HTML tokens remain owned by those
constructs.

Attributes are inert metadata. `target`, event-handler-looking names,
dimensions, language classes, and line-number classes perform no parser-side
action. Interpretation and security policy belong to consumers.

An invalid or unclosed list emits no partial attributes and releases source
according to the owning construct's fallback. The profile reuses the shared
single-pass parser and normalization operation; it may not rescan completed
nodes or create node-specific attribute stores.

## Required conformance cases

Tests must cover every registry row; Pandoc shorthand boundaries at every
attachment site; anchor, classes, and records; `{-}`; bare-name rejection;
`{}`; immediate versus spaced attachment; malformed fallback; option-off
behavior; nested ownership and cross-extension precedence; exact occurrence
scopes; language aliases; image units; occurrence-local suffix ranges;
reference inheritance without definition-range inheritance; synthesized
anchors; inert unsafe-looking metadata; allocation failure; and size-doubling
valid, duplicate, malformed, and unclosed inputs.
