# Canonical AST contract

Status: frozen for Phase 5 on 2026-07-11; footnote contract revised for v2
milestone M3 on 2026-07-16 (source-order definitions, label-carrying
references, query-based numbering — see the reference and footnote semantics
section and
`sessions-and-deltas.md`); identity/equality contract added and scope moved
off node values for v2 milestone M4 on 2026-07-17 (`MarkupSession` becomes a
canonical entry point; the footnote label field is renamed `label` because
`id` now names node identity); directive labels promoted to the public
`DirectiveLabel` kind on 2026-07-29; the
reference model unified with the footnote model on 2026-08-02, adding
`ReferenceDefinition`, `LinkReference`, and `ImageReference` for the present
34 kinds and,
in the same revision, making an undefined footnote reference literal text so
all three reference forms answer "no definition" the same way; adopted the
unified-CST ownership model on 2026-08-03, replacing the single `revision`
scalar with `MarkupTrack`, moving parser answers from session scope to the
immutable published document, and pinning which string fields carry a
`TextMap` (`incremental-canonical-ast.md`) — a map since removed, on
2026-08-07, because no consumer ever asked to see the bytes behind decoded
text and the reverse lookup they do ask for is `document.scope(of:)`.

Phase 18 adds the executable repository-level conformance data at
`specs/canonical-ast/manifest.json`. That manifest and its reviewed
Markdown/`.ast` pairs are the sole cross-platform oracle for this contract;
they do not change the production AST or define a serialization format.

This document is the language-neutral public AST contract implemented by the
Swift, Kotlin, and ES bindings. Platform APIs may use idiomatic syntax, but
they must not change names, nullability, ownership, traversal order, defaults,
or semantics.

## Core rules

- `Markup` is the only abstract AST node type, and it is the typed semantic
  projection of one unified CST rather than a separately allocated tree
  (`incremental-canonical-ast.md`, §0).
- Every `Markup` has a non-optional `track: MarkupTrack` carrying its
  `MarkupID`, its `MarkupRevision` pair, and its `SourceExtent`. Equality and
  hashing are `(MarkupID, revision.subtree)` for whole-subtree equality and
  `(MarkupID, revision.self)` for local equality — both O(1) and
  allocation-free — and equal nodes are guaranteed to have identical AST
  content. See the identity and equality section.
- Nodes do not store absolute source positions. Scopes are resolved on demand
  through the owning document (`document.scope(of:)`) in `O(log n)`, supplied
  with every `MarkupWalker` event, and printed by the dump; see
  `incremental-canonical-ast.md` §7.2 for the resolution rules and the
  coordinate profiles.
- AST values are immutable after construction and own their strings and
  collections. No value retains a C node, document, allocator, or WASM handle.
- Collections are ordered and read-only. Their order is source order unless a
  field below states otherwise.
- `TableRow`, `TableCell`, and `DirectiveLabel` are scoped `Markup` kinds
  reached through typed properties. Being owned by `header`, `rows`, `cells`,
  `label`, and `content` does not make them non-node structural records.
- A directive label is a real canonical child, not a hidden parse unit or a
  transparent wrapper. `Directive` and `DirectiveBlock` expose that same node
  through their typed optional `label: DirectiveLabel?` property.
- The AST contains parsing semantics only. Renderer state, security policy,
  layout, highlighting, generated HTML, and MS-private syntax are excluded.

## Coordinates

```text
Position(line: integer, column: integer)
Scope(start: Position, end: Position)
```

Scopes inherit the native C parser's source-position values and semantics
exactly. The C facade and platform bindings copy `line` and `column` without
rescanning, normalizing, expanding, rejecting, or otherwise reinterpreting
particular coordinate combinations. Consumers that need to interpret a source
position use the native parser contract from the same Markdown Core release.

