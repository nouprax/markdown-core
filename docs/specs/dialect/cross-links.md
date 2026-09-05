# Cross links

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `crossLinks` (default `false`). Source: Obsidian's internal links and
embeds (`[[...]]` and `![[...]]`). Executable oracle:
`@quartz-community/remark-obsidian` 0.2.4 under `specs/oracles/obsidian/`,
for the forms it parses; its label-nullability and destination projections
are registered deltas. Landing: `O1`, the first producer of
`Destination.cross`.

## Model

```text
CrossLink(embedded: Bool, dest: Destination, label: String?)
```

`CrossLink` is an inline leaf: a structured, resolver-dependent reference
into the workspace address space. Every `CrossLink.dest` is
`Destination.cross(path, anchor)`:

- A target without an anchor stores `anchor == null` and a non-empty `path`.
- A target with an anchor stores a non-empty `anchor`; its `path` excludes
  the anchor, the label, and the delimiters and may be empty, addressing the
  current document.
- `label == null` means no `|` was authored; an authored empty label is `""`.
- `embedded` is `true` if and only if the opener was `![[`. It requests
  transclusion and selects no other scanner or kind.

`path`, `anchor`, and `label` are stored exactly as written: not trimmed,
slugged, URL-decoded, case-folded, resolved, or validated. `label` is one raw
authored string, not inline content; a `CrossLink` has no children. Anchor
punctuation affects recognition only and is absent from the value.
`Destination.cross.anchor` names the declaration-side `Markup.anchor` a
consumer looks for; it declares nothing on the `CrossLink` itself.

## Syntax

Recognition is inline step A6, run at every unescaped `[[` or `![[` before
inherited bracket handling:

```text
cross-link     = [ "!" ] "[[" target [ "|" label ] "]]"
target         = [ path ] [ heading-anchor / block-anchor ]
heading-anchor = "#" heading-part *( "#" heading-part )
block-anchor   = "#^" block-id
path           = 1*path-char
heading-part   = 1*path-char
path-char      = any scalar except "#", "|", "[", "]", LF, and CR
block-id       = 1*( ASCII-letter / DIGIT / "-" )
label          = *( any scalar except "[", "]", LF, and CR )
```

- The candidate ends at the first `]]` after the opener. An unescaped `[` or
  `]` before that `]]`, or a line ending before it, makes the opener text;
  scanning resumes at the next byte, so `[[[Note]]]` is text `[`, a cross
  link `Note`, and text `]`.
- At least one of `path` and an anchor is non-empty: `[[]]`, `[[#]]`, and
  `[[|x]]` are text.
- The first `|` after the target begins the label. Inside `[[...]]` the pair
  `\|` is also the label separator: the backslash is dropped and the pipe
  separates, and no other backslash escape exists inside a cross link. A
  second `|` is label content.
- The target is the block form when `#^` immediately follows the path and a
  non-empty `block-id` runs to the `|` or `]]`; then `anchor` is the
  identifier without `#^`. Otherwise every byte after the first `#` is the
  anchor as written, so `^` is an ordinary heading byte and `[[A#^id#x]]`
  stores `anchor="^id#x"`; a heading part that is empty at any position
  (`[[Note#]]`, `[[A##B]]`, `[[A#B#]]`, `[[## text]]`) makes the opener text.
- `[[Note#Parent#Child]]` stores `anchor="Parent#Child"`;
  `[[Note#^block-id]]` stores `anchor="block-id"`; `[[^^text]]` stores
  `path="^^text"` and `anchor=null`.

`\[[Note]]` and `\![[Note]]` are inherited escapes: the first is text, the
second is a literal `!` followed by a cross link with `embedded=false`. A
recognized cross link is complete at its `]]`; a following `(`, `[`, or `{`
is text, so `[[Note]](url)` and `[[Note]]{.c}` never form a link, reference,
or span. Link content and image alt content may contain a cross link.

## Option behavior and fallback

With `crossLinks=false`, `[[`, `]]`, and `![[` follow inherited bracket
handling byte for byte. With the option on, a failed candidate consumes
nothing and the inherited rules run from the `[`. Source owned by code spans,
HTML tokens, comments, and formulas is opaque to this module, and a completed
cross link is opaque to every later step.

## Tables

While `crossLinks` is on, in every table syntax that parses cell content, a
`\|` inside a cell is not a cell boundary and reaches the inline scanner,
where it is the label separator inside `[[...]]` and the inherited escaped
pipe elsewhere. An unescaped `|` inside a cross link candidate in a table row
splits the cell, and the unmatched `[[` bytes are text. There is no
table-specific cross-link parser and the inherited delimiter-row grammar is
unchanged.

## Downstream meaning

For a note, an anchor addresses a heading or an identified block; for another
file kind, raw values such as `page=3`, `height=400`, or `outline` are
interpreted by the resolver. For an image embed, a label such as `100x145` is
a size parameter. The parser guesses no target kind from an extension and
never fetches the target.

## Scopes

`CrossLink.scope` covers the optional `!`, both delimiter pairs, and every
byte between. Field values contain no delimiter bytes.

## Required conformance cases

| Input                                | Required fields                                                         |
| ------------------------------------ | ----------------------------------------------------------------------- |
| `[[Note]]`                           | `embedded=false`, `dest=cross(path="Note", anchor=null)`, `label=null`  |
| `[[#Heading]]`                       | `dest=cross(path="", anchor="Heading")`                                 |
| `[[Folder/Note#Parent#Child|Label]]` | `dest=cross(path="Folder/Note", anchor="Parent#Child")`, `label="Label"` |
| `![[Note#^block-id]]`                | `embedded=true`, `dest=cross(path="Note", anchor="block-id")`           |
| `![[Image.png|100x145]]`             | raw `label="100x145"`; no media-type inference                          |
| `![[Document.pdf#page=3]]`           | `dest=cross(path="Document.pdf", anchor="page=3")`                      |
| `[[Note|]]`                          | `label=""`, distinct from no label                                      |

Tests also cover every malformed boundary above, `\|` inside and outside
tables, unescaped pipes in table rows, single brackets, escaped openers, all
opaque contexts, a cross link inside link and image content, a tail after
`]]`, exact scopes, option-off output, allocation failure at every node and
string, and size-doubling inputs made from `!`, `[`, `]`, `#`, `^`, and `|`.
