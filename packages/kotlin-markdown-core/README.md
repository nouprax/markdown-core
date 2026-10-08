# Kotlin Markdown Core

Cross links (`[[Note#Heading|Label]]`) and embeds (`![[Image.png|100x145]]`)
produce `CrossLink(dest, label)` and `CrossEmbedded(dest, label, dimensions)`,
respectively. `dest` is a cross destination with
raw path and optional anchor; the label is null when no separator was authored
and an empty string for `[[Note|]]`. These are leaves with exhaustive visit and
walk callbacks. Resolving files, rendering and transclusion belong to consumers.

Kotlin Multiplatform bindings for the immutable Markdown Core AST.

## Add the Dependency

Use the root coordinate from a Kotlin Multiplatform or Android project:

```kotlin
kotlin {
    sourceSets {
        commonMain.dependencies {
            implementation("com.nouprax:kotlin-markdown-core:3.0.0")
        }
    }
}
```

JVM-only Gradle and Maven consumers can use
`com.nouprax:kotlin-markdown-core-jvm:3.0.0`. Published targets are Android API
21 or later, JVM 17, macOS arm64, and Linux x64.

Every `>` container is a `Callout`. An opening `[!type]` line stores the type
as written in `variant`; optional `+` and `-` set `collapsed` to false and true.
The parsed inline `title` is visited before `content` and is never a content
child. A plain quote has null metadata, and a missing title stays null. Custom
types are preserved; default titles, aliases and styling belong to consumers.

Every Markup value also exposes `anchor` and `attributes`. Attributes contain
ordered `classes` and ordered `records` (`name`, `value`), with duplicates
preserved. Directives populate these fields through the shared Pandoc braced
attribute grammar; an absent or empty container produces empty attributes.
`Document.metadata` holds the first complete `---` envelope at the start of a
document. Metadata directly exposes ten optional fields:
`name`, `title`, `subtitle`, `time`, `date`, `authors`, `keywords`, `abstract`,
`state`, and `comment`. Unknown names, unnamed text, comments, invalid values,
and later duplicates are ignored; valid neighboring fields survive. `authors`
and `keywords` accept a single string, a bracketed array, or a block list.
`abstract` and `comment` accept single-line text and indented multiline text
with `: |`. `Metadata` is a leaf Markup node stored in `Document.metadata`,
outside `Document.content`. The walker reports its `ENTER` and `EXIT` callbacks
before content, even when all metadata fields are absent. Every `MarkupVisitor`
must implement `visit(metadata: Metadata, phase: MarkupVisitPhase)`; the node's
scalar/list values do not receive separate Markup callbacks.
Numbers retain exact decimal strings. Missing fields are null; an authored null
is a present scalar value. No field order or individual field scope is stored.
`Embedded.dimensions: Dimensions?` reads complete `W`, `WxH`, `alt|W` and
`alt|WxH` suffixes on direct and resolved images. Values range from 1 to
2147483647 without leading zeros; malformed suffixes remain parsed alt content.
Numeric-only labels have empty alt content. Embedded cross links use the same
size grammar in `CrossEmbedded.dimensions`, retaining the raw label prefix (empty
for size-only labels). Ordinary cross-link labels and invalid suffixes stay raw.
`Dimensions` is a node-independent value with required `width` and optional
`height`; it has no scope or visitor callbacks.

## Parse Markdown

```kotlin
import com.nouprax.markdown.core.Document

val source = "# Hello"
val document = Document.parse(source)

println(document.content.first()::class.simpleName)
println(document.dump(source))
```

`Document.parse` takes no dialect options. It parses the one Markdown Core
dialect, in which every feature is always recognized: footnotes, tables,
strikethrough, autolinks, task lists, formulas, and directives, on the
CommonMark base. Quotation marks, hyphens, and periods are stored as written.
The result is an immutable value tree. The package exposes parsing, editing
through a session, and typed AST inspection, not rendering.

### Sessions

A `MarkdownSession` holds a text and the document parsed from it, and changes
both with each edit. The new document continues the previous one: a node that
continues an old node keeps its id, so a Compose `key` survives the edit.