`TableRow`, `TableCell`, and `DirectiveLabel` resolve scopes like every other
`Markup`, so typed ownership boundaries do not discard source information.
A `DirectiveLabel` scope covers the complete bracketed source span, from the
opening `[` through the closing `]`. This includes both brackets for an
explicit empty `[]`; its inline descendants retain their own scopes inside
that span.

## Shared value types

### PlacementMode

`PlacementMode` has exactly two values:

- `embedded`: content participates in surrounding inline flow.
- `standalone`: content is presented independently from surrounding inline
  flow.

Placement and AST containment are related but not interchangeable. In
particular, `Formula` may be `standalone` while remaining inside a
paragraph. The invariants for other nodes are:

| Type | Allowed mode |
| --- | --- |
| `Directive` | `embedded` |
| `DirectiveBlock` | `standalone` |
| `Code` | `embedded` |
| `CodeBlock` | `standalone` |
| `Formula` | `embedded` or `standalone` |
| `FormulaBlock` | `standalone` |

### Directive attributes JSON

Directive `attributes` is an optional `String` containing the normalized JSON
representation of a generic directive attribute list. Every member name and
value is a JSON string. Non-string values and nested objects or arrays are
invalid. `null` means no attributes container was present; `"{}"` means an
explicit empty map.

Markdown source uses `{key=value}` attribute-list syntax, not JSON syntax.
Bare attributes and unquoted, single-quoted, or double-quoted values are
supported. Values that look like booleans or numbers remain strings. JSON
serialization is deterministic and is the value passed to consumers for
decoding.

An `=` promises a value, and a bare attribute is not the same thing as one with
an empty value: `{a}` is valid and `{a=}` is not. Whitespace, including a line
ending, may separate the `=` from its value, but reaching the end of the block
instead invalidates it. An unquoted value may not contain `"`, `'`, `<`, `=`,
`>`, or a backtick; reaching one of those does not end the value, it invalidates
the block. A brace is an ordinary character inside an unquoted value, and the
value ends at whitespace or at the block's closer.

`#name` and `.name` are shorthand for the `id` and `class` attributes, as in
micromark-extension-directive. A marker with no name after it is not a valid
attribute and invalidates the block.

An inline directive's colon may not sit next to another colon on either side:
the trailing rule keeps `:red:` available to emoji, and the leading one keeps a
run of colons whole, so `x ::a` is text rather than `x :` plus a directive.
`::name` and `:::name` at the start of a line are leaf and container directives
and open through the block path.

An unterminated label leaves the directive standing on its name with the
bracket as literal text, the same fallback a malformed attribute block takes.

Every repeated key uses its last value while retaining its first source
position, with one exception: `class` accumulates instead, joining its values
with a single space in source order, whether they were written as shorthand or
as `class=`. The separator is written whenever something has already
accumulated, so `{.a class="" .b}` is `class="a  b"` while `{class="" .b}` is
`class="b"`.

An attribute block that does not parse does not invalidate the directive: the
name still stands and the braces stay as literal text. `:invalid{=value}` is a
directive named `invalid` followed by the text `{=value}`.

These rules are checked against the reference implementation by
`scripts/check-mdast-parity.mjs`.

Attribute names have no HTML semantics and are never projected to HTML
attributes. For example:

```markdown
:video[My video]{id=123 muted=true title="My Video"}
```

is exposed as `{"id":"123","muted":"true","title":"My Video"}`.

### Other enums

```text
ReferenceForm = full | collapsed | shortcut
ListFlavor = bullet | ordered
TableAlignment = none | left | center | right
```

## Node inventory

`content` and other collection fields below own their values. `inline content`
means only inline `Markup` kinds are valid; `block content` means only block
kinds are valid. Bindings treat a category violation from the C facade as an
error rather than silently dropping a value.

