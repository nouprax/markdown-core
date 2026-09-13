# Canonical AST contract

[Documentation](README.md) · [Syntax guide](dialect.md) · [Debug dump](canonical-ast-dump.md)

The [machine-readable contract](canonical-ast.json) defines the ordered kind
and field inventory. This companion specifies ownership, coordinates, values,
and traversal. The node table below is checked against that inventory by
`scripts/audit-ast-projections.mjs`.

The contract is implemented by the C facade and the Swift, Kotlin, and
ECMAScript bindings. Platform APIs use idiomatic syntax while preserving names,
nullability, ownership, traversal order, defaults, and semantics. Native C views
borrow from their document; language bindings own their materialized values.

The [syntax guide](dialect.md) defines the accepted Markdown language. Shared
reviewed input/output pairs in [the conformance manifest](../../specs/canonical-ast/manifest.json)
check this AST across platforms. They are test evidence, not a serialization
format. See [syntax conformance](../architecture/syntax-conformance.md) for
maintaining the contract and comparison policies.

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
  every other value is located by its owner's scope.

## Coordinates

```text
Position(line: integer, column: integer)
Scope(start: Position, end: Position)
```

The C facade passes the supplied bytes to the native parser as UTF-8. Valid
UTF-8 is a caller precondition; Markdown Core has no validation or repair mode
for malformed input. Swift, Kotlin, and ECMAScript strings are encoded as UTF-8
before entering that same parse path.

A scope is the pair of editor source coordinates reported by the parser,
using cmark's UTF-8 coordinate convention. It is not a string range: neither
platform string indices nor the decoded `literal` determine these values.
For example, the source `é &amp; 🚀` has Text scope `1:1..1:13`, while its
literal is `é & 🚀`. Bindings do not convert columns to UTF-16 or graphemes,
add one to an end coordinate, or impose half-open interval semantics.

Lines normally begin at 1 and increment once for LF, CR, or CRLF. Columns
follow the native byte-oriented convention; a tab occupies one source byte.
The native sentinel values are preserved too: a zero-byte document has scope
`1:1..0:0`, whereas a document containing only one newline has `1:1..1:0`.
An end at `L:0` can also be produced when a block closes on a following blank
line. The coordinates are reported without validation or repair.

SoftBreak and LineBreak locate the authored break using this same convention;
their scopes do not promise retrievable string slices. A multiline table cell
can occupy segments on lines shared with other cells. A spanning grid cell can
reach beyond its starting row. These positions describe editor locations,
not a partition of the source into independently sliceable substrings.

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

### Syntax-specific ranges

The following rules describe authored editor positions, not independently
sliceable string ranges. They refine the general coordinate contract. A syntax's
punctuation can be inside its owner's scope without appearing in visible
content. Unscoped semantic fields, including generated anchors and inherited
resources, never gain a range of their own.

| Syntax | Range |
| --- | --- |
| Heading | Includes ATX markers, an optional closing sequence, a Setext underline, and attached attributes. |
| Inline formatting and spans | Includes opening/closing delimiters and any attached container; content children exclude removed suffixes. |
| Link or image | Includes opener, label/alt, tail, dimensions, and occurrence attributes; resolved references keep only their occurrence's range. |
| Autolink | Includes the angle brackets or the complete accepted bare token. |
| Cross link/embed | Includes the optional exclamation mark, both bracket pairs, target, label, and dimensions. |
| Directive | Runs from the colon through the last accepted name, label, or attribute byte. |
| Directive label | Includes its brackets, even when the label is empty. |
| Directive block | Includes its opening and closing lines, or ends at the last consumed content line when unclosed. |
| Block identifier | Remains inside the receiving block's scope after removal from visible content. |
| Metadata | Runs from the first hyphen of the opening fence through the third hyphen of the closing fence. Fields have no individual scopes. |
| Definition list and definition | Ends at the last nonblank line of the final body. A definition includes its term, markers, padding, and every body. |
| Specimen definition | Covers its marker and complete block body. |

For a referenced footnote call, `Cite` covers `[^label]` and its `Citation`
covers `^label`. The `Footnote` covers its definition through the final
continuation line. For an inline footnote, both `Cite` and `Footnote` cover
`^[content]`, while the `Citation` covers the content inside its brackets.
Their descendants retain their own authored ranges.

