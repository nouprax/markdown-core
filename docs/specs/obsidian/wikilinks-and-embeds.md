# OFM wikilinks and embeds

Status: normative target module. Authority:
[Internal links](https://obsidian.md/help/links) and
[Embed files](https://obsidian.md/help/embeds) at the snapshot pinned by the
[OFM contract index](../obsidian-flavored-markdown.md).

## Syntax

```text
wiki-link        = [ "!" ] "[[" wiki-target [ "|" label ] "]]"
wiki-target      = [ vault-path ] [ heading-anchor / block-anchor ]
heading-anchor   = "#" heading-part *( "#" heading-part )
block-anchor     = "#^" block-id
block-id         = 1*( ASCII-letter / DIGIT / "-" )
```

At least one of `vault-path`, `heading-anchor`, or `block-anchor` must be
non-empty. One link cannot contain a source newline or another `[[`/`]]`
delimiter pair. Paths may contain spaces, Unicode, filename extensions, and
`/`-separated components rooted at the vault.

The first unescaped `|` after the target begins `label`. Inside a GFM table,
the structural pipe must be authored as `\|`; the integration contract defines
when that escape is removed. A leading `!` changes only the `embedded` field and
does not select another scanner or node kind.

## AST

`wiki-link` names the source grammar only. The consumer node is `CrossLink`: a
structured, resolver-dependent reference into the workspace address space. It
may cross into another document or target a heading/block in the current
document; “cross” describes that address relationship, not a requirement that
its destination's `path` be non-empty. The shared
[`Destination`](../destinations.md) enum also represents an ordinary Markdown
`Link` URL without collapsing the two node kinds.

```text
CrossLink(
  embedded: Bool,
  dest: Destination,
  label: String?,
  scope: Scope
)
```

Every `CrossLink.dest` is either `Destination.cross(path)` or
`Destination.anchor(path, value)`.
The invariants are:

- A target with no anchor suffix constructs `cross`; its `path` is non-empty.
- A target with either anchor suffix constructs `anchor`. Its `path` excludes
  the suffix, label, and outer delimiters and may be empty for the current
  document.
- A heading spelling removes the first `#` but retains later hierarchy
  separators. `[[Note#Parent#Child]]` stores `value == "Parent#Child"`.
- A block spelling removes `#^`. `[[Note#^block-id]]` stores
  `value == "block-id"`. It does not produce another destination or anchor kind.
- `label == null` means no `|` was authored. An authored empty label is the
  distinct empty string.
- `scope` covers the optional `!`, both delimiter pairs, and all enclosed
  source. Field values do not include delimiter bytes.
- A `CrossLink` has no parsed children. `label` is an authored label/parameter,
  not another inline Markdown container.

`dest` is the complete outgoing value. `Destination.anchor.value` names the
declaration-side `Markup.anchor` sought on a target node; it does not declare an
anchor on the `CrossLink` itself. Heading and block punctuation affects source
recognition but is intentionally absent from the consumer value.

The parser does not slug, URL-decode, case-fold, resolve, or validate a path.
For a note, an anchor suffix may address a heading or an explicitly identified
block. For a PDF or another file kind, raw values such as `page=3`,
`height=400`, or `outline` are interpreted by the downstream resolver. The
parser must not guess target kind from a filename extension.

For a normal link, `label` is the custom visible label. For an image embed, a
decimal `W` or `WxH` label is the documented size parameter. That interpretation
is also downstream because the parser has no resolved file kind.

## Recognition and fallback

Wikilinks are recognized in ordinary inline content, including headings,
paragraphs, table cells, callout titles, callout bodies, and `Footnote` content.
Inline code, comments, and source bytes owned by an HTML token are opaque;
paired inline HTML tags do not suppress recognition between them. A
backslash-escaped opening bracket does not start a wikilink.

An empty target, invalid block identifier, nested opener, source newline, or
missing `]]` makes the opener ordinary text. Failure consumes no later closing
bracket that could belong to inherited Markdown syntax. The official warning
that certain filename characters “may not work” is a vault-resolution warning,
not a parser rejection rule.

The scanner runs once from the shared inline cursor. It constructs one
`Destination.cross` or `Destination.anchor` while recognizing the label;
bindings and renderers may not rescan the completed source literal.

## Required conformance cases

| Input | Required fields |
| --- | --- |
| `[[Note]]` | `embedded=false`, `dest=cross(path="Note")`, `label=null` |
| `[[#Heading]]` | `dest=anchor(path="", value="Heading")` |
| `[[Folder/Note#Parent#Child|Label]]` | `dest=anchor(path="Folder/Note", value="Parent#Child")`, `label="Label"` |
| `![[Note#^block-id]]` | `embedded=true`, `dest=anchor(path="Note", value="block-id")` |
| `![[Image.png|100x145]]` | raw `label="100x145"`; no media-type inference |
| `![[Document.pdf#page=3]]` | `dest=anchor(path="Document.pdf", value="page=3")` |
| `[[Note|]]` | `label=""`, distinct from no label delimiter |

Tests must also cover every malformed boundary above, all opaque contexts,
source scopes, escaped table pipes, allocation failure at every node/string
allocation, and size-doubling inputs made from `!`, `[`, `]`, `#`, `^`, and `|`.