| Kind | Fields in canonical order | Nullability and invariants |
| --- | --- | --- |
| `Document` | `content: [Markup]` | block content |
| `BlockQuote` | `content: [Markup]` | block content |
| `Paragraph` | `content: [Markup]` | inline content |
| `Heading` | `level: Int`, `content: [Markup]` | `level` is 1 through 6; inline content |
| `ThematicBreak` | none | leaf |
| `List` | `flavor: ListFlavor`, `start: Int?`, `tight: Bool`, `items: [ListItem]` | `start` is non-null only for ordered lists |
| `ListItem` | `checked: Bool?`, `content: [Markup]` | `checked == null` means not a task item; block content |
| `CodeBlock` | `mode`, `info: String?`, `language: String?`, `literal: String`, `fenced: Bool`, `closed: Bool` | mode is `standalone`; `info` is the complete raw info string; `language` is its first non-whitespace token; indented blocks have `fenced=false, closed=true` |
| `HTMLBlock` | `comment: Bool`, `literal: String` | raw HTML is preserved; `comment` is true when the literal is one complete comment — after surrounding whitespace it opens with `<!--` and its first `-->` is the terminal bytes — so consumers without an HTML parser can skip comment material; comment-prefixed HTML with a same-line tail is not a comment |
| `FormulaBlock` | `mode`, `literal: String` | mode is `standalone` |
| `Table` | `alignments: [TableAlignment]`, `header: TableRow`, `rows: [TableRow]` | one alignment per column; header is non-optional |
| `TableRow` | `isHeader: Bool`, `cells: [TableCell]` | `isHeader` is true only for `Table.header` and false for entries in `Table.rows` |
| `TableCell` | `content: [Markup]` | inline content |
| `DirectiveBlock` | `mode`, `name: String`, `attributes: String?`, `label: DirectiveLabel?`, `content: [Markup]` | attributes is normalized string-map JSON object text; mode is `standalone`; label is the optional first canonical child; content is block; null label and explicit empty label remain distinct |
| `DirectiveLabel` | `content: [Markup]` | complete inline child list; scope covers the full `[...]`; an explicit `[]` is a present node with empty content |
| `FootnoteDefinition` | `label: String`, `content: [Markup]` | label is written between `[^` and `]`; non-empty; block content; stays at its source position whether referenced or not |
| `ReferenceDefinition` | `label: String`, `destination: String?`, `title: String?` | a link reference definition at the position it was written; label is written between `[` and `]`; leaf; stays whether referenced or not |
| `Text` | `literal: String` | leaf |
| `SoftBreak` | none | leaf |
| `LineBreak` | none | leaf |
| `Code` | `mode`, `literal: String` | mode is `embedded`; leaf |
| `HTML` | `comment: Bool`, `literal: String` | raw HTML is preserved; leaf; `comment` follows the same one-complete-comment rule as `HTMLBlock` |
| `Formula` | `mode`, `literal: String` | either mode; leaf |
| `Emphasis` | `content: [Markup]` | inline content |
| `Strong` | `content: [Markup]` | inline content |
| `Strikethrough` | `content: [Markup]` | inline content |
| `Link` | `destination: String?`, `title: String?`, `content: [Markup]` | the inline form `[a](/u)`, whose destination is written in the source; absent and empty title remain distinct; inline content |
| `Image` | `source: String?`, `title: String?`, `content: [Markup]` | the inline form `![a](/u)`; content is parsed alt-text inline content |
| `LinkReference` | `label: String`, `form: ReferenceForm`, `content: [Markup]` | `[text][label]`, `[label][]`, `[label]`; carries no destination — which definition the label resolves to is a query; inline content |
| `ImageReference` | `label: String`, `form: ReferenceForm`, `content: [Markup]` | as `LinkReference`, for `![alt][label]` and its collapsed and shortcut forms |
| `Directive` | `mode`, `name: String`, `attributes: String?`, `label: DirectiveLabel?` | attributes is normalized string-map JSON object text; mode is `embedded`; label is the only possible canonical child; null label and explicit empty label remain distinct |
| `FootnoteReference` | `label: String` | label is written as in source; non-empty; leaf; exists only where the document defines the label — an undefined `[^x]` is `Text` |
| `CrossLink` | `reference: String` | source-faithful non-empty reference from `[[reference]]`; leaf; has no in-document definition, so it is never undefined |
| `Embed` | `reference: String` | source-faithful non-empty reference from `![[reference]]`; leaf; as `CrossLink` |

