# Heading resolution

The [anchors module](../specs/dialect/anchors.md) owns the language contract.
Parsing gives each heading its anchor after every explicit anchor is known.
Consumers receive only the completed document.

## Declaration order and inline ownership

Closing a heading registers it, so headings register in source order,
including headings in footnote definitions. A heading is a leaf block, so
closure order is source order. The parser keeps borrowed node pointers in that
order; no final tree search or sorting is needed. A heading declares no
reference label: `[Title]` is a reference only when a `Reference` declares
`Title`, and the published document resolves a `reference` destination to the
first `Reference` that declares its label.

Before the document's blocks are walked, each settled heading parses its
inline content once, in source order, with the ordinary inline parser. Its
attached attribute tail is its own unless the inline cursor actually claims
the opening brace: a code span, formula, HTML token, or comment may own it.
Ordinary blocks and owned inline fields then parse against the completed
reference map. Each inline token's fields finish before the enclosing cursor
advances. Fields use the same parser and report ordinary raw whitespace as a
boundary entry on the enclosing delimiter stack, excluding entities and opaque
tokens. The structural walk skips emitted inline trees, so every source buffer
is parsed once. Field-local delimiters remain independent of those in the
enclosing content. The [delimiter model](inline-delimiters.md) owns pairing
and scope reduction. Backtick caches allocate lazily, bounded by the input
length and the inherited backtick limit.

## Final anchors

Each heading without an authored anchor takes its computed anchor, a copy it
owns. A reference occurrence holds only the label it names; the anchor it
leads to is the heading's own, and the heading's attributes stay the
heading's.

Completion declares effective explicit anchors in the roster at each
node's ENTER, once the node is complete, while it discovers owned label/title
fields. A node's attributes are settled by then: a block's as it closes, an
inline's as its root's content is parsed, and a fence that names a formula
becomes a FormulaBlock, which holds none, as its element completes it at that
ENTER. A finish step rewrites a node's kind, literal or record, never its
attributes. Its walk parses each container's
inline content at that container's ENTER, so it visits only completed child
trees, after bracket reductions and occurrence attributes have settled; a
temporary inline later discarded by a footnote call cannot reserve an anchor.
An inline footnote is appended to the document's own roots by the parse that
declares it -- a heading's, during the document's preparation, or a
container's, from inside the walk -- and the walk visits those roots before
the content tree and again after it, until none is new. Block footnotes are
still attached to the content tree during this walk; the document's
finalization, which follows it, moves them into their chains and then gives
the headings their anchors. No anchor-specific whole-tree traversal is needed.
Every node's explicit anchor is its own, a `Reference`'s included, so each is
reserved once, at its node.

The same walk resolves contextual script-space escape tokens after
bracket/delimiter ownership is final, before consolidation merges the token's
Text into its neighbours. Heading projection and all later consumers
therefore read decoded literals; it never reinterprets authored escape spellings.
Script depth follows child and owned-field edges; a block, such as an inline
note's Footnote, begins its own context. Failed enclosing candidates therefore leave field
escapes literal, while a completed script also decodes escapes in nested labels.
Span, Superscript and Subscript contribute their ordinary child content.

Heading synthesis follows the registered source order. The projection streams
parsed text through Unicode simple lowercase, whitespace replacement, and the
permitted-category filter. It traverses ordinary children without recursion and
keeps return edges only when entering an owned directive label. Opaque bodies
and footnote targets are never descended. One checked-in Unicode 17.0.0 table
compiles the complete scalar operation into disjoint ranges with a constant
offset. Missing ranges mean omission. Each scalar needs one lookup, including
case conversion and whitespace replacement. Its generator verifies both the
pinned UnicodeData SHA-256 and Node Unicode version. Builds need no download.

A spelling is taken when the roster holds it as an explicit anchor or a
heading before was given it. When some heading's anchor is to be computed,
finalization's own exact-string index holds both: it first takes every
explicit anchor, reading the roster's anchor records in log order under the
roster's copy of their bytes, and then each spelling a heading is given.
Each occupied base has one increasing suffix cursor stored directly in its
index slot. Every candidate, the base first, costs one entry lookup, which
returns an occupied or a vacant slot; a vacant one the heading is given is
committed in place with the final node-owned spelling, without hashing it
again. Only a new key can grow the index. Synthesis advances the base cursor
before querying its next candidate and never reads that borrowed slot again
after a vacant candidate returns. This keeps slot lifetimes valid through
rehashing without allocating a stable object per anchor. Once `base-N` is known
to be occupied, later duplicates of that base never retry it. A spelling of the
form `base-N` has one such base, so occupied candidate work is amortized over
reserved spellings. There is no cardinality-dependent algorithm or restart at
suffix 1 for each heading. Decimal suffixes are appended directly with bounded
stack storage and no general format-string processing.

Projection and target construction reuse a single scratch buffer and a single
projection stack; the final string is one exact-size copy the heading owns. Scratch capacity is retained across
headings rather than discarded when a value is attached.

## Lifetime and bounds

The heading collection and finalization's anchor index borrow nodes and
strings only until finalization completes; the roster holds its own copy of
every label it records. Each heading owns its computed anchor, and a reference
occurrence owns its label, so freeing a heading or a document invalidates no
other node.
Every failure joins the parser's terminal allocation-failure transaction. Parse-time indices are discarded
before consolidation or an element's finish step can replace nodes.

Expected work is proportional to parsed input, visited nodes, and produced
anchor bytes, using the shared hash index's normal bounds. Memory is
proportional to headings and unique reserved anchors. Tests measure node/string/candidate work, rather than using
timing to assert linearity. They include dense suffix reservations, long targets with thousands of references,
source-order headings inside footnotes, and strict allocation-failure sweeps. Canonical fixtures and Swift, Kotlin, and ES tests
verify that the same completed facts cross every binding boundary.
