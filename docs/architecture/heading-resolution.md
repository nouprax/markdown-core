# Heading resolution

The [anchors module](../specs/dialect/anchors.md) owns the language contract.
Parsing establishes heading labels before reference lookup, then fills their
shared targets after every explicit anchor is known. Consumers and extension
postprocessors receive only the completed document.

## Declaration order and inline ownership

Block finalization registers headings in source order, including headings in
footnote definitions. A heading is a leaf block, so closure order is source
order. The parser keeps borrowed node pointers in that order; no final tree
search or sorting is needed. Explicit reference definitions are already in the
ordinary reference map before heading declarations are added. The map's
existing first-definition rule therefore gives explicit definitions priority
and selects the first duplicate heading.

A heading label uses authored source, not projected display text. Its endpoint
depends on whether the normal inline cursor actually claims trailing heading
attributes. Merely finding a syntactically valid tail is insufficient: a code
span, formula, HTML token, or comment may own its opening brace. Conversely,
brackets inside an attached attribute value are not part of the heading label.

Each heading starts the ordinary inline parser. It can finish and declare its
label without reference lookup until it encounters a live unescaped bracket.
That bracket makes the authored label unwritable under the inherited reference
label grammar. The parser retains that same inline subject, including its
cursor and delimiter state, and resumes it after all writable heading labels
have been declared. There is no second inline recognizer, reparsed prefix,
placeholder AST, or later replacement of literal Text with Links.

Opaque tokens and attached attribute containers are consumed by their existing
owners before this boundary is tested. On completion the same raw-label
validator used by explicit references rejects brackets even when they occurred
inside an opaque token. For example, `# Title {k="[Later]"}` can declare
`Title`, while `# Title [Later]` cannot declare a heading label and may itself
contain a forward reference. A valid declaration cannot depend on a reference
lookup. This is the invariant that permits one declaration pass and one
continuation pass, without a fixed-point resolver or general task scheduler.

Ordinary blocks and owned inline fields then parse against the completed map.
The heading's ordinary inline subject is the sole owner of its temporary
caches and delimiter/bracket stacks. Backtick caches allocate lazily, bounded
by the input length and the inherited backtick limit; a pending heading does
not retain a maximum-sized cache for source that never needs one.

## Final anchors and shared resources

Each writable heading declaration creates an ordinary reference resource with
an initially empty URL, no title, and empty attributes. Occurrences retain that
resource through the existing reference algorithm. No occurrence copies or
interprets its provisional URL during parsing. Finalization writes `#anchor`
to the resource once; bindings encode and decode that resource once, just as
they do for an explicit reference definition. The heading's attributes do not
become inherited reference attributes.

After inline parsing and footnote finalization, the existing owned-tree phase
walk reserves effective explicit anchors from content, footnotes, and owned
label/title fields. The registry and C facade use one effective-anchor accessor
for local-over-inherited precedence. A reference resource's inherited anchor
is hashed only on its first emitted inheriting occurrence. This identity index
is necessary to avoid repeatedly hashing a long definition anchor for every
short reference; unreferenced or fully overridden definitions reserve nothing.

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
Each occupied base has one increasing suffix cursor. Once `base-N` is known to
be occupied, later duplicates of that base never retry it. A spelling of the
form `base-N` has one such base, so occupied candidate work is amortized over
reserved spellings. There is no cardinality-dependent algorithm or restart at
suffix 1 for each heading.

## Lifetime and bounds

The heading collection and anchor indices borrow nodes and strings only until
finalization completes. Pending subjects own their temporary parser state.
The reference map and occurrences own resources through existing reference
counts; freeing the heading or document does not invalidate a detached Link.
Every failure joins the parser's terminal allocation-failure transaction and
disposes pending subjects before their nodes. Parse-time indices are discarded
before consolidation or extension postprocessing can replace nodes.

Expected work is proportional to parsed input, visited nodes, and produced
anchor/target bytes, using the shared hash index's normal bounds. Memory is
proportional to headings, unique reserved anchors and inherited resources, plus
live inline state. Tests measure node/string/candidate work, rather than using
timing to assert linearity. They include dense suffix reservations, nested live
delimiters at suspension, long shared anchors with thousands of references,
source-order headings inside footnotes, detached resource lifetime, and strict
allocation-failure sweeps. Canonical fixtures and Swift, Kotlin, and ES tests
verify that the same completed facts cross every binding boundary.
