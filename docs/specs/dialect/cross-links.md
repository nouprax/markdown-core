# Cross links and embeds

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Put a workspace target between double brackets to link to another document.

```markdown
Read [[Getting started]] and [[Folder/Note|the note]].
```

These produce `CrossLink` nodes. The first has path `Getting started` and no
label. The second has path `Folder/Note` and label `the note`. The parser stores
these strings; the application finds the resource and chooses a display label.

Labels are raw strings, not inline Markdown. `[[Note|**bold**]]` keeps the
asterisks in the label. A missing label is null; an authored empty label in
`[[Note|]]` is `""`. Paths and labels are not trimmed, decoded, or case-folded.

## Headings and blocks

```markdown
[[Note#A heading]]

[[#A heading]]

[[Note#Parent#Child|a subsection]]

[[Note#^block-id]]
```

The destinations store these path/anchor pairs:

| Source | Path | Anchor |
| --- | --- | --- |
| `[[Note#A heading]]` | `Note` | `A heading` |
| `[[#A heading]]` | Empty string | `A heading` |
| `[[Note#Parent#Child]]` | `Note` | `Parent#Child` |
| `[[Note#^block-id]]` | `Note` | `block-id` |

An empty path addresses the current document. Heading anchors retain their
spelling, including interior `#` separators. Block anchors accept ASCII letters,
digits, and hyphens and must consume the rest of the target. Otherwise the
heading form is tried: `[[A#^id#x]]` stores anchor `^id#x`.

The block reference addresses a [declaration](block-identifiers.md) written
`#block-id#`. Target anchors do not populate the reference node's own `anchor`
field. Heading matching is consumer policy: `[[#My Heading]]` retains
`My Heading`, even though the automatic heading anchor is `my-heading`.

## Embeds

Put `!` immediately before the opening brackets:

```markdown
![[Note]]

![[movie.mp4|raw *caption*|320x180]]
```

These become `CrossEmbedded`, requesting transclusion. The second stores a raw
label of `raw *caption*` and dimensions of 320 by 180. The kind of resource,
loading, playback, and transclusion are determined by the application.

## Dimensions

An embed label can be `W`, `WxH`, `label|W`, or `label|WxH`, using the same
positive 32-bit integer grammar as [image dimensions](links-and-images.md#image-dimensions).
A valid suffix is removed from the raw label. A size-only label becomes empty,
rather than null. Only the last label separator is considered, and whitespace
immediately before it invalidates the size suffix.

Malformed sizes stay in the label. Ordinary cross links never interpret sizes:
`[[Note|320x180]]` has that complete label and no dimensions field. The filename
and target anchor do not affect size recognition.

## Pipes and tables

Both `|` and `\|` can separate a target from its label. In a pipe table, use
the escaped spelling to avoid starting another cell:

```markdown
| Link | Embed |
| --- | --- |
| [[Note\|Read more]] | ![[asset\|320x180]] |
```

The row contains two cells with the intended cross link and embed. The escaped
separator's backslash is not stored. Other backslashes inside cross links stay
literal; there is no general escape or entity-decoding pass.

## Incomplete targets

A path or nonempty anchor is required. Empty heading parts, line endings, or
brackets before the closing `]]` invalidate the candidate. `[[]]`, `[[#]]`,
and `[[A##B]]` therefore fall back to ordinary Markdown. Escaping the first `[`
prevents recognition; escaping `!` leaves a literal exclamation mark followed
by an ordinary cross link.

A recognized cross link ends at `]]`. A following destination, reference tail,
or attribute container does not attach. Source inside code, formulas, comments,
HTML tokens, and automatic URL tokens remains owned by those constructs.
