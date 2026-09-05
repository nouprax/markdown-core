# Block identifiers

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `blockIdentifiers` (default `false`). Source: Obsidian's block
identifiers. Executable oracle: none; the Obsidian package does not parse
them, so product fixtures are the oracle of record. Landing: `O7`.

## Model

A block identifier populates the universal `anchor` field of the
[anchors](anchors.md) module on the block it addresses, without the caret and
without a discriminator. It is never a node and never remains visible content
after recognition. The `#^id` reference spelling belongs to the
[cross links](cross-links.md) module.

```text
block-id = 1*( ASCII-letter / DIGIT / "-" )
```

Whitespace, underscores, other punctuation, and non-ASCII letters make a
candidate invalid.

## Placements

Attachment happens while the owning block is finalized, never in a
document-wide repair pass or an offset side table. Three placements exist,
and their owner set is exactly `Paragraph`, `ListItem`, `List`, `Callout`,
and `Table`; headings are addressed through heading anchors, and code blocks,
rows, cells, and callout parts are not addressable.

1. Paragraph suffix. The rule matches when the paragraph's last line, after
   trailing spaces and tabs are removed, ends with `^block-id` preceded by
   one or more spaces or tabs, or consists solely of `^block-id` at
   indentation zero to three while the paragraph has an earlier line. `\^`
   never starts an identifier. The removed bytes are the identifier, its
   caret, the preceding whitespace, and in the own-line form the preceding
   line ending. The rule is lexical on the final line's bytes: only
   block-level owners such as a code block protect a candidate, while inline
   code, inline HTML, and inline comments on that line do not.
2. Structured block line, block-start step 15. A line indented zero to three
   spaces whose content is `^block-id` optionally followed by spaces or tabs,
   preceded by one or more blank lines and followed by one or more blank
   lines or the end of the document, attaches to the last block before those
   blank lines when that block is a `List`, `Callout`, or `Table`. The line,
   the blank lines, and the owner are direct content of the same container;
   for nested lists the owner is the outermost list at that level. When a
   table owns a following caption, the identifier line after the caption
   attaches to the `Table`. Otherwise the line is a paragraph.
3. List item suffix. When an item's first block is a `Paragraph` and the
   candidate is on the marker line, the identifier attaches to the `ListItem`
   and the `Paragraph` keeps `anchor=null`; otherwise placement 1 applies to
   the paragraph that owns the final line.

Callout metadata is extracted before attachment, so a candidate on a metadata
line is title text. A candidate attaches only when the owner's anchor is
still `null` when finalization reaches it; otherwise its bytes are ordinary
content. The owner's `anchor` receives the identifier value; the consumer
value is indistinguishable from the same anchor produced by another rule.

## Option behavior and fallback

With `blockIdentifiers=false`, every candidate is ordinary content. With the
option on, a caret without a valid non-empty identifier is text, trailing
non-space bytes after an identifier make the candidate ordinary content, and
a structured-block line without both required blank boundaries is a
paragraph. Identifier-like bytes inside code, an HTML block, a comment, or a
cross link are those constructs' bytes.

## Scopes

Successful recognition removes the separating whitespace, caret, and
identifier from visible content; the owner's scope still covers the complete
authored construct including the identifier, child scopes end before an
inline suffix, and a detached line contributes to the owner's scope although
it produces no child. Allocation failure unwinds through the owner's path and
leaves no detached identifier.

## Required conformance cases

Tests cover paragraph suffixes with tabs and trailing whitespace, the
own-line form, escaped carets, item suffixes, whole-list, callout, and table
identifiers including a table with a caption, metadata-free and
metadata-bearing callouts, nested lists, invalid characters, missing
separation, a second candidate on one owner, an owner whose anchor another
rule already set, a caret before superscript parsing, end of document, exact
scopes, option-off output, allocation failure, and long identifier and
candidate sequences.
