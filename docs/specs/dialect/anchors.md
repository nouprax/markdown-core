# Anchors

[Syntax guide](../dialect.md) · [Documentation](../README.md)

An anchor names a node so an application can link to it. Every anchor is
explicit: supported syntax attaches it, and the parser derives none from a
node's text.

```markdown
# Getting started {#start}

[Jump to the section](#start)
```

The heading's `anchor` is `start`; the link's destination is `#start`.
Referring to an anchor does not declare an anchor on the link itself. The
parser does not navigate to the target.

## Explicit anchors

Use an attribute identifier on a heading or another supported attachment site.
Both `{#start}` and `{id=start}` declare the same value. Within one container
the last ID wins; an empty final `id=` clears the candidate. See
[attributes](attributes.md) for all attachment sites, and
[block identifiers](block-identifiers.md) for the `#id#` declaration on other
blocks.

## Headings without one

```markdown
# Hello, World!
```

A heading that declares no anchor has none: its `anchor` is null. An
application that wants a link target for every heading computes one from the
heading's content itself, by whatever rule it chooses; the parser reserves no
name for it.

## Linking to a heading

Link to a heading by the anchor it declares: `[First chapter](#chapter)` for
`# First chapter {#chapter}`. A bracketed heading text such as
`[First chapter]` is no reference; only a link reference definition declares a
label.

## Anchor ownership

A link reference definition is a `Reference` with the anchor and attributes
the definition states, and a reference occurrence has the ones written on it.
Two explicit declarations may keep the same anchor, without diagnostics.

Cross-link anchors remain raw; the application chooses how to match them to
these declarations. See [cross links](cross-links.md#headings-and-blocks) and
the [heading-resolution architecture](../../architecture/heading-resolution.md)
for the syntax and implementation boundaries.
