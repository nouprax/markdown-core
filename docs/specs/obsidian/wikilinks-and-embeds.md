# OFM wikilinks and embeds

Status: normative target module. Authority:
[Internal links](https://obsidian.md/help/links) and
[Embed files](https://obsidian.md/help/embeds) at the snapshot pinned by the
[OFM contract index](../obsidian-flavored-markdown.md).

## Syntax

```text
wiki-link        = [ "!" ] "[[" wiki-target [ "|" label ] "]]"
wiki-target      = [ vault-path ] [ fragment-route / block-route ]
fragment-route   = "#" fragment *( "#" fragment )
block-route      = "#^" block-id
block-id         = 1*( ASCII-letter / DIGIT / "-" )
```

At least one of `vault-path`, `fragment-route`, or `block-route` must be
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
`path` be non-empty. An ordinary Markdown `Link` already owns a destination and
does not become a `CrossLink`.

```text
CrossLink(
  embedded: Bool,
  path: String,
  route: none | fragment | block,
  dest: String?,
  label: String?,
  scope: Scope
)
```

The invariants are:

- `route == none` exactly when `dest == null`.
- `path` excludes route, label, and outer delimiters. It may be empty for a
  same-document fragment such as `[[#Heading]]`.
- `fragment` removes the first `#` but retains later hierarchy separators. For
  `[[Note#Parent#Child]]`, `dest == "Parent#Child"`.
- `block` removes `#^`. For `[[Note#^block-id]]`,
  `dest == "block-id"`.
- `label == null` means no `|` was authored. An authored empty label is the
  distinct empty string.
- `scope` covers the optional `!`, both delimiter pairs, and all enclosed
  source. Field values do not include delimiter bytes.
- A `CrossLink` has no parsed children. `label` is an authored label/parameter,
  not another inline Markdown container.

`dest` is the reference-side selector inside `path`; the complete destination
is the `(path, route, dest)` tuple. It never declares an anchor on the
`CrossLink` itself. After workspace resolution, a fragment or block `dest` may
match the declaration-side `Markup.anchor` of a target node, but equal strings
do not make the two fields the same semantic fact.

The parser does not slug, URL-decode, case-fold, resolve, or validate a path.
For a note, a fragment normally addresses a heading. For a PDF or another file
kind, raw values such as `page=3`, `height=400`, or `outline` are interpreted by
the downstream resolver. The parser must not guess target kind from a filename
extension.

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

The scanner runs once from the shared inline cursor. It records path, route,
`dest`, and label during recognition; bindings and renderers may not rescan
the completed source literal.

## Required conformance cases

| Input | Required fields |
| --- | --- |
| `[[Note]]` | `embedded=false`, `path="Note"`, `route=none`, `dest=null`, `label=null` |
| `[[#Heading]]` | `path=""`, `route=fragment`, `dest="Heading"` |
| `[[Folder/Note#Parent#Child|Label]]` | `dest="Parent#Child"`, `label="Label"` |
| `![[Note#^block-id]]` | `embedded=true`, `route=block`, `dest="block-id"` |
| `![[Image.png|100x145]]` | raw `label="100x145"`; no media-type inference |
| `![[Document.pdf#page=3]]` | `route=fragment`, `dest="page=3"` |
| `[[Note|]]` | `label=""`, distinct from no label delimiter |

Tests must also cover every malformed boundary above, all opaque contexts,
source scopes, escaped table pipes, allocation failure at every node/string
allocation, and size-doubling inputs made from `!`, `[`, `]`, `#`, `^`, and `|`.
