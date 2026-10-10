# Heading resolution

The [anchors module](../specs/dialect/anchors.md) owns the language contract.
Parsing establishes heading labels before reference lookup, then gives each
heading its anchor after every explicit anchor is known. Consumers receive
only the completed document.

## Declaration order and inline ownership

Block finalization registers headings, including headings in footnote
definitions. A heading records its source start when it registers
(`markdown_core_source_entry`), because a numbered node holds only its extent,
and the collected headings are ordered by that start; no final tree search is
needed. Each heading whose text is a writable label keeps
that label, normalized, and declares it in the ordinary reference map, as each
`Reference` declares its own. Reference parsing asks the map only whether a
label is declared, so it needs no heading-specific case. The published
document resolves a `reference` destination: to the first `Reference` that
declares the label or, when none does, to the first heading.

Each writable heading declares its label, duplicates included. Every
declaration owns its label in the same allocation as its record.

A heading label uses authored source, not projected display text. Its endpoint
depends on whether the normal inline cursor actually claims trailing heading
attributes. Merely finding a syntactically valid tail is insufficient: a code
span, formula, HTML token, or comment may own its opening brace. Conversely,
brackets inside an attached attribute value are not part of the heading label.

Each heading starts the ordinary inline parser. It can finish and declare its
label without reference lookup until it encounters a live unescaped bracket.
That bracket makes the authored label unwritable under the inherited reference
label grammar. The parser retains that same inline state, including its
cursor and delimiter state, and resumes it after all writable heading labels
have been declared. There is no second inline recognizer, reparsed prefix,
placeholder AST, or later replacement of literal Text with Links.

A consumed directive with an owned label is also a suspension boundary: that
label has live brackets and may contain references of its own. The inline state
retains a field-completion event on its delimiter stack, including when the
token ends the heading.
A directive without a label does not suspend declaration.

Opaque tokens and attached attribute containers are consumed by their existing
owners before this boundary is tested. On completion the same raw-label
validator used by explicit references rejects brackets even when they occurred
inside an opaque token. For example, `# Title {k="[Later]"}` can declare
`Title`, while `# Title [Later]` cannot declare a heading label and may itself
contain a forward reference. A valid declaration cannot depend on a reference
lookup. This is the invariant that permits one declaration pass and one
continuation pass, without a fixed-point resolver or general task scheduler.

Ordinary blocks and owned inline fields then parse against the completed map.
Each inline token's fields finish before the enclosing cursor advances. Fields
use the same parser and report ordinary raw whitespace as a boundary entry
on the enclosing delimiter stack, excluding entities and opaque tokens. The structural walk
skips emitted inline trees, so every source buffer is parsed once. Field-local
delimiters remain independent of those in the enclosing content.
The [delimiter model](inline-delimiters.md) owns pairing and scope reduction.
The heading's ordinary inline state is the sole owner of its temporary
caches and delimiter/bracket stacks. Backtick caches allocate lazily, bounded
by the input length and the inherited backtick limit; a pending heading does
not retain a maximum-sized cache for source that never needs one.

## Final anchors

Each heading without an authored anchor takes its computed anchor, a copy it
owns. A reference occurrence holds only the label it names; the anchor it
leads to is the heading's own, and the heading's attributes stay the
heading's.

An explicit anchor is noted as its node is numbered. A heading's own anchor
is read from the collected headings, because its text, which may declare it,
is parsed after the heading is numbered. Every explicit anchor is reserved
before any heading is given a computed anchor, when the document finishes. No
anchor-specific whole-tree traversal is needed. Every node's explicit anchor
is its own, a `Reference`'s included, so each is noted once, at its node.

Each inline root's completion pass resolves contextual script-space escape
tokens after bracket/delimiter ownership is final, before consolidation merges
the token's Text into its neighbours. Heading projection and all later consumers
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

The exact-string anchor index reserves both explicit and synthesized spellings.
Each occupied base has one increasing suffix cursor stored directly in its
index slot. An entry lookup returns either an occupied or a vacant slot; the
latter is committed using the final node-owned spelling without hashing it
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

The heading collection and anchor indices borrow nodes and strings only until
finalization completes. Pending inline states own their temporary parser state.
Each heading owns its computed anchor and its label, and a reference
occurrence owns its label, so freeing a heading or a document invalidates no
other node.
Every failure joins the parser's terminal allocation-failure transaction and
disposes pending inline states before their nodes. Parse-time indices are discarded
before consolidation or an element's completion step can replace nodes.

Expected work is proportional to parsed input, visited nodes, and produced
anchor bytes, using the shared hash index's normal bounds. Memory is
proportional to headings and unique reserved anchors, plus live inline state. Tests measure node/string/candidate work, rather than using
timing to assert linearity. They include dense suffix reservations, nested live
delimiters at suspension, long targets with thousands of references,
source-order headings inside footnotes, and strict allocation-failure sweeps. Canonical fixtures and Swift, Kotlin, and ES tests
verify that the same completed facts cross every binding boundary.
