# OFM block identifiers

Status: normative target module. Authority:
[Internal links — Link to a block](https://obsidian.md/help/links#Link%20to%20a%20block%20in%20a%20note)
at the snapshot pinned by the
[OFM contract index](../obsidian-flavored-markdown.md).

This module defines authored block identifiers. The `#^id` route inside a
wikilink belongs to the wikilink module.

## Identifier grammar

```text
block-id = 1*( ASCII-letter / DIGIT / "-" )
```

`ASCII-letter` is `A` through `Z` or `a` through `z`. The stored value excludes
the leading `^`. Empty identifiers and identifiers containing whitespace,
underscore, punctuation other than `-`, or non-ASCII letters are invalid.

## Placements and ownership

An identifier is metadata on the block it addresses. It is never a sibling AST
node and never remains visible content after successful recognition.

1. A paragraph accepts at least one ASCII space followed by `^block-id` at the
   end of its final source line. The suffix attaches to that `Paragraph`.
2. A complete structured block (`List`, `Callout`, or `Table`) accepts a line
   containing optional ASCII spaces and exactly `^block-id`. There must be a
   blank line between the block and identifier line and another blank line or
   end-of-document after it. The line attaches to the preceding structured
   block and produces no paragraph.
3. A specific `ListItem` accepts at least one ASCII space followed by
   `^block-id` at the end of its bullet-point content line. The suffix attaches
   to that item rather than the containing list.

The target addressable set is exactly `Paragraph`, `ListItem`, `List`,
`Callout`, and `Table`. Headings use wikilink fragments. Code blocks, table
rows/cells, and internal pieces of a callout are not independently addressable
under this contract.

Every addressable kind owns:

```text
anchor: String?
```

The source construct remains a block identifier in Obsidian terminology;
`anchor` is the canonical AST name because the value declares a link target and
is not the node's object identity.

One block owns at most one anchor. A candidate that would assign a second
anchor is not consumed and follows ordinary paragraph/inline parsing. This is
the target model's deterministic duplicate boundary; vault-wide identifier
uniqueness checking remains outside the parser.

## Content and scope

Successful recognition removes the separating spaces, caret, and identifier
from visible content. The addressed node's scope still covers the complete
authored construct, including its identifier. Child scopes end before an inline
suffix. A detached structured-block identifier contributes to the owner's
scope even though it produces no child.

Attachment occurs while block ownership is finalized. It may not be
implemented as a document-wide repair pass or a side table keyed by offsets.
Allocation failure leaves no detached identifier and unwinds through the same
owner path as the block.

## Fallback and interactions

- A caret without a valid non-empty identifier remains text.
- Trailing non-space bytes after an identifier make the candidate ordinary
  content.
- A structured identifier without both required separation boundaries becomes
  an ordinary paragraph.
- Identifier-like bytes owned by code, an HTML token/block, a comment, or a
  crosslink source form are opaque to this module. Paired inline HTML tags do
  not suppress recognition in intervening source.
- A list-item suffix wins over whole-list attachment because it is inside the
  item's content line; a detached identifier after the list owns the list.
- A metadata-free and metadata-bearing `Callout` use the same `anchor` field.

## Required conformance cases

Tests must cover paragraph suffixes, human-readable identifiers, item suffixes,
whole-list/callout/table identifiers, metadata-free callouts, nested lists,
invalid characters, missing separation, duplicate candidates, end-of-document,
source scopes, allocation failure, and long identifier/candidate sequences.
