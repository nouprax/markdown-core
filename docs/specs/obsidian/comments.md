# OFM comments

Status: normative target module. Authority:
[Basic formatting syntax — Comments](https://obsidian.md/help/syntax#Comments)
at the snapshot pinned by the
[OFM contract index](../obsidian-flavored-markdown.md).

## Syntax and classification

`%%` opens a comment and the next unescaped `%%` closes it. The body may be on
one line or span source lines. Comment content is opaque: Markdown, OFM
delimiters, block markers, and HTML-like bytes inside it are not parsed.

A comment is placed directly in block content only when its opening and closing
delimiter each occupy their own line apart from optional ASCII spaces. Every
other valid comment is placed in the current inline content, including a
multiline comment whose opener or closer shares a line with non-space content.

## Lossless AST and stripping

```text
Comment(
  literal: String,
  scope: Scope
)
```

`literal` excludes both delimiter pairs and preserves every intervening byte,
including line endings and indentation. `scope` covers delimiters and body.
`Comment` is a contextual leaf: its parent edge records whether it occupies
block or inline content, so the node stores no duplicate mode field.

Recognition and retention are separate decisions. `stripComments` defaults to
`true` whenever `comments` is on, so the default output omits recognized comment
nodes. With stripping disabled, the same recognition produces the lossless
`Comment`; delimiters are not reinterpreted as text. Stripping must not change
how surrounding delimiters bind.

## Precedence and fallback

Inline and fenced code plus source bytes owned by an HTML token or block
suppress comment recognition. Paired inline HTML tags do not suppress comment
recognition between them. Once a comment opens, it suppresses every other inline
and block extension until its closer. An escaped `\%%` is text. An unmatched
opener is text and does not hide the remainder of the document.

The implementation scans from the shared source cursor and never searches the
whole remaining document repeatedly. Closing lookup must therefore remain
linear for long runs of `%` and for many unmatched candidates.

## Required conformance cases

Tests must cover inline, standalone, multiline inline, empty, adjacent, escaped,
and unmatched comments; correct block/inline parent placement;
Markdown/OFM-looking bodies; code and HTML; strip on/off; surrounding delimiter
binding; exact literal/scope; allocation failure in retained mode; and
size-doubling percent runs.
