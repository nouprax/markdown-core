# Obsidian Flavored Markdown target contract

Status: target contract for the planned Obsidian profile. The current parser
does not yet implement every requirement in this document. The implementation
plan is
[`docs/plans/2026-09-02-obsidian-flavored-markdown.md`](../plans/2026-09-02-obsidian-flavored-markdown.md).

This document defines the source-to-AST behavior Markdown Core must provide for
Obsidian Flavored Markdown (OFM). It is deliberately narrower than “whatever
Obsidian happens to accept”: a requirement enters this contract only when it is
stated by the official help pages in the source closure below or is an explicit
Markdown Core model decision needed to make such a statement unambiguous.

## Authority and source closure

The normative Obsidian source is the official
[Obsidian Flavored Markdown page](https://obsidian.md/help/obsidian-flavored-markdown),
read at `obsidianmd/obsidian-help` commit
[`d780d6b48a92ee6a150304b40ee888f322bf43bf`](https://github.com/obsidianmd/obsidian-help/tree/d780d6b48a92ee6a150304b40ee888f322bf43bf).
Its linked syntax pages, plus the embedded-search link reached from the embeds
page, complete the source closure:

- [Basic formatting syntax](https://obsidian.md/help/syntax), specifically
  highlights, footnotes, comments, code blocks, task lists, and external image
  dimensions;
- [Internal links](https://obsidian.md/help/links), including heading links,
  block links, block identifiers, aliases, and vault-relative paths;
- [Embed files](https://obsidian.md/help/embeds), including note, heading,
  block, image, audio, PDF, canvas, list, and search embeds;
- [Search](https://obsidian.md/help/plugins/search), specifically embedded
  search results expressed as a `query` fenced code block;
- [Callouts](https://obsidian.md/help/callouts), including titles, folding,
  nesting, case-insensitive types, aliases, and custom types;
- [Advanced formatting syntax](https://obsidian.md/help/advanced-syntax),
  specifically tables and `$`/`$$` MathJax delimiters.

The commit-pinned source files are
[`Obsidian Flavored Markdown.md`](https://github.com/obsidianmd/obsidian-help/blob/d780d6b48a92ee6a150304b40ee888f322bf43bf/en/Editing%20and%20formatting/Obsidian%20Flavored%20Markdown.md),
[`Basic formatting syntax.md`](https://github.com/obsidianmd/obsidian-help/blob/d780d6b48a92ee6a150304b40ee888f322bf43bf/en/Editing%20and%20formatting/Basic%20formatting%20syntax.md),
[`Advanced formatting syntax.md`](https://github.com/obsidianmd/obsidian-help/blob/d780d6b48a92ee6a150304b40ee888f322bf43bf/en/Editing%20and%20formatting/Advanced%20formatting%20syntax.md),
[`Internal links.md`](https://github.com/obsidianmd/obsidian-help/blob/d780d6b48a92ee6a150304b40ee888f322bf43bf/en/Linking%20notes%20and%20files/Internal%20links.md),
[`Embed files.md`](https://github.com/obsidianmd/obsidian-help/blob/d780d6b48a92ee6a150304b40ee888f322bf43bf/en/Linking%20notes%20and%20files/Embed%20files.md),
[`Search.md`](https://github.com/obsidianmd/obsidian-help/blob/d780d6b48a92ee6a150304b40ee888f322bf43bf/en/Plugins/Search.md),
and
[`Callouts.md`](https://github.com/obsidianmd/obsidian-help/blob/d780d6b48a92ee6a150304b40ee888f322bf43bf/en/Editing%20and%20formatting/Callouts.md).

OFM inherits CommonMark and GFM. CommonMark behavior remains owned by the
repository's pinned cmark/CommonMark 0.31.2 oracle; GFM tables,
strikethrough, autolinks, and the base task-list behavior remain owned by the
GFM 0.29 specification and the pinned cmark-gfm oracle. The official help
page's LaTeX link describes the expression language, not another Markdown
delimiter grammar. Markdown Core captures the expression body and does not
parse TeX or MathJax commands.

The executable OFM oracle is
`@quartz-community/remark-obsidian@0.2.4`. It is supplementary evidence, not a
replacement for the official documentation. Its exact authority boundary and
known gaps are registered in
[`specs/oracles/obsidian/deltas.json`](../../specs/oracles/obsidian/deltas.json).
An oracle-only construct, such as its tag syntax, does not enter this contract.

## Scope

Markdown Core parses one document without a vault, filesystem, renderer, CSS,
MathJax runtime, Prism runtime, or Obsidian settings database. It must preserve
the syntax needed by those consumers, but it must not claim to:

- resolve a shortest path, create a missing note, update renamed links, or
  decide whether a destination exists;
- transclude another file, choose a media player, render a PDF page, execute a
  search, or render a Mermaid diagram;
- choose a callout icon/color or decide whether a fold is currently open;
- validate TeX, run MathJax, or perform syntax highlighting; or
- implement editor-only completion queries such as the transient `[[## text]]`
  and `[[^^text]]` searches.

Those are resolution, rendering, or editor operations. The parser owns the
authored node, its semantic fields, and its source scope.

## Current support audit

The audit is against `main` at `f7ed6bf2`, using the CLI's explicit
`--profile gfm-extended` plus the existing external parity gates. “Partial”
means that at least one form linked by the official OFM page is not
representable by the current AST.

| Official requirement | Current behavior | Status |
| --- | --- | --- |
| CommonMark and GFM base syntax | Pinned cmark, cmark-gfm, and remark gates cover the inherited layers. | present |
| Do not parse Markdown inside HTML elements | Block HTML is opaque, but `<span>**bold**</span>` still produces `Strong` between two `HTML` nodes. | partial |
| `[[Link]]`, paths, headings, blocks, and display text | The complete spelling is one `Text` node. | missing |
| `![[Link]]` and the documented file/embed forms | The complete spelling is one `Text` node. | missing |
| Paragraph, structured-block, and list-item block identifiers | `^id` remains paragraph text and is not attached to a block. | missing |
| Referenced footnotes `[^id]` | `FootnoteReference` and `FootnoteDefinition` are present. | present |
| Inline footnotes `^[text]` | The complete spelling remains text. | missing |
| Inline and block `%%` comments | Delimiters and body remain ordinary Markdown content. | missing |
| `~~text~~` | Produces `Strikethrough`. | present |
| `==text==` | The complete spelling remains text. | missing |
| Fenced and indented code blocks | Produces `CodeBlock`, including the info/language string. | present |
| `- [ ]`, `- [x]`, and any other completion character | Space, `x`, and `X` work; `?`, `-`, and other markers remain text, and `checked: Bool?` cannot preserve the marker. | partial |
| Callouts, titles, folding, custom types, and nesting | Produces an ordinary `BlockQuote` whose first text begins `[!type]`. | missing |
| Obsidian tables, including a two-hyphen delimiter cell | Produces typed `Table`, `TableRow`, and `TableCell` nodes. | present |
| Image width and `width x height` parameters | The parameter remains part of image alt text or wikilink text. | missing |
| `$inline$` and `$$display$$` MathJax containers | Produces `Formula`/`FormulaBlock` for the documented forms. | present |
| Mermaid and embedded-search fenced blocks | Produces a `CodeBlock` with language `mermaid` or `query`; execution/rendering is correctly out of scope. | present |

The missing surface is therefore: wikilinks/embeds, block identifiers and
references as structured data, inline footnotes, Obsidian comments,
highlights, non-space task markers, callouts, image dimensions, and inline-HTML
suppression. Existing support must remain one shared implementation for the
inherited CommonMark/GFM/table/strikethrough/code/math behavior; the Obsidian
profile must not fork those algorithms.

## Documented acceptance grammar

This section freezes the valid, documented subset. It does not turn the help
site's advisory examples into claims about every malformed delimiter. Unless a
fallback is stated below, input outside this subset follows the inherited
CommonMark/GFM grammar and is ordinary text.

### Wikilinks and embeds

```text
wiki-link       = [ "!" ] "[[" wiki-target [ "|" display ] "]]"
wiki-target     = [ vault-path ] [ fragment-subpath / block-subpath ]
fragment-subpath = "#" fragment *( "#" fragment )
block-subpath   = "#^" block-id
block-id        = 1*( Latin-letter / DIGIT / "-" )
```

- At least one of `vault-path`, `fragment-subpath`, or `block-subpath` must be
  non-empty. The source examples permit spaces, Unicode, filename extensions,
  and `/`-separated paths rooted at the vault. Newlines and `[[`/`]]` are not
  part of one link.
- The first `|` after the target begins display text. In a GFM table source the
  same delimiter is written `\|`; table unescaping must not split the cell
  before the wikilink scanner sees it.
- A leading `!` sets `embedded=true`; it does not select a different scanner.
  A single node model covers note, heading, block, image, audio, PDF, canvas,
  and list embeds. File kind and vault resolution remain downstream.
- A fragment subpath such as `#Heading#Child` retains every documented
  component and is stored as `Heading#Child`: the first delimiter is excluded
  and later hierarchy separators are retained. For a note it addresses a
  heading; for a PDF or another file type, values such as `page=3` and
  `height=400` are interpreted downstream. A block subpath is stored without
  the syntactic `#^`. The parser does not slug, URL-decode, case-fold, resolve,
  or reinterpret either value.
- `display` is preserved as authored, without the `|`. For a normal wikilink it
  is display text. For an image embed, a decimal `W` or `W x H` display value is
  the documented pixel-size parameter; interpretation may occur only after a
  consumer knows that the target is an image.
- `[[#Heading]]` and links to a path with or without `.md` are valid. The help
  site's warning that some filename characters “may not work” is advisory and
  is not a parser rejection rule.

The AST representation is one inline `WikiLink` kind:

```text
WikiLink(
  embedded: Bool,
  path: String,
  subpathKind: none | fragment | block,
  subpath: String?,
  display: String?,
  scope: Scope
)
```

`subpath == null` exactly when `subpathKind == none`; absent and empty display
text remain distinct. A wikilink has no parsed child content because the
official syntax defines display text as a link label, and the selected oracle
also exposes it as a string.

### Block identifiers

A block identifier is metadata on the addressed block, not a sibling paragraph
and not visible inline content.

- A simple paragraph accepts one ASCII space followed by `^block-id` at the end
  of its final line. The marker is removed from paragraph content.
- A structured block (list, block quote, callout, or table) accepts a line
  containing only `^block-id` when that line is separated from the structured
  block and following content by blank lines. The identifier attaches to the
  preceding structured block and does not create a paragraph.
- A specific list item accepts one ASCII space followed by `^block-id` at the
  end of its bullet-point content line; the marker is removed from visible item
  content and the identifier attaches to that item.
- Quotation, callout, and table internals are not separately addressable. The
  identifier belongs to the whole structured block.
- Identifiers consist only of Latin letters, digits, and hyphens, exactly as
  the official help page states. The stored value excludes `^`.

The addressable set in this contract is `Paragraph`, `ListItem`, `List`,
`BlockQuote`, `Callout`, and `Table`; headings use heading fragments instead.
Each addressable kind owns one optional `blockIdentifier: String?` field. The
same field and attachment algorithm cover every placement above; there is no
repair-up callback or separate post-parse side table.

### Footnotes

The existing `[^id]` call and `[^id]:` definition model remains unchanged. In
addition, `^[inline content]` creates an inline `InlineFootnote` with parsed
inline content and a scope covering the caret and brackets. It has no authored
identifier and must not invent one; numbering is renderer state.

### Comments

`%%` opens a comment and the next `%%` closes it. The body may be inline or span
lines. A comment is `standalone` only when its opening and closing delimiter
each occupy their own line apart from optional spaces; every other placement is
`embedded`. Comment bodies are opaque: Markdown, OFM delimiters, and block
markers inside them are not parsed. An unmatched opener is ordinary text.
Comment recognition is disabled inside code and HTML.

The lossless AST form is `Comment(mode: embedded | standalone, literal,
scope)`, where `literal` excludes delimiters and preserves line endings. The
Obsidian preset enables `stripObsidianComments`, so its default result omits
these nodes just as Reading view omits comments. Turning stripping off retains
the same recognized nodes; it does not reinterpret the delimiters as text.

### Highlights

Exactly two unescaped equals signs open and close a highlight. A valid
highlight has non-empty content. Its body is parsed as inline content, producing
`Highlight(content, scope)`; the scope covers both delimiter runs. Code,
comments, and HTML suppress recognition. An unmatched run is text.

Nested inline parsing is a Markdown Core model decision: it preserves the
compositional AST invariant used by emphasis, strong, strikethrough, links, and
table cells. The selected oracle currently emits one text child instead; that
shape is not allowed to override the repository's shared AST model.

### Tasks

The task prefix is `[c]` at the beginning of a list item's first paragraph,
where `c` is exactly one Unicode code point. ASCII space means incomplete;
every other character means complete. The AST stores the one source of truth:

```text
ListItem(taskMarker: String?, content, blockIdentifier, scope)
```

`null` means a non-task item, `" "` means incomplete, and every other value
means complete. Platform convenience properties may derive `checked` from
`taskMarker`, but no wire or AST payload may store both facts independently.

### Callouts

A callout begins only when a block quote's first content line starts with
`[!type]`. `type` is non-empty ASCII letters, digits, underscores, or hyphens;
matching and built-in aliases are ASCII case-insensitive. An unsupported type
is still a valid custom callout and retains its lowercased type. The optional
`+` or `-` must immediately follow `]`; `+` means initially expanded and `-`
means initially collapsed. Remaining content on the marker line is the custom
title. A marker with no remaining text has no custom title. The type character
set is a Markdown Core malformed-marker boundary: the help page defines type
identifiers and shows hyphenated custom identifiers but does not publish a
complete error grammar.

```text
Callout(
  type: String,
  fold: none | expanded | collapsed,
  title: [Markup]?,
  content: [Markup],
  blockIdentifier: String?,
  scope: Scope
)
```

The title, when present, is inline content; the body is block content. Both may
be empty. A title-only callout is valid. Callouts nest through ordinary block
quote nesting, and the body may contain all Markdown/OFM inline constructs and
embeds. Built-in type-to-icon aliases are renderer data and do not change the
stored type.

### Tables, images, math, and HTML

- OFM tables use the existing typed table model. Outer pipes are optional, a
  delimiter cell contains at least two hyphens, colons select alignment, and
  basic inline formatting is valid in cells. A `|` that belongs to a wikilink
  display value or image size must be escaped in table source.
- Standard external images continue to use `Image`. A label of `W`,
  `alt|W`, or `alt|W x H`, with decimal components and no spaces around `x`,
  sets optional pixel `width` and `height` fields and removes the size suffix
  from parsed alt content. A width without height preserves the source aspect
  ratio at render time. The AST does not fetch the image.
- `$...$` produces embedded `Formula`; `$$...$$` produces standalone
  `Formula` or `FormulaBlock` according to the existing canonical formula
  placement rules. The body is an opaque string. The OFM contract adds no
  renderer and no TeX AST.
- Fenced `mermaid` and `query` input remains a `CodeBlock` with the authored
  language. Diagram rendering and search execution are downstream operations.
- Markdown and OFM constructs are not recognized inside HTML elements. Block
  HTML remains one `HTMLBlock`. Between a matched inline HTML opening and
  closing tag, tags remain `HTML` nodes and intervening bytes remain literal
  `Text`; for example `<span>**bold**</span>` contains no `Strong`. The inline
  tag stack must handle nesting and must not change inherited behavior for an
  unmatched tag.

## Precedence and failure behavior

The grammar uses one block pass and one inline pass, preserving the existing
CommonMark architecture. Within those passes:

1. fenced/indented code and block HTML are opaque;
2. block quotes/lists/tables are recognized before callout and block-identifier
   refinement;
3. inline code, inline HTML regions, and comments are opaque to all other
   inline extensions;
4. wikilinks are recognized before GFM table cells split on an escaped alias
   pipe;
5. the remaining inline constructs use the shared delimiter stack and never a
   whole-document regex or input-size/cardinality branch.

An unclosed delimiter, invalid block identifier, or callout marker in any
position other than the first block-quote content line falls back to inherited
Markdown text. Parsing remains deterministic, linear in source bytes plus AST
output, stack-safe for adversarial nesting, and allocation-failure strict under
the existing parser contract.

## Options and conformance

The implementation introduces an `obsidian` preset/profile; it does not change
the existing `default`, `commonmark`, `gfm`, or `gfm-extended` meanings in a
minor release. The preset enables the existing GFM, footnote, formula, and code
behavior plus wikilinks, block identifiers, inline footnotes, comments,
highlights, custom task markers, callouts, image dimensions, and HTML-content
suppression. `stripObsidianComments` is true in that preset.

Each new semantic node/field must enter the canonical JSON contract, prose and
dump grammar together, then every C/Swift/Kotlin/ES facade, exhaustive visitor,
walker, wire decoder, and shared conformance fixture in the same change. The
official examples own the product goldens. The external OFM oracle compares
only the intersection it actually parses; every intentional model delta and
every unsupported oracle area remains explicit and fail-closed.
