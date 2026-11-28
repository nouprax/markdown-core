# @nouprax/es-markdown-core

Cross links (`[[Note#Heading|Label]]`) and embeds (`![[Image.png|100x145]]`)
produce `CrossLink(dest, label)` and `CrossEmbed(dest, label, dimensions)`,
respectively. `dest` is a cross destination with
raw path and optional anchor; the label is null when no separator was authored
and an empty string for `[[Note|]]`. These are leaves with exhaustive visit and
walk callbacks. Resolving files, rendering and transclusion belong to consumers.

Immutable ECMAScript and TypeScript bindings for Markdown Core, backed by the
same C parser compiled to WebAssembly.

## Install

```sh
pnpm add @nouprax/es-markdown-core
```

The package is ESM-only and supports Node.js 20 or later and browsers that can
load its WebAssembly asset. Importing the module completes WebAssembly
initialization, so `Document.parse` is synchronous.

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
outside `Document.content`. The walker reports its `"enter"` and `"exit"`
callbacks before content, even when all metadata fields are absent. Every
`MarkupVisitor` must provide the `metadata(node, phase)` callback; the node's
scalar/list values do not receive separate Markup callbacks.
Numbers retain exact decimal strings. Missing fields are null; an authored null
is a present scalar value. No field order or individual field scope is stored.
`Embed.dimensions: Dimensions | null` reads complete `W`, `WxH`, `alt|W` and
`alt|WxH` suffixes on direct and resolved images. Values range from 1 to
2147483647 without leading zeros; malformed suffixes remain parsed alt content.
Numeric-only labels have empty alt content. Embedded cross links use the same
size grammar in `CrossEmbed.dimensions`, retaining the raw label prefix (empty
for size-only labels). Ordinary cross-link labels and invalid suffixes stay raw.
`Dimensions` is a node-independent value with required `width` and optional
`height`; it has no scope or visitor callbacks.

## Parse Markdown

```js
import { Document, MarkupDumper } from "@nouprax/es-markdown-core";

const source = "# Hello";
const document = Document.parse(source);

console.log(document.content[0].kind);
console.log(document.dump(source));
console.log(MarkupDumper.dump(document, document.content[0], source));
```

`Document.parse` takes one option, the text `unit` that scope queries count
columns in: `"utf16"` by default, or `"utf8"`. It parses the one Markdown Core dialect,
in which every feature is always recognized: footnotes, tables,
strikethrough, autolinks, task lists, formulas, and directives, on the
CommonMark base. Quotation marks, hyphens, and periods are stored as written.

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

`^[inline note]` produces a one-item `Cite` whose citation's referent owns the
note's `Footnote`, `{ kind: "footnote", target: { kind: "note", footnote } }`,
with a null `label` and the parsed inline body as its content. A referenced
definition `[^x]: body` is a `Footnote` block where it was written, and a
`[^x]` call names it by label. `Document.footnotes` lists every footnote,
inline notes included, in source order, and `document.footnote(label)`
returns the first one with that label.

A link reference definition `[r]: /u "t"` is a `Reference` block where it was
written, with its normalized `label`, its `url` destination and its `title`.
A reference link or image `[text][r]`, `[r]` or `![alt][r]` has the
destination `{ kind: "reference", label: "r" }` and a null title.
`Document.references` lists every `Reference` in source order, and
`document.reference(label)` returns the `Reference` a label names: the first
one with that label, else null.

`%%comment%%` produces `Comment`, the kind an HTML comment already produces,
inline or as a block when both `%%` fences stand on lines of their own under
the same container prefixes. The body is opaque and stored as written, nothing
is stripped, and a consumer that does not want comments drops the nodes.

`Document.parse` returns a discriminated `Markup` union with recursively
readonly TypeScript properties. The JavaScript objects are not
runtime-frozen. The package exposes parsing and typed AST inspection, not
rendering or AST mutation.
Ordered lists expose their `delimiter`, `"period"` for `1.` and `"parenthesis"` for `1)`.

A task prefix is `[ ]`, `[x]` or `[X]`, `[?]` or `[-]`, followed by a space or
tab; the item's `status` is open, done, question or cancelled, and null for
an ordinary item. Recognition is limited to the item's opening line and
removes the prefix before deciding its first block. Any other character
between the brackets, and a missing separator, remain literal text.

Bracketed spans (`[text]{.class}`) produce `Span(content)` with the shared
anchor and attributes. Superscript (`^text^`) and subscript (`~text~`) retain
parsed inline content; their bodies must be non-empty and contain no raw
whitespace. An escaped ASCII space within a completed body becomes NBSP.
Strikethrough uses `~~text~~`. All three kinds support typed visitor callbacks, and their scopes include their authored delimiters.
Their ES kind tags are `"span"`, `"superscript"` and `"subscript"`.

Every fenced container is named (`:::name`), and `DirectiveBlock.name` is
never null. A term followed by `: body` produces `DefinitionList` with
ordered `definitions`. Each `Definition` has an inline `term`, ordered block-body
arrays in `content`, and `compact` determined by the blank line before its first
body. A body can be empty. Walking visits the term and then the bodies without
introducing extra Markup wrappers.