Every row above also carries the inherited `track: MarkupTrack`; it is not
repeated in the table. No row has a stored scope, and no row has a stored
absolute offset of any kind.

At most one field per kind is the kind's content text, spelled `literal`, and
it is a `Utf8Text`: decoded characters and nothing beside them. UTF-8 there is
the caller's obligation carried through, not a property the engine
manufactures — `incremental-canonical-ast.md` §7.1 assumes the input encoding
and never validates it, so bytes that were not UTF-8 going in are not UTF-8
coming out. Seven kinds
have one: `CodeBlock`, `HTMLBlock`, `FormulaBlock`, `Text`, `Code`, `HTML`,
and `Formula`. The other twenty-seven have none; their content is a child
sequence or nothing, and a kind with no textual value simply never carries the
`TEXT` part of `incremental-canonical-ast.md` §9.1.

**No text field carries a map back to the bytes that produced it**, and
neither does any other string field: `Link.destination` and `Link.title`,
`Image.source` and `Image.title`, `ReferenceDefinition.destination` and
`.title`, `CodeBlock.info` and `.language`, every `label`, `name`,
`attributes`, and `reference` are plain scalars holding decoded characters.
Naming the source span of any of them is a sub-node extent that does not exist
yet (`incremental-canonical-ast.md` §7.2); a consumer that needs one resolves
the owning node's extent and searches within it.

The seven text fields used to be the exception, paired with a span map back to
source. `incremental-canonical-ast.md` §6.1 removes it: no consumer asked to
see the bytes behind decoded text, the two kinds whose reverse lookup the
design was for — `CrossLink.reference` and `Embed.reference` — are
source-faithful and so map identically, and a local diagnostic names a source
span rather than a decoded character's provenance. What consumers do ask for
is the reverse lookup itself, and that is `document.scope(of:)`.

### Identity and equality

```text
MarkupTrack {
    MarkupID       identity
    MarkupRevision revision      // { self, subtree }
    SourceExtent   extent
}
```

`MarkupID` pairs the owning document's opaque `DocumentDomain` with a positive
ordinal: ordinals are unique within a domain, never reused after retirement,
and stable across incremental commits while the node remains the same logical
node. Nodes from different domains never compare equal, and passing an
identity from another domain is a programmer error that traps rather than a
result value. A one-shot parse gets its own domain, as does any change to the
schema, parse options, or source profile.

`revision` is a pair, never a single number. `revision.self` is the revision
at which the node's own local projection last changed — its kind, scalar and
text fields, direct child membership and order, and the parser answers
addressed to it. `revision.subtree` is that plus everything reachable below
it. A pure positional shift changes neither. Both are drawn from the one
positive, strictly monotonic `Revision` counter of the owning domain; zero is
invalid.

Equality and hashing on every kind are `(MarkupID, revision.subtree)`, which
is whole-subtree equality; `(MarkupID, revision.self)` compares the node's own
projection without its descendants. Identifiable-style APIs use `MarkupID`
alone. Two equal nodes are guaranteed to have identical AST content. Absolute
source position is not content.

Which identities survive an edit is decided by the anchored continuity rule of
`incremental-canonical-ast.md` §5.2: identity never crosses a parent or a
kind, nodes the edit does not overlap are matched positionally against stable
old witnesses, and only the children the edit overlaps are matched by content.
The full identity contract lives there.

### Relationship to upstream cmark-gfm