```kotlin
MarkdownSession("# Title\n\nfirst\n").use { session ->   // TextUnit.UTF16 by default
    session.edit(listOf(TextEdit(9, 14, "edited")))   // one batch, parsed once
    session.append("\n> quote\n")
    val document = session.document                  // the tree of Document.parse(session.text), ids aside
}
```

`edit` is the one way to change a range: a single replacement is a batch of
one. A batch lists disjoint ranges in the text before it, in any order, and two
edits at one offset apply in the order listed. Offsets count in the session's
`unit`, which is also how its documents count scope columns. `close` releases
the engine's session; the documents it returned stay complete values.

### Identity, equality and scopes

Every node has an `id: MarkupID`, unique within its document across every
owned relation and numbered from 1 by a parse in the order its nodes
complete: a node's owner numbers the nodes it holds as it completes, and the
document numbers itself last. Two parses of one text are equal, ids included. Ids suit Compose `key` in lazy lists.
`equals` is deep value equality: the same kind, id, scalar fields, extent,
runs and pairwise equal children in every relation, compared with an explicit work
stack after a reference check. `hashCode` reads the id alone.

A node stores no line or column. Its `extent: Extent(lead, span)` is the raw
byte range the engine keeps, a block's in the UTF-8 source and an inline
node's in its inline root's content, which starts at 0: `lead` is signed, from
the end of the previous node in the same relation (or the owner's start) to
the node's start, and `span` is its length. Its `runs` are its own source
ranges in source order, each `Run(lead, span)` in bytes, and every node has at
least one. The first run's lead is from the end of the source of the previous
node in the same relation, or from the start of the owner's source for a
relation's first node; every other run's lead is from the end of the run
before. A node's source starts where its first run starts and ends where its
last ends, and the source between two runs is not the node's; runs that touch
are one run, so a block whose own source is its range has exactly one run, its
range. Scopes, one per run in source order, are computed on request from the
runs and the source the document was parsed from:

```kotlin
val document = Document.parse(source)            // TextUnit.UTF16 by default
val scopes = document.scope(node, source)        // [Scope], columns in document.unit
val hit = document.node(Position(3, 7), source)  // the last node in walk order one of whose ranges holds that scalar
```

`Document.parse(source, unit)` chooses how those queries count columns:
`TextUnit.UTF16` (the default, as Android `Editable` and Compose
`TextFieldValue` count) or `TextUnit.UTF8`. A position names the start of a
scalar. The canonical dump always prints UTF-8 columns.

### Errors

The library throws one exception, `MarkdownCoreException`, whose `code` says
why:

- `ErrorCode.ALLOCATION_FAILED`: `Document.parse` or a session step could not
  allocate, or the text exceeds 1 GiB of UTF-8, or its tree exceeds a byte
  array's capacity.
- `ErrorCode.OUT_OF_BOUNDS`: `scope` or `dump` got a source that ends before
  a node's last range does, or `node` got a position whose line or column is below 1. A
  position past the source, or one no node holds, answers `null`. A session
  edit whose range starts after its end, ends past the text or overlaps
  another edit of its batch is `OUT_OF_BOUNDS` too.
- `ErrorCode.KIND_MISMATCH`: the engine's code for a value read as the wrong
  kind. It is shared by every binding; the typed Kotlin nodes never reach it.
- `ErrorCode.INSIDE_SCALAR`: a session edit has an offset inside a scalar: at
  a continuation byte in UTF-8, or between the two halves of a surrogate pair
  in UTF-16.

A node of another document is not checked, and the answer for it means
nothing.

### Compose

The Compose compiler treats classes from a module it did not compile as
unstable. This package adds no Compose dependency; it ships
[`compose-stability.conf`](compose-stability.conf), which lists the AST types
as stable. Copy it into your project and add it to the Compose compiler:

```kotlin
composeCompiler {
    stabilityConfigurationFiles.add(layout.projectDirectory.file("compose-stability.conf"))
}
```
Ordered lists expose their `variant` and `delimiter`.

Task prefixes accept exactly one authored Unicode scalar, such as `- [?]`,
`- [✓]`, or `- [🚀]`, followed by a space, tab, vertical tab, or form feed.
The item preserves that scalar in `marker`; completion is derived. Recognition
is limited to the item's opening line and removes the prefix before deciding
its first block. Empty or multi-scalar markers and a missing separator remain
literal text.

[Block identifiers](../../docs/specs/dialect/block-identifiers.md) use `text #id#` to populate
`Paragraph.anchor`, or `- [✓] task #id#` to populate `ListItem.anchor`.
A standalone identifier line can attach to an eligible preceding list,
callout, or table under the module's boundary rules. The marker disappears
from visible content, while scopes retain the authored source positions.
Identifiers are declarations; target resolution belongs to consumers.

Within a pipe-table cell, write a cross-link label separator as `\|`, as in
`[[Note\|Label]]` or `![[asset\|100x145]]`. It remains inside that cell.
Inline `$x$` and display `$$` forms use `Formula` and `FormulaBlock` under the
[formula grammar](../../docs/specs/dialect/formulas.md). Ordinary fenced code remains `CodeBlock`
with its info, language label, and literal body. Code, formula, comment, HTML
token, and cross-reference payloads retain their ownership boundaries; text
between paired inline HTML tags remains eligible for Markdown parsing.

`==highlight==` produces `Mark` with parsed inline `content`, including nested
emphasis, links, and other inline nodes. Matching consumes two equals signs
at a time; unmatched signs remain text. Typed visitor callbacks
include the `Mark` case, and its scope covers both delimiters and the body.

`++inserted++` produces `Insertion` with parsed inline `content`. Repeated pairs
nest (`++++text++++`), and an odd leftover plus stays outside the matching
pairs (`+++text+++`). Insertion participates in exhaustive visitor callbacks;
its scope includes the delimiters. Escapes and opaque bodies retain literal plus signs.

Footnote and specimen definitions stay in the tree where they were written. A
referenced definition `[^x]: body` is a `Footnote` block with its normalized
`label`. `^[inline note]` produces a one-item `Cite` whose citation owns its
note: `CitationReferent.Footnote(FootnoteTarget.Note(footnote))`, a `Footnote`
with a null label whose content holds the parsed inline body directly, visited
before the citation's prefix and suffix. `Document.footnotes` and
`Document.specimens` list every definition, inline notes included, in source
order, and `document.footnote(label)` and `document.specimen(label)` return the
first one whose label equals the referent's; an inline note is never found by
label.

A link reference definition `[label]: /url "title"` is a `Reference` block where
it was written, with its normalized `label`, a `Destination.Url` `dest`, its
`title`, and the anchor and attributes it states. A reference occurrence --
`[text][label]`, `[label][]`, `[label]` or `![alt][label]` -- is a `Link` or
`Embedded` whose `dest` is `Destination.Reference(label)` with a null title.
`Document.references` lists every definition in source order, and
`document.reference(label)` returns the node the label resolves to: the first
`Reference` with that label, else the first `Heading` whose text declares it.

`%%comment%%` produces `Comment`, the kind an HTML comment already produces,
inline or as a block when both `%%` fences stand on lines of their own under
the same container prefixes. The body is opaque and stored as written, nothing
is stripped, and a consumer that does not want comments drops the nodes.

Bracketed spans (`[text]{.class}`) produce `Span(content)` with the shared
anchor and attributes. Superscript (`^text^`) and subscript (`~text~`) retain
parsed inline content; their bodies must be non-empty and contain no raw
whitespace. An escaped ASCII space within a completed body becomes NBSP.
Strikethrough uses `~~text~~`. All three kinds support typed visitor callbacks, and their scopes include their authored delimiters.

Nameless fenced containers (`::: {.class}` or `::: class`) expose
`DirectiveBlock.name` as null; named and nameless forms share closing and nesting
rules. A term followed by `: body` or `~ body` produces `DefinitionList` with
ordered `definitions`. Each `Definition` has an inline `term`, ordered block-body
arrays in `content`, and `compact` determined by the blank line before its first
body. A body can be empty. Walking visits the term and then the bodies without
introducing extra Markup wrappers.

## Traverse and Inspect

Source files are grouped into `common` (shared constraints and support),
`markup` (nodes and their values), and `visitor` (callbacks, traversal, and dump).
The public callback interface is named `MarkupVisitor` in all three bindings.

`Markup.walk(visitor)` performs a stack-safe depth-first traversal and drives
all `MarkupVisitor` callbacks. Each required overload receives a concrete node and
`MarkupVisitPhase` and returns `Unit`. The walker supplies `ENTER` before a
node's descendants and `EXIT` after them. Consumers accumulate results in
visitor state; there is no separate single-node dispatch API.

`MarkupVisitor` requires a `fun visit(embedded: Embedded, phase: MarkupVisitPhase): Unit`
overload for every concrete kind. Implementations use the declared parameter
names, including `paragraph`, `tableRow`, and `citation`. Adding a kind makes
incomplete implementations fail to compile. Visitors process callbacks without
recursively visiting descendants; the dumper uses this same traversal.

Traversal schedules each node's typed fields in canonical order. A directive
label remains the named `label` field, outside directive content. Metadata,
citations, footnotes and specimens are Markup and use the same callbacks.

A dump prints scopes, so it takes the source like a scope query:
`document.dump(source)` and `document.dump(node, source)` delegate to the
public `MarkupDumper` and return the canonical file-tree dump of the document
or of one of its subtrees:

```kotlin
import com.nouprax.markdown.core.MarkupDumper

val source = "# Hello"
val document = Document.parse(source)
println(document.dump(source))
println(MarkupDumper.dump(document, document.content.first(), source))
```

On JDK 26 and later, JVM applications should launch with
`--enable-native-access=ALL-UNNAMED` so the package-private JNI loader can load
the bundled native library without a restricted-native-access warning.

Pipe, simple, multiline and grid tables share one model. `caption` is an
optional `TableCaption` with inline `content`; `Table:`, `table:` and `:` accept
a preceding or following caption. Between tables, an uncaptained preceding
table claims it first. Walkers visit the caption before `head`, `content`, and
`foot`. Multiline and grid cells use ordinary block content and authored relative
widths; pipe and simple cells use inline content and null widths. Grid cells
carry `rowspan` and `colspan` once in their starting row. Source-defined rows
with no starting cells retain `cells=[]`; an authored empty cell retains
`content=[]`. Consumers derive occupied coordinates and layout from these rows
and spans.

Attributes attach to inline code (``x`{.code}`), ATX and Setext headings,
fenced code, links/media, reference definitions and angle autolinks. Each node
holds the anchor, classes and records written on it, including duplicates: a
reference occurrence holds its own, and the `Reference` it names holds the
definition's. Image dimension suffixes and dimension attribute records
remain independent. All returned values use the binding's native collections
and remain usable after parsing finishes.

Parsed headings receive automatic anchors: `# Hello World` declares
`hello-world`, with `-1`, `-2`, and later suffixes for collisions. Explicit
anchors anywhere in the document are reserved first. `[Hello World]`,
`[Hello World][]`, and `[go][Hello World]` name `reference("hello world")`, and
`document.reference("hello world")` answers the heading, including before the
heading; an explicit reference definition takes priority. Labels
use authored heading text, so `# *Title*` is referenced by `[*Title*]`.
Heading attributes stay on the heading, and generated targets add no scope.

### Pandoc-derived syntax

The always-on dialect includes inline code, heading, fenced code and link
attributes; automatic anchors and implicit heading references; bracketed spans;
superscript and subscript; bibliography citations; named and nameless fenced
containers; fancy ordered lists and example lists; definition lists; table
captions; and simple, multiline and grid tables. These 18 feature groups use
one immutable AST and the same behavior on every binding. The
[dialect contract](../../docs/specs/dialect.md) specifies syntax, precedence and intentional differences
from the pinned Pandoc reader. Citation numbering, example-list resolution,
table coordinate expansion and rendering remain consumer responsibilities.
