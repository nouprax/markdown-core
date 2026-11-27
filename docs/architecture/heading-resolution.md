# Heading resolution

The [anchors module](../specs/dialect/anchors.md) owns the language contract.
A heading's anchor is the explicit one its attributes declare, or none; the
parser computes no anchor from a heading's text. Consumers receive only the
completed document.

## Inline ownership

A heading is a leaf block whose inline content parses as a paragraph's does,
with the ordinary inline parser, at the heading's ENTER in completion's walk.
An ATX heading's closing sequence ends its text first. Its attached attribute
tail is its own unless the inline cursor actually claims the opening brace: a
code span, formula, HTML token, or comment may own it. A heading declares no
reference label: `[Title]` is a reference only when a `Reference` declares
`Title`, and the published document resolves a `reference` destination to the
first `Reference` that declares its label.

Each inline token's fields finish before the enclosing cursor advances. Fields
use the same parser and report ordinary raw whitespace as a boundary entry on
the enclosing delimiter stack, excluding entities and opaque tokens. The
structural walk skips emitted inline trees, so every source buffer is parsed
once. Field-local delimiters remain independent of those in the enclosing
content. The [delimiter model](inline-delimiters.md) owns pairing and scope
reduction. Backtick caches allocate lazily, bounded by the input length and
the inherited backtick limit.

## Explicit anchors

Completion declares effective explicit anchors in the roster at each node's
ENTER, once the node is complete, while it discovers owned label/title fields.
A node's attributes are settled by then: a block's as it closes, an inline's
as its root's content is parsed, and a fence that names a formula becomes a
FormulaBlock, which holds none, as its element completes it at that ENTER. A
finish step rewrites a node's kind, literal or record, never its attributes.
Its walk parses each container's inline content at that container's ENTER, so
it visits only completed child trees, after bracket reductions and occurrence
attributes have settled; a temporary inline later discarded by a footnote
call declares nothing. Every node's explicit anchor is its own, a
`Reference`'s included, and two nodes may declare the same one.

The same walk resolves contextual script-space escape tokens after
bracket/delimiter ownership is final, before consolidation merges the token's
Text into its neighbours, so all later consumers read decoded literals. Script
depth follows child and owned-field edges; a block, such as an inline note's
Footnote, begins its own context. Failed enclosing candidates therefore leave
field escapes literal, while a completed script also decodes escapes in
nested labels.

## Lifetime and bounds

A heading owns its attributes, anchor included, and a reference occurrence
owns its label, so freeing a heading or a document invalidates no other node.
The roster holds its own copy of every label it records. Every failure joins
the parser's terminal allocation-failure transaction.

Work is proportional to parsed input and visited nodes. Canonical fixtures and
Swift, Kotlin, and ES tests verify that the same completed facts cross every
binding boundary.