A bracketed bibliography `Cite` covers the brackets and contents. Each item
runs from its first non-whitespace byte after `[` or `;` through its last
non-whitespace byte before `;` or `]`. An author-in-text cite begins at its
mode marker or `@` and ends at the key or its claimed tail's closer. Its first
item ends at the key when there is no tail or the tail's first section is
keyed; otherwise it ends at the last non-whitespace byte of its suffix. Later
keyed sections follow the bracketed item rule. An item includes neither a
separator semicolon nor a closing bracket. A parenthesized specimen `Cite`
includes parentheses, while its item covers `@label`.

A table includes its claimed caption, whose scope includes the caption marker.
A pipe row covers its physical line; cells cover the segments between pipes,
and synthesized empty cells use the row's end. A simple row covers its line,
and a simple cell covers its trimmed segment. An empty segment uses its start
byte, or the line's last byte if the segment starts beyond the line.

Multiline rows cover their physical lines. Grid rows cover the lines following
their opening boundary through the line before the next row begins, excluding
the final table border. A row without physical content lines uses its closing
boundary. Multiline/grid cells span their first through last line segments,
clipped to each line's end; a cell without physical content lines uses the
corresponding closing-boundary segment. Joined-segment soft breaks cover the
original line ending and may therefore include other columns' bytes in the
contiguous range. A row-spanning grid cell can end below its owning row, as
the containment exception above permits.

## Shared value types

### Placement

`Placement` has exactly two values:

- `embedded`: content participates in surrounding inline flow.
- `standalone`: content is presented independently from surrounding inline
  flow.

Placement and AST containment are related but not interchangeable. In
particular, `Formula` may be `standalone` while remaining inside a paragraph.

`Formula` is the only kind with an explicit `mode` field. `Code` and
`Directive` are inline kinds; `CodeBlock`, `DirectiveBlock`, and `FormulaBlock`
are block kinds. `Comment` is valid in either inline or block content, with
its placement determined by the owning relation rather than a stored mode.

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
records follow inherited declarations without deduplication. Each binding keeps its native collection types and owns all
returned values after the native document is released.

Parsed headings always have a nonempty anchor: an explicit identifier wins,
otherwise the [anchors module](dialect/anchors.md) derives one from parsed
content after reserving every emitted explicit anchor. Generated anchors add
no source range. Writable authored heading labels also define ordinary
reference targets, including forward references. These use `Destination.url`
with the final `#anchor`, no title, and no inherited heading attributes; all
occurrences share the existing reference resource. Explicit definitions win.
`Document.metadata: Metadata?` holds ten named optional values defined by the
[properties grammar](dialect/properties.md). Metadata is never
Markup and has no visitor callbacks. It is produced by the leading properties envelope.
It retains only the envelope scope; absent fields differ from explicit null values.

### Dimensions

`Dimensions(width: Int, height: Int?)` is a node-independent value. Width is
required and height is optional; every present component is in 1..2147483647.
It has no kind, scope, anchor, attributes, children or visitor callbacks.
`Embedded.dimensions` and `CrossEmbedded.dimensions` have type `Dimensions?`, absent when no complete valid suffix was
recognized, including malformed labels. The dimension suffix produces this value from image
labels and embedded cross-link labels. `CrossLink` has no dimensions field.
The value is independent of a destination's shared identity and attribute records.

### Destination

```text
Destination = url(String) | cross(path: String, anchor: String?)
```

`Destination` is a tagged value, not a node: it has no scope, children,
anchor, or attributes, and a branch's fields exist only in that branch. It is
the `dest` of every `Link` and `Embedded`, which own the `url` branch: the
complete semantic destination the inherited grammar produced, the bytes
between angle brackets or the bare destination with backslash escapes and
character references decoded and no percent-encoding, normalization, or
resolution, and possibly empty. The `cross` branch is the workspace address of
the [cross links](dialect/cross-links.md) module and is stored by
`CrossLink` and `CrossEmbedded`. The parser fetches no URL, opens no file, tests no
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
Flow = none | left | center | right
```

`Flow` is a shared value for authored horizontal content alignment. `none`
means no explicit alignment was authored. `TableColumn.flow` uses this
value; its producing syntax determines which value is stored.

### CitationReferent, Citation, Footnote, and Specimen

```text
CitationReferent = bib(key: String, mode: BibMode) | footnote(id: String) | specimen(id: String)