## Identity, equality and scopes

Every node has `id`, a number below 2^53 that is unique within its document
and numbered from 1 in canonical walk order by a parse, so two parses of one
text are equal, ids included. Use it as a list key. `markupEquals(a, b)` is
deep value equality including ids, the comparator for
`React.memo(component, (a, b) => markupEquals(a.node, b.node))`.

A node stores its `extent`, `{ lead, span }` in bytes, never a line or
column: a block's in the UTF-8 source, an inline node's in the content of its
inline root, which starts at 0. Its `runs` are its own source ranges in source
order, each `{ lead, span }` in bytes, and every node has at least one. The
first run's lead is from the end of the source of the previous node in the
same relation, or from the start of the owner's source for a relation's first
node; every other run's lead is from the end of the run before. A node's
source starts where its first run starts and ends where its last ends, and the
bytes between two runs are not the node's, such as the `> ` prefixes between
the lines of a paragraph in a block quote; runs that touch are one run, so a
block whose own source is its range has one run, its range. A table's rows, in
head then content order, are one relation, so the extent and the first
run of its first body row lead from the end of its last head row.
`document.scope(node, source)` answers one scope per run of the node, in
source order, and `document.nodeAt(position, source)` the last node one of
whose runs holds the position; both compute them on request from the runs and
the source the document was parsed from, with columns in the document's unit,
and extents take no part.

## Errors

Every failure is a `MarkdownCoreError`, whose `code` is one `ErrorCode`:

- `"allocationFailed"`: a parse could not allocate, or the text's UTF-8
  exceeds the engine's 1 GiB capacity.
- `"outOfBounds"`: `scope` or `dump` got a source that ends before the node
  does, or `nodeAt` got a line or column that is not an integer of at least 1.
  A position past the source, or one no node holds, is not an error: `nodeAt`
  returns `null`.
- `"kindMismatch"`: the engine's status for a value that is not of the kind a
  call reads. The binding decodes the whole tree into typed values, so none of
  its calls reports it today; the code keeps the set equal to the engine's.

A node of another document is not checked: pass the document's own nodes.

## Traverse and Inspect

Source files are grouped into `common` (shared constraints and support),
`markup` (nodes and their values), and `visitor` (callbacks, traversal, and dump).
The public callback interface is named `MarkupVisitor` in all three bindings.

`walk(markup, visitor)` performs a stack-safe depth-first traversal and drives
all `MarkupVisitor` callbacks, supplying `"enter"` before descendants and
`"exit"` after them. Consumers accumulate results in their own state; there
is no separate single-node dispatch API. Callbacks return `undefined`, which
rejects return values that TypeScript's `void` would silently discard.

Callback keys match `kind` tags, and each parameter has its concrete node type:

```typescript
type MarkupVisitor = {
    [Kind in Markup["kind"]]: (
        this: void,
        node: Extract<Markup, { kind: Kind }>,
        phase: MarkupVisitPhase
    ) => undefined;
};
```

Object methods such as `heading(node, phase) { ... }` infer `Heading` and
`MarkupVisitPhase` automatically. Internally, the walker indexes callbacks by
`node.kind`; its mapped union preserves the correlation without `any` or casts.

Every callback is required; missing callbacks are compile errors. Metadata,
citations and footnotes are Markup and use the same callbacks.
Visitors process callbacks without recursively visiting descendants; the dumper
uses the same traversal. The walker schedules each node's typed fields in canonical order. A directive
label remains the named `label` field, outside directive content.

`MarkupDumper.dump(document, source)`, `MarkupDumper.dump(document, node, source)`
and the document's `dump(source)` and `dump(node, source)` emit the canonical
debug tree for a complete document or focused subtree, with scopes computed
from the source in UTF-8 columns. The
text is intended for logs, snapshots, and debugging rather than persistence or
data interchange.

Tables are pipe tables. A table has its `columns`, one `head` row and its
body rows in `content`; walkers visit `head` before `content`. Every cell
holds inline `content`; an authored empty cell retains `content=[]`.

Attributes attach to inline code (``x`{.code}`), ATX and Setext headings,
fenced code, direct links/media, resolved references, reference definitions
and angle autolinks. Each node has the attributes written on it: a `Reference`
has the anchor, classes and records its definition states, and a reference
occurrence has its own. Image dimension suffixes and dimension attribute records
remain independent. All returned values use the binding's native collections
and remain usable after parsing finishes.

A heading's anchor is the one its attributes write, `# Hello World {#hello}`
declaring `hello`, which `[go](#hello)` links to; a heading without one has no
anchor, and the parser derives none from its text. Heading attributes stay on
the heading.

### Pandoc-derived syntax

The always-on dialect includes inline code, heading, fenced code and link
attributes; bracketed spans; superscript and subscript;
bibliography citations; and definition
lists. These feature groups use one immutable AST and the same behavior on
every binding. The [dialect contract](../../docs/specs/dialect.md) specifies
syntax, precedence and intentional differences from the pinned Pandoc reader.
Citation numbering, table coordinate expansion and rendering remain consumer
responsibilities.
