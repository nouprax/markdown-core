# Canonical AST contract

**`docs/specs/canonical-ast.json` is the contract.** This document is its prose
companion: it carries everything a table cannot say — the core rules, the
coordinate model, ownership, the attribute grammar — and its own kind/field
table below is a second copy of the JSON, **checked against it by
`scripts/audit-ast-projections.mjs` kind for kind, field for field, in order**,
so the two cannot drift. Edit the JSON; the audit will tell you if this table
disagrees.

The executable repository-level conformance data lives at
`specs/canonical-ast/manifest.json`. That manifest and its reviewed
Markdown/`.ast` pairs are the sole cross-platform oracle for this contract;
they do not change the production AST or define a serialization format.

The language the parser accepts is defined by [`dialect.md`](dialect.md) and
its modules; this document is the contract of the AST the implementation
produces today. Where a dialect module describes a kind, field, or value that
this document lacks, the module names the landing item that adds it, and this
document stands until that item merges.

This document is the language-neutral public AST contract implemented by the
Swift, Kotlin, and ES bindings. Platform APIs may use idiomatic syntax, but
they must not change names, nullability, ownership, traversal order, defaults,
or semantics.

## Core rules

- `Markup` is the only abstract AST node type.
- Every `Markup` has the ordered inherited fields `scope: Scope`,
  `anchor: String?`, and non-null `attributes: Attributes`.
- AST values are immutable after construction and own their strings and
  collections. No value retains a C node, document, allocator, or WASM handle.
- Collections are ordered and read-only. Their order is source order unless a
  field below states otherwise.
- `TableRow` and `TableCell` are scoped `Markup` kinds reached through typed
  table properties. Being owned by `head`, `content`, `foot`, and `cells`
  does not make them non-node structural records.
- `DirectiveLabel` is `Markup` owned by a directive's typed `label` field. It
  is not an element of the directive's `content` and is not exposed through a
  generic child/content sequence.
- The AST contains parsing semantics only. Renderer state, security policy,
  layout, highlighting, and generated HTML are excluded.
- Adjacent `Text` nodes in one content array are merged into one node spanning
  from the first's start to the last's end, and a `Text` node is never empty.
- Besides `Markup`, exactly the scoped values `Citation`, `Footnote`, `Specimen`,
  and `Metadata` carry a `scope`, because they are written;
  every other value is located by its owner's scope. Those values arrive with
  the landing items that add them.

The remark oracle records `table-row-width-shape` for table representation:
mdast retains ragged rows until HTML conversion, whereas this AST completes
short rows and truncates long rows to the delimiter's column count. The
comparison normalizes only mdast using each table's own width.

## Coordinates

```text
Position(line: integer, column: integer)
Scope(start: Position, end: Position)
```

The C facade passes the supplied bytes to the native parser as UTF-8. Valid
UTF-8 is a caller precondition; Markdown Core has no validation or repair mode
for malformed input. Swift, Kotlin, and ECMAScript strings are encoded as UTF-8
before entering that same parse path.

`line` is 1-based and increments once per line ending, whether LF, CR, or CRLF.
`column` is the 1-based byte index within the line; a tab is one byte. `start`
is the first byte of the node's first code point and `end` the last byte of its
last code point, inclusive. Column 0 is the one sentinel: an end position `L:0`
names the boundary before the first byte of line `L`, that is, the position
just after the line ending of line `L - 1`. It is the end of a block whose
extent closes with a line ending it consumed, such as a list item, a footnote
definition, an indented code block, or a Setext heading that is followed by a
blank line, and an empty document has the scope `1:1..1:0`. A node's scope
never includes the line ending that terminates its last line, with one
exception: `SoftBreak` and `LineBreak` are the nodes of a line ending, so their
scopes cover those line-ending bytes. `SoftBreak` covers the line-ending bytes
of its break. `LineBreak` covers the line-ending bytes together with the
backslash that produced it; when trailing spaces produced it, the spaces stay
inside the preceding `Text` node's scope and `LineBreak` covers the line ending
alone. A multiline or grid table cell under the dialect's table options is the
one construct whose scope may include bytes of sibling cells, because its
segments are written on shared lines. A grid table cell whose `rowspan`
exceeds one is the one construct whose scope leaves its parent's: a
`TableRow` covers its own lines, the spanning cell reaches into the lines of
later rows, and the scope-containment gate ledgers that exception.