Citation(referent: CitationReferent, prefix: [Markup], suffix: [Markup], scope)

Footnote(id: String, content: [Markup], scope)
Specimen(id: String?, start: Int?, content: [Markup], scope)
```

`CitationReferent` is a tagged value like `Destination`: no scope, and a
branch's fields exist only in that branch. The `bib` branch is produced
by [bibliography citations](dialect/citations.md); every referenced
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
facade exposes `markdown_core_specimen` and its typed accessors. See [specimens](dialect/specimens.md) for definition and reference syntax. Ordinary lists have no specimen variant or label field.

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
| `CodeBlock` | `info: String?`, `language: String?`, `literal: String`, `fenced: Bool`, `closed: Bool` | mode is `standalone`; `info` is trimmed, escape/entity-decoded, and excludes attached attributes; `language` is its first space/tab-delimited token; indented blocks have `fenced=false, closed=true` |
| `HTMLBlock` | `literal: String` | raw HTML is preserved; a block that opens with `<!--` and whose end line holds only whitespace after the first `-->` is a `Comment` |
| `FormulaBlock` | `literal: String` | mode is `standalone` |
| `Table` | `caption: TableCaption?`, `columns: [TableColumn]`, `head: [TableRow]`, `content: [TableRow]`, `foot: [TableRow]` | non-empty columns define the logical grid; rows are owned exactly once in head/content/foot order; pipe tables have one head row, no foot rows, null relative widths, and unit spans; no span crosses a group boundary |
| `TableCaption` | `content: [Markup]` | independently owned inline caption, visited before the table row groups |
| `TableRow` | `cells: [TableCell]` | cells whose upper-left coordinate starts in this row, in logical order; no row-local header state |
| `TableCell` | `rowspan: Int`, `colspan: Int`, `content: [Markup]` | positive spans; inline or block content is stored as parsed without paragraph normalization |
| `DirectiveBlock` | `name: String?`, `label: DirectiveLabel?`, `content: [Markup]` | null name for a nameless container, otherwise a letter-first name; attributes use the inherited fields; label is a typed Markup field spanning its brackets, never content; absent and empty labels remain distinct; block content |
| `DirectiveLabel` | `content: [Markup]` | inline content; the scope spans the brackets, so an empty label is still a place |
| `Text` | `literal: String` | leaf |
| `SoftBreak` | none | leaf |
| `LineBreak` | none | leaf |
| `Code` | `literal: String` | mode is `embedded`; leaf |
| `HTML` | `literal: String` | raw HTML is preserved; an HTML comment token is a `Comment`; leaf |
| `CrossLink` | `dest: Destination`, `label: String?` | inline leaf; cross destination; complete raw label; no separator means null |
| `CrossEmbedded` | `dest: Destination`, `label: String?`, `dimensions: Dimensions?` | inline leaf; workspace transclusion; cross destination; label is the raw prefix after a valid size suffix; no separator means null |
| `Comment` | `literal: String` | an HTML comment or a `%%` comment, the one kind valid in both block and inline content, which the parent edge records; `literal` excludes the delimiters and keeps every byte between them; leaf |
| `Formula` | `mode: Placement`, `literal: String` | either mode; leaf |
| `Emphasis` | `content: [Markup]` | inline content |
| `Strong` | `content: [Markup]` | inline content |
| `Strikethrough` | `content: [Markup]` | inline content |
| `Mark` | `content: [Markup]` | inline content |
| `Insertion` | `content: [Markup]` | inline content |
| `Span` | `content: [Markup]` | inline content; may be empty |
| `Superscript` | `content: [Markup]` | inline content; empty bodies are retained |
| `Subscript` | `content: [Markup]` | inline content; non-empty body |
| `Link` | `dest: Destination`, `title: String?`, `content: [Markup]` | `dest` is the tagged `Destination` value and is never absent: `[a]()` and `[a](<>)` wrote one and wrote nothing in it, so it is `url("")`; a reference occurrence answers the destination its definition stated, and an unresolved reference is the inherited literal text; every `Link` owns the `url` branch; absent and empty title remain distinct; inline content |
| `Embedded` | `dest: Destination`, `title: String?`, `dimensions: Dimensions?`, `content: [Markup]` | `dest` is the tagged `Destination` value and is never absent, for the reason `Link.dest` is not; every `Embedded` owns the `url` branch; absent and empty title remain distinct; content is parsed alt-text inline content |
| `Directive` | `name: String`, `label: DirectiveLabel?` | letter-first name; attributes use the inherited fields; label is a typed Markup field spanning its brackets, never content; absent and empty labels remain distinct; leaf |
| `Cite` | `citations: [Citation]` | one or more items in source order; every item has exactly one referent and one cite never mixes referent families; an inherited `[^label]` call is one item with a `footnote` referent whose id is the normalized label without the caret and with empty affixes; its items are scoped values, never children, so it is a leaf |
| `DefinitionList` | `definitions: [Definition]` | non-empty ordered associations |
| `Definition` | `term: [Markup]`, `content: [[Markup]]`, `compact: Bool` | inline term; non-empty outer content; each inner collection is one block body; compact records the absence of a blank term gap; visit term then bodies |

Every row also has the ordered inherited fields `scope: Scope`,
`anchor: String?`, and `attributes: Attributes`; they are not repeated in the table. The `url` of a `Link` or `Embedded` destination, and
every `title`, are the CommonMark-unescaped values with angle-bracket
wrappers removed and no percent-encoding or normalization. A link reference
definition produces no node: the parser consumes it, and every successful
full, collapsed, shortcut, or autolink form is the `Link` or `Embedded` it names,
with the definition's destination and title and its own occurrence scope. An
unresolved reference is the inherited literal text with its brackets.

### Typed table ownership

```text
Table(caption: TableCaption?, columns: [TableColumn], head: [TableRow], content: [TableRow], foot: [TableRow], scope)
TableCaption(content: [Markup], scope)
TableColumn(flow: Flow, relative: Double?)
TableRow(cells: [TableCell], scope)
TableCell(rowspan: Int, colspan: Int, content: [Markup], scope)
```

Tables, rows, and cells are immutable `Markup`; a column is an unscoped value.
The C child chain contains the rows in head/content/foot order, partitioned by
counts stored on the table. No row duplicates its owning group's identity.
Bindings expose the three named arrays directly. Walkers visit the independently
owned caption first, then the three row groups in that order. Pipe tables produce one head row, body rows in `content`, empty
`foot`, `relative=null`, and unit spans. Missing cells keep their scope at the
row's end. Pipe and simple cells keep their inline nodes directly; multiline and grid
cells store the ordinary parsed block sequence in the same `content` field.

The [tables guide](dialect/tables.md) defines the source geometry.
To recover logical coordinates, process head, content, and foot rows in order.
Track columns occupied by active rowspans; place each starting cell in the first
free column, occupying its colspan for its rowspan. Every row must be completely
covered, with no overlap or overrun, and no span may cross a row-group boundary.
A row with no starting cells is valid only when earlier spans cover it completely. `columns` is non-empty; a present `relative`
is a positive finite authored width share. `TableCaption.content` contains
inlines, and its scope includes its authored marker. Table scope includes the
caption, whether it precedes or follows the grid. Rows retain source-defined boundaries, including a row with `cells=[]` when
all its coordinates are covered by earlier spans. An authored empty cell is
instead a `TableCell` with empty `content`. A spanning cell is stored once in
its starting row; the parser does not emit covered-coordinate placeholders or
layout-derived rows. Consumers recover coordinate occupancy and layout from
the ordered rows and spans.

## Parsing

`Document.parse(source)` is the binding entry point; C exposes
`markdown_core_document_parse`. Both take the source without parse options and
return the same fixed dialect. The document does not retain source text,
a normalized source copy, a line index, tokens, trivia, or recovery records.
Scope tracking is mandatory.

Allocation failure aborts parsing and publishes no partial document. A binding
that detects a category violation in the C facade fails with its platform
contract-violation error. Valid UTF-8 is a precondition of the C API; the
bindings provide valid UTF-8 input.

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