Markdown Core descends from cmark-gfm, and its Markdown semantics are
deliberately identical to upstream's except for the differences below. That is
a checked claim, not an aspiration: `scripts/check-upstream-parity.mjs` parses
the GFM specification corpus with both this parser and the pinned upstream
build, normalizes the two ASTs, and requires them to agree everywhere else.

The spec fixtures cannot make this claim on their own. Their expected blocks
are canonical dumps this parser generated, so they pin the behaviour without
proving it right; a divergence introduced before those dumps were frozen would
be preserved by them, not caught.

| Difference | What upstream does | What Markdown Core does |
| --- | --- | --- |
| Added syntax | no formula, directive, cross-link, or embed | parses all four; the parity comparison runs under `--profile gfm`, which leaves them off |
| Task-item checked state | substring search for `[x]` over the whole line, so `- [ ] call me [x] later` is reported checked | reads the marker the scanner matched, so that item is unchecked |
| Footnote definition placement | moved to the document tail in first-reference order | stays at its source position (below) |
| Rewound definition titles | a title candidate followed by non-whitespace is rewound out of the definition, but the scanned title stays in the reference map, so references resolve with it | the rewind drops the title everywhere — the definition has none, as the spec's own prose says, and micromark agrees |
| Split table lead's escapes | pipe-unescapes the paragraph lines split off above a recovered header row, then inline-parses them, so a lead's `\\\|` renders `\|` | keeps the lead's authored spelling and parses it like any paragraph — `\\\|` is an escaped backslash and a literal pipe, as micromark also reads it |

Link reference definitions are **not** a difference: both parsers consume them
into the reference map and neither leaves a node behind.

The registry the check reads is `specs/upstream-parity/deltas.json`, and it is
the place a new difference gets written down. A divergence is only ever a
defect or an entry there; it is never accepted on the grounds that this
implementation is obviously right.

### Relationship to the remark ecosystem

cmark-gfm cannot judge the constructs it does not implement. For those the
authority is the micromark extension that defines each one —
`micromark-extension-directive` for directives, `micromark-extension-math` for
math, `micromark-extension-gfm-*` for footnotes and tables — which is what
Markdown Core's own extensions were written against.

Those extensions emit a token stream rather than a tree, so
`scripts/check-mdast-parity.mjs` runs them through remark, which registers them
unchanged and turns their tokens into mdast. remark is the harness; the syntax
being compared is the extensions' own. The AST *shape* is remark's, which is
why every `ast-shape` entry in `specs/mdast-parity/deltas.json` comes from that
layer and not from the syntax definitions.

Neither project specifies which construct wins when two of them could claim the
same line: that follows from micromark's construct priority on one side and
this repository's extension registration order on the other. Divergences of
that kind are decisions rather than defects.

Two kinds of entry live in that registry. **Shape deltas** are places where the
two ASTs state the same thing differently and the normalizer bridges them: mdast
leaves reference links unresolved beside `definition` nodes where this
repository resolves them as cmark does, puts a directive's label in the
directive's own children where this repository wraps it in `DirectiveLabel`,
has no soft-break node, and carries an image's alternative text as a string
rather than as inline children.

**Registered divergences** are places where remark is not the authority:

| Construct | Authority | Difference |
| --- | --- | --- |
| `$...$` inline math | GitHub | GitHub's heuristics keep prose about money out of math; remark-math has none, and parses `Value $300B and ~$100B` as an expression |
| `` $`...`$ `` | GitHub | a documented GitHub form remark-math does not implement |
| `$$...$$` alone on a line, and ```` ```formula ```` | GitHub | block formulas here; remark leaves inline math and a code block |

The gate requires every registered divergence to still reproduce, so one that
upstream later settles surfaces as a failure rather than as an entry nobody has
re-read.

### Reference and footnote semantics (revised 2026-08-02)

Footnotes and link references are one model. This section states it once for
`FootnoteReference`, `LinkReference`, and `ImageReference` together.

**Definitions stay where they were written.** A footnote definition is an
ordinary block at its source position: never moved to the document tail, never
dropped when unreferenced, never reordered by use. A link reference definition
is a `ReferenceDefinition` block in the same way. Labels match case-folded with
collapsed whitespace, and the earliest definition of a label in document order
wins.

**A reference carries its label, not its destination.** A `LinkReference` or
`ImageReference` carries the label as written and the form it was written in;
a `FootnoteReference` carries the label written between `[^` and `]`. The
destination is stated once, at the definition. Which definition a label
resolves to is a query, not node content — like a footnote's number.

**A reference with no definition is not a reference.** It is the text the
author typed. `[bar]` with no definition is prose; `[^x]` with no definition is
the literal five characters `[^x]`, label included and *not* reparsed, so
`[^~~x~~]` is text and holds no `Strikethrough`. A bracket whose label has no
non-whitespace character never forms a reference either.

That third rule was the last axis to unify. Until 2026-08-02 an unresolved
`[^x]` kept its node, on the argument that its marker is unambiguous so the
node can be produced without a definition. What settled it was the cost on the
other side: a renderer would need a special case for the one reference form
that can be unresolved, and the rule it would special-case is not available to
the other two — a bare `[bar]` is prose by grammar, so a link reference exists
only where a definition does, and no choice about it was ever open. Both
authorities, cmark-gfm and remark, already degraded to text. Keeping the node
bought a distinction nothing downstream wanted.

The rule reaches only the reference forms a *document* defines. `CrossLink` and
`Embed` have no in-document definition mechanism at all: their targets name
things outside the file, and whether one resolves is a question for the
consumer that owns those targets, not for the parser. They are nodes whenever
their syntax matches, and "unresolved" is not a state this AST can observe.

Numbering, first-use order, resolution state, and back-reference ordinals are
not AST content. They are parser answers: queries over the relation indexes
the immutable published document pins, addressed by `MarkupID`
(`incremental-canonical-ast.md` §4.1 and §6.3), with the answer record types
defined in `sessions-and-deltas.md`. They are not a live-session feature, and
a retained document keeps answering them after later commits and session
close. Renderers that need the GFM presentation
(definitions gathered at the tail in first-use order, numbered markers)
derive it from those queries. This aligns the tree with the mdast model and
keeps edits from rewriting unrelated parts of the document.

Making definedness decide a node's type puts a document-scoped fact inside an
inline parse, which an incremental parser has to answer without reading the
whole document each time. It is answered by persistent definition tables — one
for reference definitions, one for footnote definitions — that a commit
retracts and re-registers over the lines it reparses, before it builds the CST
for them; this is publication step 0 of `incremental-canonical-ast.md` §6.3,
and it is why the CST's node inventory can depend on a document-wide relation
without the publication order becoming circular. A label whose definedness
flips re-refines exactly the units that read it, found through the mention
index rather than by scanning. The two kinds get separate tables rather than
one keyed by label: `[x]:` and `[^x]:` would share a bucket, one kind's flip
could hide behind the other's presence, and the units that read the hidden one
would keep a stale tree.

### Typed table ownership

```text
Table(alignments, header: TableRow, rows: readonly TableRow[])
TableRow(isHeader, cells: readonly TableCell[])
TableCell(content: readonly Markup[])
```

These are all immutable `Markup` values. The typed edges preserve legal table
shape without a generic public `children` property. `isHeader` mirrors and
validates the owning edge: the value in `Table.header` is true and values in
`Table.rows` are false.

### Typed directive-label ownership

```text
Directive(label: DirectiveLabel?)
DirectiveBlock(label: DirectiveLabel?, content: readonly Markup[])
DirectiveLabel(content: readonly Markup[])
```

`DirectiveLabel` is a public `Markup` kind placed immediately after
`DirectiveBlock` in the one canonical inventory. The C tree and every platform
AST expose one topology: when present, the label node is the directive's first
direct child and owns its complete inline child list. A block directive's block
`content` follows it; an inline directive has no other children. There is at
most one label node.

Absence and explicit emptiness are structural rather than scalar metadata:
missing brackets mean `label == null` and no `DirectiveLabel` child, while
`[]` means a present `DirectiveLabel` whose `content` is empty. Bindings
project the real child edge into the typed optional property; they do not
slice a label prefix, hide a storage node, or compensate with a transparent
edge rule.

## ParseOptions

`Document.parse(source, options = ParseOptions.default)` and
`MarkupSession(options)` (`sessions-and-deltas.md`) are the two canonical
parsing entry points. `ParseOptions` is immutable and contains exactly these
booleans:

| Field | Default |
| --- | --- |
| `smartPunctuation` | `true` |
| `footnotes` | `true` |
| `tables` | `true` |
| `strikethrough` | `true` |
| `autolinks` | `true` |
| `taskLists` | `true` |
| `formulas` | `true` |
| `directives` | `true` |
| `crossLinks` | `true` |
| `embeds` | `true` |

Disabling an extension disables recognition of its syntax and produces the
same fallback core AST on every platform. `formulas` controls every supported
formula form together: dollar delimiters, LaTeX delimiters, and `formula`
fenced code. Scope tracking is mandatory and is not an option.
Renderer-only `unsafe`, `github-pre-lang`, and `full-info-string` options do
not exist. Raw HTML, URLs, and full code info strings are always retained.

## MarkupVisitor and MarkupWalker

The typed `MarkupVisitor<Result>` has one dispatch method for every `Markup`
kind in the 34-kind node inventory, including `TableRow`, `TableCell`, and
`DirectiveLabel`. The interface is exhaustive: every typed method is required,
there is no `defaultVisit`, optional handler, catch-all adapter, or
protocol-extension fallback. Adding a `Markup` kind must therefore produce
compile errors in every visitor until the new case is handled. Visiting one
node does not implicitly recurse.

The standard read-only `MarkupWalker` walks a `Document` (whole or from
a subtree root) depth-first and emits `entering` then `exiting` events for
every reachable `Markup`, each carrying the node's resolved absolute scope. Applying an
exhaustive MarkupVisitor on `entering` invokes it exactly once per node. MarkupWalker owns
the typed-property rules, so consumers never inspect kinds to discover
structure:

- ordinary containers traverse `content` in index order;
- `List` traverses `items` in order;
- `Table` traverses `header`, then `rows`; each row traverses cells and each
  cell traverses inline content;
- directives traverse the `DirectiveLabel` node first when present; that node
  traverses its inline `content`, and block `content` follows it;
- `Link` and `Image` traverse their inline `content`.

Rows, cells, and directive labels produce normal visitor callbacks before
their descendants.
MarkupVisitor and MarkupWalker expose no replace, remove, setter, parent mutation, or
native-handle callback.

## Diagnostic dump

Swift, Kotlin, and TypeScript publish `MarkupDumper.dump(document)` with a
convenience `document.dump()`, plus a subtree form
`MarkupDumper.dump(document, of: node)` / `document.dump(of: node)`. All
traverse that platform's immutable typed tree through its exhaustive MarkupVisitor
and read-only MarkupWalker; they do not call the C diagnostic dump. Dumping is
document-mediated because scopes are (subtree dumps print scopes with the
subtree as origin — see `canonical-ast-dump.md`). The canonical text grammar
is defined in `canonical-ast-dump.md` and is diagnostic rather than a
serialization API; its frozen `id=` key on footnote nodes prints the
`label` field.

## Kotlin `List` naming contract

The concrete AST type remains `com.nouprax.markdown.core.List`. Kotlin source
inside the library spells collection types as `kotlin.collections.List<T>`.
Consumers resolve ambiguity with either the fully qualified AST name or an
import alias such as:

```kotlin
import com.nouprax.markdown.core.List as MarkdownList
```