Scopes inherit the native C parser's source-position values and semantics
exactly. The C facade and platform bindings copy `line` and `column` without
rescanning, normalizing, expanding, rejecting, or otherwise reinterpreting
particular coordinate combinations. Consumers that need to interpret a source
position use the native parser contract from the same Markdown Core release.

A `Markup.scope` is the source-faithful, contiguous editor cursor range of that
node's own lexical occurrence. It never becomes an expanded or composite range
of every source location that contributed semantic values to the node.
Reference resolution, metadata inheritance, normalization, synthesis, and
other finalization operations may populate fields on an occurrence, but they
must not copy, union, substitute, or otherwise change its scope. In particular,
a resolved reference occurrence does not acquire the separate definition's
range, and a generated value has no fictional source position.

`TableRow` and `TableCell` have non-optional scopes like every other `Markup`,
so typed table boundaries do not discard source information.

## Shared value types

### PlacementMode

`PlacementMode` has exactly two values:

- `embedded`: content participates in surrounding inline flow.
- `standalone`: content is presented independently from surrounding inline
  flow.

Placement and AST containment are related but not interchangeable. In
particular, `Formula` may be `standalone` while remaining inside a paragraph.

**`Formula` is the only kind that carries a `mode`**, because it is the only
one whose value is a fact about the source rather than about the kind. For the
other five kinds the placement is constant and therefore implied by the kind:

| Type | Its one value, now implied by the kind |
| --- | --- |
| `Directive` | `name: String`, `label: DirectiveLabel?` | letter-first name; attributes use the inherited fields; label is a typed Markup field spanning its brackets, never content; absent and empty labels remain distinct; leaf |
| `DirectiveBlock` | `name: String`, `label: DirectiveLabel?`, `content: [Markup]` | letter-first name; attributes use the inherited fields; label is a typed Markup field spanning its brackets, never content; absent and empty labels remain distinct; block content |
| `Code` | `literal: String` | mode is `embedded`; leaf |
| `CodeBlock` | `info: String?`, `language: String?`, `literal: String`, `fenced: Bool`, `closed: Bool` | mode is `standalone`; `info` is the complete raw info string; `language` is its first non-whitespace token; indented blocks have `fenced=false, closed=true` |
| `FormulaBlock` | `literal: String` | mode is `standalone` |

### Universal attributes and metadata

Every Markup carries the ordered inherited fields `scope: Scope`,
`anchor: String?`, and `attributes: Attributes`. The
[attributes module](dialect/attributes.md) owns the single grammar,
normalization, and attachment operation. `Attributes(classes: [String],
records: [Record])` is never null. `Record(name: String, value: String)`
retains every assignment occurrence; classes retain every word occurrence.
The last identifier wins and an empty final `id=` clears the anchor.

Inline code, ATX/Setext headings, fenced code, and completed link/image
occurrences attach the same normalized attribute grammar. Reference definitions
supply inherited attributes; local anchors take precedence and local classes and
records follow inherited declarations without deduplication. These attachment
sites intentionally differ from Remark; exact inputs are registered in its
oracle policy. Each binding keeps its native collection types and owns all
returned values after the native document is released.

Parsed headings always have a nonempty anchor: an explicit identifier wins,
otherwise the [anchors module](dialect/anchors.md) derives one from parsed
content after reserving every emitted explicit anchor. Generated anchors add
no source range. Writable authored heading labels also define ordinary
reference targets, including forward references. These use `Destination.url`
with the final `#anchor`, no title, and no inherited heading attributes; all
occurrences share the existing reference resource. Explicit definitions win.
Differential fuzzing against cmark, cmark-gfm and remark keeps this extension
outside their shared-language domain; the independent scope classifier and
separate comparison counts are documented in the
[oracle policy](../../specs/oracles/README.md). The pinned Pandoc oracle checks
the implicit-reference behavior itself.
Remark's `heading-anchor-unavailable` comparison boundary omits only
`Heading.anchor`: mdast has no corresponding identifier fact. Heading levels,
content and attributes, and anchors on every other kind remain observable.

