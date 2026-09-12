# Anchors

[Syntax guide](../dialect.md) · [Documentation](../README.md)

An anchor names a node so an application can link to it. Headings receive
automatic anchors, and supported syntax can attach an explicit one.

```markdown
# Getting started

[Jump to the section](#getting-started)
```

The heading's `anchor` is `getting-started`; the link's destination is
`#getting-started`. Referring to an anchor does not declare an anchor on the
link itself. The parser does not navigate to the target.

## Explicit anchors

Use an attribute identifier on a heading or another supported attachment site:

```markdown
# Getting started {#start}
```

The heading keeps `start` instead of generating an ID. Both `{#start}` and
`{id=start}` declare the same value. Within one container the last ID wins;
an empty final `id=` clears the candidate. See [attributes](attributes.md) for
all attachment sites, and [block identifiers](block-identifiers.md) for the
`#id#` declaration on other blocks.

## Automatic anchors

```markdown
# Hello, World!

# Hello, World!

# !!!
```

The anchors are `hello-world`, `hello-world-1`, and `section`. Generation runs
in source order after explicit anchors have been reserved throughout the
document, so a later explicit declaration can cause an earlier automatic one
to receive a suffix. Explicit duplicates are retained without diagnostics.

The base is computed from parsed heading content:

1. Project visible inline content to plain text. Formatting, links, images,
   spans, and directive labels contribute their child text; text, code, and
   formulas contribute their literals. Breaks contribute spaces. Cross links
   and embeds contribute their label when present, otherwise their target
   text. Bibliography citations contribute affixes and `@key`; specimen
   citations contribute `@id`. HTML, comments, and footnote calls contribute
   nothing.
2. Apply Unicode 17 simple lowercase mapping, without normalization or full
   case-fold expansion.
3. Replace each Unicode `White_Space` scalar with one hyphen, without merging
   adjacent replacements.
4. Retain letters, numbers, combining marks, connector punctuation, hyphens,
   and underscores; remove everything else.
5. Use `section` if the result is empty.

For a collision, append `-N` with the smallest available positive integer.
Leading digits are retained. `A.B` becomes `ab`; `:tada:` becomes `tada`, with
no emoji expansion. Generated strings add no source position.

## Implicit heading references

Reference a heading by its authored text:

```markdown
# First chapter

[First chapter] [First chapter][] [go there][First chapter]
```

All three links resolve to `#first-chapter`. References may appear before the
heading. The heading label excludes its opening/closing marker and attached
attributes, then follows ordinary reference-label normalization. Inline markup
remains part of that label: `# *Foo*` is referenced by `[*Foo*]`, not `[Foo]`.

An explicit link definition wins over a heading with the same normalized label.
Repeated heading labels target the first heading in source order. Labels that
cannot be expressed as reference labels contribute no implicit definition.
Reference images can use these definitions too. Attributes on a heading are
not inherited by its implicit references.

## Anchor ownership

An occurrence's non-null explicit anchor wins over an inherited reference
anchor. Inherited attributes change semantic values, never the occurrence's
source range. An unused reference definition reserves no anchor; its resolved
occurrences do. Two explicit declarations may keep the same anchor.

Cross-link anchors remain raw; the application chooses how to match them to
these declarations. See [cross links](cross-links.md#headings-and-blocks) and
the [heading-resolution architecture](../../architecture/heading-resolution.md)
for the syntax and implementation boundaries.
