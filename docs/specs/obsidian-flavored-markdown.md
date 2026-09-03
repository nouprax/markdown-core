# Obsidian syntax-extension profile contract

Status: normative target contract for the planned canonical vNext AST and
`obsidian` extension preset. The current implementation does not yet satisfy
every module. The implementation sequence is specified in
[`docs/plans/2026-09-02-obsidian-flavored-markdown.md`](../plans/2026-09-02-obsidian-flavored-markdown.md).

This file owns authority, scope, composition, and the current support audit. It
does not duplicate feature rules. Each linked module below is a normative spec,
not explanatory guidance.

## Authority and source closure

The extension-syntax authority is the official
[Obsidian Flavored Markdown page](https://obsidian.md/help/obsidian-flavored-markdown),
read at `obsidianmd/obsidian-help` commit
[`d780d6b48a92ee6a150304b40ee888f322bf43bf`](https://github.com/obsidianmd/obsidian-help/tree/d780d6b48a92ee6a150304b40ee888f322bf43bf).
Its linked syntax pages, plus the embedded-search link reached from the embeds
page, complete the reviewed source closure:

- [Basic formatting syntax](https://obsidian.md/help/syntax), specifically
  highlights, footnotes, comments, code blocks, task lists, and external image
  dimensions;
- [Internal links](https://obsidian.md/help/links), including heading links,
  block links, block identifiers, aliases, and vault-relative paths;
- [Embed files](https://obsidian.md/help/embeds), including note, heading,
  block, image, audio, PDF, canvas, and list embeds;
- [Search](https://obsidian.md/help/plugins/search), specifically embedded
  search results expressed as a `query` fenced code block;
- [Callouts](https://obsidian.md/help/callouts), including titles, folding,
  nesting, case-insensitive types, aliases, and custom types; and
- [Advanced formatting syntax](https://obsidian.md/help/advanced-syntax),
  specifically tables, Mermaid containers, and `$`/`$$` MathJax delimiters.

The exact source files and anchors are also recorded by the executable oracle
policy in
[`specs/oracles/obsidian/deltas.json`](../../specs/oracles/obsidian/deltas.json).
OFM inherits CommonMark and GFM. CommonMark remains owned by the repository's
pinned cmark/CommonMark 0.31.2 oracle; GFM tables, strikethrough, autolinks, and
base task-list recognition remain owned by GFM 0.29 and cmark-gfm. The linked
MathJax, LaTeX, Mermaid, and search-language documents define downstream body
languages, not additional Markdown parsers.

The `obsidian` profile is an extension preset, not a request to reproduce every
Obsidian parser or renderer dialect difference. It adds the source constructs
owned by the modules below while preserving inherited CommonMark/GFM behavior.
In particular, it does not adopt Obsidian's rule that suppresses Markdown
between paired inline HTML tags.

`@quartz-community/remark-obsidian@0.2.4` is supplementary implementation
evidence, not a language authority. Its exact scope and deliberate projections
are registered in the oracle policy. An oracle-only construct does not enter
this contract, and official extension syntax absent from the oracle remains
required.

## Normative module set

| Module | Sole owner of |
| --- | --- |
| [Wikilinks and embeds](obsidian/wikilinks-and-embeds.md) | `[[...]]` and `![[...]]` source forms normalized to `CrossLink`, with whole-resource or anchor `Destination` values, labels, and raw embed parameters |
| [Block identifiers](obsidian/block-identifiers.md) | `^id` definition placement, ownership, removal, and attachment to the universal anchor field |
| [Footnotes](obsidian/footnotes.md) | referenced and inline source forms normalized to one-item `Cite` values with `CitationReferent.footnote` and document-owned `Footnote` values |
| [Comments](obsidian/comments.md) | inline/standalone `%%` comments and stripping |
| [Highlights](obsidian/highlights.md) | `==...==` recognition and the `Mark` inline node |
| [Tasks](obsidian/tasks.md) | arbitrary task characters and `ListItem.marker` |
| [Callouts](obsidian/callouts.md) | the universal `Callout` quote container and optional `[!type]` metadata |
| [Inherited syntax and integration](obsidian/inherited-and-integration.md) | resolved reference links/images, tables, image sizes, math, code containers, inherited HTML behavior, and cross-feature precedence |

The module owning a construct defines its grammar, AST fields, source scopes,
fallback, and conformance cases. This index owns profile enablement and rules
that apply to the complete profile. If two modules interact, the integration
module owns the composition rule while each feature module retains its local
semantics.

The block-identifier module depends on the shared
[anchor model](anchors.md); it contributes one source attachment rule and does
not create an Obsidian-specific target identity. The wikilink and inherited-link
modules depend on the shared [destination model](destinations.md): ordinary
links use its `url` branch and `CrossLink` uses its `cross` or `anchor` branch.
The footnote module depends on the shared [citation model](citation-model.md).
Only its footnote referent and group/item shape are part of OFM; the same
contract's Pandoc `@key` source syntax remains an independent extension.

## Parser boundary

Markdown Core parses one document without a vault, filesystem, renderer, CSS,
MathJax runtime, Mermaid runtime, Prism runtime, search engine, or Obsidian
settings database. It preserves authored semantics needed by those consumers,
but does not:

- resolve a shortest path, create a missing note, update renamed links, or
  decide whether a destination exists;
- transclude another file, infer its resolved media kind, render a PDF page,
  execute a search, or render a diagram;
- choose a callout icon/color or decide whether a fold is currently open;
- validate TeX, run MathJax, or perform syntax highlighting; or
- implement editor-only completion queries such as transient `[[## text]]` and
  `[[^^text]]` searches.

These are resolution, rendering, editor, or runtime operations. The parser owns
the immutable semantic node, authored fields, source order, and source scope.

## Current support audit

This audit is against `main` at `f7ed6bf2`, using the CLI's explicit
`--profile gfm-extended` and the existing parity gates. “Partial” means that at
least one documented form is not representable by the current canonical AST.

| Official requirement | Current behavior | Status |
| --- | --- | --- |
| CommonMark and GFM base syntax | Pinned cmark, cmark-gfm, and remark gates cover the inherited layers. | present |
| Reference links and images | Source recognition is present, but the current AST exposes `LinkReference`, `ImageReference`, `ReferenceDefinition`, and `ReferenceForm` instead of resolved `Link`/`Image` values. | partial |
| `[[Link]]`, paths, headings, blocks, and labels | The complete spelling is one `Text` node. | missing |
| `![[Link]]` and documented file/embed forms | The complete spelling is one `Text` node. | missing |
| Paragraph, structured-block, and list-item block identifiers | `^id` remains paragraph text and is not attached to a block. | missing |
| Referenced footnotes `[^id]` | Source recognition is present, but the current AST exposes source-shaped `FootnoteReference` and `FootnoteDefinition` kinds instead of the target `Cite`/`Citation`/`CitationReferent`/`Footnote` model. | partial |
| Inline footnotes `^[text]` | The complete spelling remains text. | missing |
| Inline and standalone `%%` comments | Delimiters and body remain ordinary Markdown content. | missing |
| `~~text~~` | Produces `Strikethrough`. | present |
| `==text==` | The complete spelling remains text. | missing |
| Fenced and indented code blocks | Produces `CodeBlock`, including the info/language string. | present |
| `- [ ]`, `- [x]`, and any other completion character | Space, `x`, and `X` work; other markers remain text, and `checked: Bool?` cannot preserve the marker. | partial |
| Quoted blocks and `[!type]` metadata | Produces `BlockQuote`; a marker remains visible first-line text. | partial |
| Obsidian tables, including a two-hyphen delimiter cell | Produces typed `Table`, `TableRow`, and `TableCell` nodes. | present |
| Image width and `width x height` parameters | The parameter remains part of image alt text or wikilink text. | missing |
| `$inline$` and `$$display$$` MathJax containers | Produces `Formula`/`FormulaBlock` for the documented forms. | present |
| Mermaid and embedded-search fenced blocks | Produces `CodeBlock` with language `mermaid` or `query`; execution/rendering is out of scope. | present |

The missing target surface is therefore: consumer-normalized reference links
with shared `Destination.url` values and images, `CrossLink` values with
shared whole-resource/anchor destinations for wikilinks/embeds, block identifiers and
references as structured data, the unified
`Cite`/`Citation`/`CitationReferent`/`Footnote` model and inline source form,
comments, highlights, non-space task markers, the universal `Callout` model and
optional metadata, and image dimensions. Existing inherited syntax keeps one
shared implementation; the Obsidian profile must not fork those algorithms.

## Shared architecture and failure contract

The implementation uses the existing block pass and inline pass. There is one
parser, one source cursor, one node-ownership model, and one algorithm per
semantic operation. Module implementations may add scanners or typed fields,
but may not add a second document parser, renderer repair pass, binding-side
rescan, or input-shape/size branch.

Existing code and raw-HTML tokens retain ownership of their own source bytes,
but paired inline HTML tags do not create a suppressing region. A construct that
fails its module grammar falls back exactly as that module specifies; recovery
must not consume bytes belonging to later CommonMark, GFM, or OFM constructs.
Parsing is deterministic, linear in source bytes plus AST output, stack-safe
under adversarial nesting, and allocation-failure strict under the existing
parser contract.

## Profiles and canonical migration

The canonical vNext AST replaces `BlockQuote` with `Callout` for every profile,
without an alias or compatibility node. Existing `default`, `commonmark`, `gfm`,
and `gfm-extended` profiles retain their source grammar and produce
metadata-free `Callout` values for ordinary `>` containers.

For every profile with footnotes enabled, canonical vNext also replaces
`FootnoteReference` and `FootnoteDefinition` with inline `Cite`, its owned
`Citation` carrying a `CitationReferent.footnote`, document-owned `Footnote`,
and `Document.footnotes`. This is one universal consumer model; only the
`obsidian` profile adds the `^[...]` source form.

Canonical vNext resolves every successful direct, full, collapsed, shortcut,
and autolink form to `Link(dest=Destination.url(...))`, and every successful
direct or reference image to `Image`. `LinkReference`, `ImageReference`,
`ReferenceDefinition`, and `ReferenceForm` are removed from every profile
without aliases. Their labels, forms, definitions, and lookup map remain
parser-internal source machinery; they never become `Citation` or a
document-owned link registry.

The new `obsidian` profile composes the inherited GFM, footnote, formula, and
code behavior with all modules in this contract. It enables
`stripObsidianComments` by default. The universal reference-link/image
normalization, `BlockQuote` to `Callout`, source-shaped footnote to
`Cite`/`Citation`/`CitationReferent`/`Footnote`, and `checked` to `marker`
migrations are intentional canonical vNext changes and receive no duplicate
compatibility state.

Pandoc `@key` citation syntax is not part of OFM and is not enabled by the
`obsidian` profile. A separate `citations` parse extension recognizes it while
reusing the same `Cite`, `Citation`, and `CitationReferent` model; disabling
that extension does not disable footnote syntax or its `Cite` nodes carrying
`CitationReferent.footnote`.

OFM syntax recognition is enabled as one profile composition and disabled in
the existing profiles; this contract does not require a public toggle for every
individual module. Comment retention is the one independent semantic option
because it changes lossless output after the same recognition.

## Conformance obligations

Each new kind, field, enum, and option must enter
`docs/specs/canonical-ast.json`, `canonical-ast.md`, and
`canonical-ast-dump.md` together, then every C/Swift/Kotlin/ES facade,
exhaustive visitor, walker, wire decoder, dumper, and shared fixture in the same
implementation change.

Every module must provide positive examples, malformed boundaries, option-off
cases, source scopes, allocation failures, and adversarial complexity cases.
The integration module adds cross-feature cases. Official examples own product
goldens. The external OFM oracle compares only its declared intersection; every
current gap, model projection, and unsupported area remains explicit and
fail-closed.