`Document.metadata: Metadata?` holds ten named optional values defined by the
[Properties value model](dialect/properties.md#model). Metadata is never
Markup and has no visitor callbacks. O6 produces it from the leading envelope.
It retains only the envelope scope; absent fields differ from explicit null values.

### Dimensions

`Dimensions(width: Int, height: Int?)` is a node-independent value. Width is
required and height is optional; every present component is in 1..2147483647.
It has no kind, scope, anchor, attributes, children or visitor callbacks.
`Media.dimensions` and `CrossEmbedded.dimensions` have type `Dimensions?`, absent when no complete valid suffix was
recognized, including malformed labels. O9 produces this value from image
labels and embedded cross-link labels. `CrossLink` has no dimensions field.
The value is independent of a destination's shared identity and attribute records.

### Destination

```text
Destination = url(String) | cross(path: String, anchor: String?)
```

`Destination` is a tagged value, not a node: it has no scope, children,
anchor, or attributes, and a branch's fields exist only in that branch. It is
the `dest` of every `Link` and `Media`, which own the `url` branch: the
complete semantic destination the inherited grammar produced, the bytes
between angle brackets or the bare destination with backslash escapes and
character references decoded and no percent-encoding, normalization, or
resolution, and possibly empty. The `cross` branch is the workspace address of
the [cross links](dialect/cross-links.md) module and is first produced by
`CrossLink` and `CrossEmbedded` (`O1`, `O9`). The parser fetches no URL, opens no file, tests no
existence, and infers no media type; no such result is a field or a branch.
The C facade answers it through `markdown_core_node_destination`, whose
`kind` names the branch and whose other branch's fields are zeroed; Swift
models it as an enum with associated values, Kotlin as a sealed interface with
one class per branch, and ECMAScript as a discriminated union on `kind`.

### Other enums

```text
BibMode = normal | authorInText | suppressAuthor
ListFlavor = bullet | ordered
OrderedListVariant = decimal | alpha(lowercased: Bool) | roman(lowercased: Bool) | default
OrderedListDelimiter = period | parenthesis(closed: Bool) | default
TableAlignment = none | left | center | right
```

### CitationReferent, Citation, Footnote, and Specimen

```text
CitationReferent = bib(key: String, mode: BibMode) | footnote(id: String) | specimen(id: String)

Citation(referent: CitationReferent, prefix: [Markup], suffix: [Markup], scope)

Footnote(id: String, content: [Markup], scope)
Specimen(id: String?, start: Int?, content: [Markup], scope)
```

`CitationReferent` is a tagged value like `Destination`: no scope, and a
branch's fields exist only in that branch. The `bib` branch is first produced
by the [citations](dialect/citations.md) module with `P7`; every referenced
`[^label]` call and inline `^[content]` note produce the `footnote` branch,
whose `id` names the `Footnote` in `Document.footnotes` with the equal id.

`Citation` and `Footnote` are scoped values, not `Markup` kinds, as the
[footnotes](dialect/footnotes.md) module defines them: they are written, so
each carries a `scope`, and each owns Markup, but neither is ever a child of
a node. A `Citation` is reached only through `Cite.citations`, which holds at
least one item in source order; its `prefix` and `suffix` are non-null inline
content, empty when absent. A `Footnote` is reached only through
`Document.footnotes`, which holds every referenced definition and inline note
ordered by scope start, wherever it was written. A referenced definition keeps
its normalized label without the caret as `id` and its parsed block content.
An inline note keeps its parsed inline body directly, without a `Paragraph`.
Its id is `inline-N` for the N-th inline opener in source order (outer before
nested), with the smallest free `-K` suffix when necessary. All authored ids
are reserved before ids are assigned during document finalization. Nested
citations are id edges, including semantic cycles, never object references.
A later definition of an id already defined is a `Footnote` after the first, which every call resolves to, so a consumer
keying footnotes by id takes the first. The C facade answers the values through the opaque handles
`markdown_core_citation` and `markdown_core_footnote` and their accessors,
never through `markdown_core_node`; Swift, Kotlin, and ECMAScript model them
as value types outside their `Markup` unions, and `CitationReferent` as
`Destination` is modeled: a Swift enum with associated values, a Kotlin sealed
interface, and an ECMAScript discriminated union on `kind`.

`Specimen` follows the same definition ownership as `Footnote`: it is reached
only through `Document.specimens`, after content and footnotes in walks and
dumps. Every definition remains present, including anonymous and duplicate
ones. Its nullable `id` retains the authored label; a `specimen(id)` referent
names the first equal non-null id. Its nullable `start` retains an effective
explicit counter reset. A consumer derives displayed numbers in definition
order; neither definitions nor references store that derived state. The C
facade exposes `markdown_core_specimen` and its typed accessors. The model and
transports support these values now; [specimen syntax](dialect/specimens.md)
lands with `P9b`. Ordinary lists have no specimen variant or label field.

## Node inventory

`content` and other collection fields below own their values. `inline content`
means only inline `Markup` kinds are valid; `block content` means only block
kinds are valid. A category violation reported by the C facade fails
`Document.parse` on that binding with the platform contract-violation error
and returns no document.

| Kind | Fields in canonical order | Nullability and invariants |
| --- | --- | --- |
| `Document` | `content: [Markup]`, `metadata: Metadata?`, `footnotes: [Footnote]`, `specimens: [Specimen]` | block content; document-owned footnotes and specimens retain their values in scope-start order; visit content, then footnotes, then specimens; neither definition sequence counts as children |
| `Callout` | `variant: String?`, `collapsed: Bool?`, `title: [Markup]?`, `content: [Markup]` | every `>` container; `variant` is the authored type as written or null when the container has no metadata line, and then `collapsed` and `title` are null; `collapsed` is null when no `+` or `-` fold marker was authored, false for `+` and true for `-`; `title` is a node-valued field of inline content visited before `content` and never counted among its children; a present title holds at least one node; block content |
| `Paragraph` | `content: [Markup]` | inline content |
| `Heading` | `level: Int`, `content: [Markup]` | `level` is 1 through 6; inline content |
| `ThematicBreak` | none | leaf |
| `List` | `flavor: ListFlavor`, `start: Int?`, `variant: OrderedListVariant?`, `delimiter: OrderedListDelimiter?`, `tight: Bool`, `items: [ListItem]` | `start` is non-null only for ordered lists |
| `ListItem` | `marker: String?`, `content: [Markup]` | `marker == null` means not a task item; block content |
| `CodeBlock` | `info: String?`, `language: String?`, `literal: String`, `fenced: Bool`, `closed: Bool` | mode is `standalone`; `info` is the complete raw info string; `language` is its first non-whitespace token; indented blocks have `fenced=false, closed=true` |
| `HTMLBlock` | `literal: String` | raw HTML is preserved; a block that opens with `<!--` and whose end line holds only whitespace after the first `-->` is a `Comment` |
| `FormulaBlock` | `literal: String` | mode is `standalone` |
| `Table` | `columns: [TableColumn]`, `head: [TableRow]`, `content: [TableRow]`, `foot: [TableRow]` | non-empty columns define the logical grid; rows are owned exactly once in head/content/foot order; pipe tables have one head row, no foot rows, null relative widths, and unit spans; no span crosses a group boundary |
| `TableRow` | `cells: [TableCell]` | cells whose upper-left coordinate starts in this row, in logical order; no row-local header state |
| `TableCell` | `rowspan: Int`, `colspan: Int`, `content: [Markup]` | positive spans; inline or block content is stored as parsed without paragraph normalization |
| `DirectiveBlock` | `name: String`, `label: DirectiveLabel?`, `content: [Markup]` | letter-first name; attributes use the inherited fields; label is a typed Markup field spanning its brackets, never content; absent and empty labels remain distinct; block content |
| `DirectiveLabel` | `content: [Markup]` | inline content; the scope spans the brackets, so an empty label is still a place |
| `Text` | `literal: String` | leaf |
| `SoftBreak` | none | leaf |
| `LineBreak` | none | leaf |
| `Code` | `literal: String` | mode is `embedded`; leaf |
| `HTML` | `literal: String` | raw HTML is preserved; an HTML comment token is a `Comment`; leaf |
| `CrossLink` | `dest: Destination`, `label: String?` | inline leaf; cross destination; complete raw label; no separator means null |
| `CrossEmbedded` | `dest: Destination`, `label: String?`, `dimensions: Dimensions?` | inline leaf; workspace transclusion; cross destination; label is the raw prefix after a valid size suffix; no separator means null |
| `Comment` | `literal: String` | an HTML comment or a `%%` comment, the one kind valid in both block and inline content, which the parent edge records; `literal` excludes the delimiters and keeps every byte between them; leaf |
| `Formula` | `mode: PlacementMode`, `literal: String` | either mode; leaf |
| `Emphasis` | `content: [Markup]` | inline content |
| `Strong` | `content: [Markup]` | inline content |
| `Strikethrough` | `content: [Markup]` | inline content |
| `Mark` | `content: [Markup]` | inline content |
| `Insertion` | `content: [Markup]` | inline content |
| `Span` | `content: [Markup]` | inline content; may be empty |
| `Superscript` | `content: [Markup]` | inline content; non-empty body |
| `Subscript` | `content: [Markup]` | inline content; non-empty body |
| `Link` | `dest: Destination`, `title: String?`, `content: [Markup]` | `dest` is the tagged `Destination` value and is never absent: `[a]()` and `[a](<>)` wrote one and wrote nothing in it, so it is `url("")`; a reference occurrence answers the destination its definition stated, and an unresolved reference is the inherited literal text; every `Link` owns the `url` branch; absent and empty title remain distinct; inline content |
| `Media` | `dest: Destination`, `title: String?`, `dimensions: Dimensions?`, `content: [Markup]` | `dest` is the tagged `Destination` value and is never absent, for the reason `Link.dest` is not; every `Media` owns the `url` branch; absent and empty title remain distinct; content is parsed alt-text inline content |
| `Directive` | `name: String`, `label: DirectiveLabel?` | letter-first name; attributes use the inherited fields; label is a typed Markup field spanning its brackets, never content; absent and empty labels remain distinct; leaf |
| `Cite` | `citations: [Citation]` | one or more items in source order; every item has exactly one referent and one cite never mixes referent families; an inherited `[^label]` call is one item with a `footnote` referent whose id is the normalized label without the caret and with empty affixes; its items are scoped values, never children, so it is a leaf |

Every row also has the ordered inherited fields `scope: Scope`,
`anchor: String?`, and `attributes: Attributes`; they are not repeated in the table. The `url` of a `Link` or `Media` destination, and
every `title`, are the CommonMark-unescaped values with angle-bracket
wrappers removed and no percent-encoding or normalization. A link reference
definition produces no node: the parser consumes it, and every successful
full, collapsed, shortcut, or autolink form is the `Link` or `Media` it names,
with the definition's destination and title and its own occurrence scope. An
unresolved reference is the inherited literal text with its brackets.

### Typed table ownership

```text
Table(columns: [TableColumn], head: [TableRow], content: [TableRow], foot: [TableRow], scope)
TableColumn(alignment: TableAlignment, relative: Double?)
TableRow(cells: [TableCell], scope)
TableCell(rowspan: Int, colspan: Int, content: [Markup], scope)
```

Tables, rows, and cells are immutable `Markup`; a column is an unscoped value.
The C child chain contains the rows in head/content/foot order, partitioned by
counts stored on the table. No row duplicates its owning group's identity.
Bindings expose the three named arrays directly, and walkers traverse them in
that order. Pipe tables produce one head row, body rows in `content`, empty
`foot`, `relative=null`, and unit spans. Missing cells keep their scope at the
row's end. Inherited cells keep their inline nodes directly; later block-cell
syntax stores the parsed block sequence in the same `content` field.

The [tables module](dialect/tables.md#logical-grid) defines placement, span
occupancy, and group boundaries. `columns` is non-empty; a present `relative`
is a positive finite authored width share. The caption field arrives with its
kind and syntax in P11a.

The dialect recognizes complete double-bracket cross links before inherited link
and image bracket handling, including the inner reference of triple brackets.
The exact affected CommonMark inputs are registered in `specs/oracles/cmark/`;
remark directive labels may likewise contain cross links.

## Parsing

`Document.parse(source)` is the only parsing entry point on every surface,
and it takes no options. The parser recognizes the one dialect of
[`dialect.md`](dialect.md), in which every feature is always on: there is no
`ParseOptions`, no profile, no preset, and no switch of any kind, on the C
facade, the installed CLI, or any binding. Quotation marks, hyphen runs, and
periods are stored as written; the parser has no smart punctuation. Nothing
strips anything: an HTML comment and a `%%` comment are `Comment` nodes, and a
consumer that does not want comments drops the nodes.

A parse returns exactly the `Document` this document describes. The document
does not retain source text, a normalized source copy, a line index, tokens,
trivia, or recovery records. Scope tracking is mandatory and is not an option.
Renderer-only `unsafe`, `github-pre-lang`, and `full-info-string` options do
not exist. Raw HTML, URLs, and code info strings are always retained.

The test tree keeps no layer selection either: every package fixture, oracle
gate, and position audit parses the one language through the same entry a
consumer uses, and the package fixtures' fence tags only classify examples for
the oracle corpora, as [`test-architecture.md`](test-architecture.md) states.
No binding, C facade, or installed executable exposes a switch, and the shared
canonical manifest names no option.

## Visitor and walking

The typed `Visitor<Result>` has one dispatch method for every `Markup` kind in
the node inventory, including `TableRow`, `TableCell`, and `DirectiveLabel`.
The interface is exhaustive: every typed method is required, there is
no `defaultVisit`, optional handler, catch-all adapter, or protocol-extension
fallback. Adding a `Markup` kind must therefore produce compile errors in every
visitor until the new case is handled. Visiting one node does not implicitly
recurse.

The bindings also expose a read-only, depth-first `walk` operation driven by an
exhaustive node-kind-dispatched walking visitor. Every typed callback receives
an `entering` phase before the node's owned markup relations and an `exiting`
phase after them. The walk is implemented with an explicit action stack, so
language call-stack depth does not grow with AST depth.

Walking does not expose an iterator or a generic child projection. Each
node-kind traversal branch selects its own typed, owned relations. Relations
are visited in canonical field order and arrays retain their stored order:
`Table.head`, `Table.content`, and `Table.foot` are visited in that order, while `DirectiveBlock.label` precedes
`DirectiveBlock.content`. A directive label therefore participates in a
complete AST walk as the named `label` field without becoming directive
content or contributing to a `children` collection.

The scoped values `Citation`, `Footnote`, and `Specimen` receive value callbacks and the
walk descends into their markup arrays in declared field order: a `Cite`
visits each `Citation`, whose `prefix` precedes its `suffix`, and `Document`
visits `content`, `footnotes`, then `specimens`, each definition descending into its
`content`. Metadata carries its envelope scope and ten fields but no Markup edges, so
it receives no visitor callbacks. Unscoped values are likewise not descended into.

The walking visitor is exhaustive under the same rule as `Visitor`: every
node-kind callback is required and there is no default, optional handler,
untyped callback, or catch-all adapter. The walk is observation only; it has no
prune, replace, remove, setter, parent mutation, or native-handle callback.

Operations that need relation-specific policy rather than the canonical full
walk continue to implement recursion in their own exhaustive per-node Visitor.

## Debug dump

Swift, Kotlin, and TypeScript publish `TreeDumper.dump(markup)` and a
convenience `Markup.dump()` method. Each TreeDumper uses exhaustive per-node
Visitor dispatch, like cmark's per-node render callback: that node's dump
function emits its fields and decides which content or field nodes to visit.
No binding calls the C debug dump. Dumping a non-Document Markup treats that
value as the root and emits only its operation-defined dump projection. The
canonical text grammar is defined in `canonical-ast-dump.md` and is for
debugging rather than serialization.

## Kotlin `List` naming contract

The Kotlin AST type remains `com.nouprax.markdown.core.List`. Kotlin source
inside the library spells collection types as `kotlin.collections.List<T>`.
Consumers resolve ambiguity with either the fully qualified AST name or an
import alias such as:

```kotlin
import com.nouprax.markdown.core.List as MarkdownList
```

The remark oracle's `list-tightness-shape` projection combines a list's
`spread` with every direct item's `spread`, as mdast-util-to-hast does. Both
sources of looseness must be false for `List.tight` to be true; nested lists
are evaluated independently. The native `tight` value is compared unchanged.

`task-marker-completion` compares absent, incomplete, and complete task states
against boolean-only mdast and cmark-gfm XML. Those oracles cannot attest to
`x` versus `X`; exact authored markers remain covered by canonical fixtures
and binding tests. An unchecked marker followed by literal `[x]` still
exposes the registered upstream task-state defect.

M7's directive migration is checked by the exact-input differences in
[`specs/oracles/remark/deltas.json`](../../specs/oracles/remark/deltas.json):
Unicode letter-first names, dotted shorthands, bare-member rejection, empty
assignments, adjacent members, quoted line-ending normalization, unquoted
punctuation and entity preservation, unmatched-quote fallback, class splitting,
and ordered duplicate retention. The comparison reads universal anchors and
attributes on every Markup kind; it does not deduplicate the native values.

The exact-input `task-prefix-before-block-content` remark delta records an
empty task item's lack of a paragraph for lazy continuation, following the
[task-list prefix rule](dialect/task-lists.md). The expanded M7 fuzz corpus
exposed this pre-existing difference; task parsing is unchanged.

O5's [task-prefix grammar](dialect/task-lists.md) preserves one authored
Unicode scalar and consumes the whole SP/TAB/VT/FF separator run when an item
opens, before deciding its first block. Line endings never serve as separators,
even before lazy paragraph content. Custom markers, opening-line ownership,
block decisions and opaque bodies have exact witnesses in the cmark-gfm and
remark registries. The Obsidian registry locks both semantic digests for its
UTF-16 marker limit, excluded `]`, whitespace handling, paragraph-first scan,
escape decoding and later-line recognition; those are deliberate differences,
while `custom-task-character` is closed by agreement.

O6 adds the `properties-envelope` CommonMark delta: a complete first `---`
envelope becomes metadata even when its payload is not YAML. The pinned
CommonMark examples `---\n---\n` and `---\nFoo\n---\nBar\n---\nBaz\n`
therefore produce metadata in place of the initial body blocks. Unsupported
members are ignored and never enter Markup.

O9 adds the exact-input `image-dimensions` CommonMark delta. Complete positive
32-bit `W`, `WxH`, `alt|W` and `alt|WxH` suffixes populate each Media's `dimensions`
value and leave only the parsed prefix as alt content. CommonMark retains the
suffix as alt text. The [links and images module](dialect/links-and-images.md),
package fixtures, and shared canonical `media-dimensions` case own this syntax,
its malformed fallbacks, source scopes and cross-context compositions.

### Bracketed spans and script delimiters

The [bracketed-span module](dialect/bracketed-spans.md) defines `Span` and its
attribute suffix. The [script module](dialect/superscript-and-subscript.md)
defines `Superscript` and `Subscript`: single tildes always belong to Subscript,
which deliberately differs from cmark-gfm's single-tilde strikethrough.
The exact historical inputs remain in `specs/oracles/cmark-gfm/deltas.json`.
Pandoc differences in empty bodies, escaped spaces, Unicode whitespace and
opaque tokens are pinned in `specs/oracles/pandoc/deltas.json`; product fixtures
retain every node's authored scope and the exact decoded content.
The directive-envelope fallback witness in `specs/oracles/remark/deltas.json`
records a balanced label becoming a Span when its enclosing directive fails;
remark leaves that pair literal because it has no bracketed-span rule.

### Bibliography citations, specimens and ordered markers

The [citations](dialect/citations.md), [specimens](dialect/specimens.md) and
[lists](dialect/lists.md) modules produce the existing typed citation,
definition and ordered-list values on every binding. A complete bibliography
group beats a shortcut, including a virtual heading reference; direct links,
resolving reference tails and Spans have their specified earlier precedence.
Bare keys resolve to document-wide specimen labels only when the occurrence
has no bibliography tail. Generated anchors project the stored affixes and
keys in source order.

The exact `bibliography-citations` CommonMark/GFM differences retain inputs
whose @key now becomes a Cite. The `nested-ordered-start` CommonMark difference
requires a new nested ordered list to start at one. Pandoc agreements cover
keys, modes, ordinary tails, link/Span precedence, heading shortcuts and list
variants. Its retained affix whitespace, malformed-group fallback, underscore
boundary, unresolved reference tails, conditional startnum behavior and example
number rendering remain exact differences in the Pandoc registry. No projection
turns specimen IDs into numbers or discards duplicate definitions.
