# Fenced divs

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `fencedDivs` (default `false`). Source: Pandoc's `fenced_divs`.
Executable oracle: the Pandoc 3.11 CLI under `specs/oracles/pandoc/`.
Landing: `P8`.

## Model

```text
Div(closed: Bool, content: [Markup])
```

`Div` is a block kind whose content is arbitrary block content. Its attribute
container or class word populates the universal `anchor` and `attributes`
fields under the [attributes](attributes.md) module.

## Grammar

Recognition is block-start step 8, tested after the directive step:

```text
opener     = *3SP 3*":" *WSP ( attributes / class-word ) *WSP *":" *WSP EOL
class-word = 1*( non-whitespace scalar other than ":", "{", and "}" )
closer     = *3SP 3*":" *WSP EOL
```

A tab in the leading indentation disqualifies the line. The colon run must be
followed by whitespace or `{`: a run followed immediately by a directive name
is a container directive under `directives`, and with `directives` off such a
line is paragraph text. An unbraced class word is shorthand for one class,
not an attribute container, so `::: -` produces class `-` while `::: {-}`
produces class `unnumbered`; `::: {}` is a valid opener with `anchor=null`
and `Attributes.empty`. A colon run with neither a container nor a class word
never opens a div.

An opener may interrupt a paragraph, as a code fence does. The div's content
is parsed in place by the ordinary block parser through the shared container
stack, so divs nest without a separate algorithm. A closer is eligible when,
after the enclosing containers' prefixes are stripped, the line matches the
closer grammar and its colon run is at least as long as the innermost open
colon container's opening run; it then closes that container, whether it is
a `Div` or a container directive, and may interrupt a paragraph inside it. A
shorter bare colon line is ordinary content of the innermost container. A
colon line inside fenced code, an HTML block, or another opaque block is that
block's content. Pandoc closes with any colon run of three or more; the
[conflicts](conflicts.md) register records the difference.

A div with no eligible closer ends where its enclosing container's content
ends or at the end of the document, with `closed=false`; it is the same node
either way.

## Option behavior and fallback

With `fencedDivs=false`, colon lines are paragraph text or, under
`directives`, directives. With the option on, an invalid braced list or
trailing bytes other than spaces and colons leave the line to the following
block starts, and after a valid opener commits a malformed inner opener is
content and cannot close its parent accidentally. Recognition never scans
ahead for a matching fence before parsing content, and allocation failure
follows the shared rule.

## Scopes

A closed `Div.scope` covers both fence lines and everything between. An
unclosed `Div.scope` ends at the end of the last line of its last child
block, or at the end of the opener line when it has no children.

## Required conformance cases

Tests cover braced, empty, and unbraced-class openers; three and longer
runs; optional trailing colons; a run followed by a name with `directives` on
and off; closers shorter than, equal to, and longer than the opener; nested
divs and a div nested with a container directive; empty content; missing
closers at container end and document end; paragraph interruption; malformed
containers; code and HTML opacity; block content of every kind; exact scopes;
option-off output; nesting limits; allocation failure; and long colon runs.
