# @nouprax/es-markdown-core

Cross links (`[[Note#Heading|Label]]`) and embeds (`![[Image.png|100x145]]`)
produce `CrossLink(dest, label)` and `CrossEmbedded(dest, label, dimensions)`,
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
with `: |`. Metadata stays outside Markup children and visitor callbacks.
Numbers retain exact decimal strings. Missing fields are null; an authored null
is a present scalar value. No field order or individual field scope is stored.
`Media.dimensions: Dimensions | null` reads complete `W`, `WxH`, `alt|W` and
`alt|WxH` suffixes on direct and resolved images. Values range from 1 to
2147483647 without leading zeros; malformed suffixes remain parsed alt content.
Numeric-only labels have empty alt content. Embedded cross links use the same
size grammar in `CrossEmbedded.dimensions`, retaining the raw label prefix (empty
for size-only labels). Ordinary cross-link labels and invalid suffixes stay raw.
`Dimensions` is a node-independent value with required `width` and optional
`height`; it has no scope or visitor callbacks.

## Parse Markdown

```js
import { Document, TreeDumper } from "@nouprax/es-markdown-core";

const document = Document.parse("# Hello");

console.log(document.content[0].kind);
console.log(document.dump());
console.log(TreeDumper.dump(document.content[0]));
```

`Document.parse` takes no options. It parses the one Markdown Core dialect,
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
at a time; unmatched signs remain text. Typed visitors and walking visitors
include the `Mark` case, and its scope covers both delimiters and the body.

`++inserted++` produces `Insertion` with parsed inline `content`. Repeated pairs
nest (`++++text++++`), and an odd leftover plus stays outside the matching
pairs (`+++text+++`). Insertion participates in exhaustive and walking visitors;
its scope includes the delimiters. Escapes and opaque bodies retain literal plus signs.

`^[inline note]` produces a one-item `Cite` and a document-owned `Footnote`
whose content holds the parsed inline body directly. Referenced definitions and
inline notes share `Document.footnotes` in source order. Generated `inline-N`
ids avoid every authored id; nested calls remain id edges and can be visited
without following semantic cycles.

`%%comment%%` produces `Comment`, the kind an HTML comment already produces,
inline or as a block when both `%%` fences stand on lines of their own under
the same container prefixes. The body is opaque and stored as written, nothing
is stripped, and a consumer that does not want comments drops the nodes.

`Document.parse` returns a discriminated `Markup` union with source scopes and
recursively readonly TypeScript properties. The JavaScript objects are not
runtime-frozen. The package exposes parsing and typed AST inspection, not
rendering or AST mutation.
Ordered lists expose their `variant` and `delimiter`.

Task prefixes accept exactly one authored Unicode scalar, such as `- [?]`,
`- [✓]`, or `- [🚀]`, followed by a space, tab, vertical tab, or form feed.
The item preserves that scalar in `marker`; completion is derived. Recognition
is limited to the item's opening line and removes the prefix before deciding
its first block. Empty or multi-scalar markers and a missing separator remain
literal text.

Bracketed spans (`[text]{.class}`) produce `Span(content)` with the shared
anchor and attributes. Superscript (`^text^`) and subscript (`~text~`) retain
parsed inline content; their bodies must be non-empty and contain no raw
whitespace. An escaped ASCII space within a completed body becomes NBSP.
Strikethrough uses `~~text~~`. All three kinds support typed visitors and
walking visitors, and their scopes include their authored delimiters.
Their ES kind tags are `"span"`, `"superscript"` and `"subscript"`.

Nameless fenced containers (`::: {.class}` or `::: class`) expose
`DirectiveBlock.name` as null; named and nameless forms share closing and nesting
rules. A term followed by `: body` or `~ body` produces `DefinitionList` with
ordered `definitions`. Each `Definition` has an inline `term`, ordered block-body
arrays in `content`, and `compact` determined by the blank line before its first
body. A body can be empty. Walking visits the term and then the bodies without
introducing extra Markup wrappers.

## Traverse and Inspect

`visit(markup, visitor)` dispatches exactly one node to an exhaustive typed
`Visitor`. `walk(markup, walkingVisitor)` performs a stack-safe depth-first walk
and dispatches `entering` and `exiting` to an exhaustive `WalkingVisitor` by
node kind. Each node-kind branch chooses its typed fields and content; there is
no public iterator or uniform child projection. A directive label is walked as
the named `label` field, not as directive content.

`TreeDumper.dump(markup)` and each Markup's non-enumerable `dump()` method emit
the canonical debug tree for a complete document or focused subtree. The
text is intended for logs, snapshots, and debugging rather than persistence or
data interchange.

Tables expose `columns`, `head`, `content`, and `foot`. Each `TableColumn` has
`alignment` and nullable `relative`; each `TableCell` has `rowspan`, `colspan`,
and direct inline or block `content`. Rows carry their cells and scope; group
ownership belongs to the table. Pipe tables have unit spans and no authored widths.

Attributes attach to inline code (``x`{.code}`), ATX and Setext headings,
fenced code, direct links/media, resolved references and angle autolinks.
Reference definitions can supply an anchor, classes and records. An occurrence's
nonempty anchor wins; its classes and records follow inherited declarations,
including duplicates. Image dimension suffixes and dimension attribute records
remain independent. All returned values use the binding's native collections
and remain usable after parsing finishes.

Parsed headings receive automatic anchors: `# Hello World` declares
`hello-world`, with `-1`, `-2`, and later suffixes for collisions. Explicit
anchors anywhere in the document are reserved first. `[Hello World]`,
`[Hello World][]`, and `[go][Hello World]` resolve to `#hello-world`, including
before the heading; an explicit reference definition takes priority. Labels
use authored heading text, so `# *Title*` is referenced by `[*Title*]`.
Heading attributes stay on the heading, and generated targets add no scope.
