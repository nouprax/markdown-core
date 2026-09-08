# @nouprax/es-markdown-core

Cross links (`[[Note#Heading|Label]]`) and embeds (`![[Image.png|100x145]]`)
produce `CrossLink(embedded, dest, label)`. `dest` is a cross destination with
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

Every Markup value also exposes `anchor` and `attributes`. Attributes contain
ordered `classes` and ordered `records` (`name`, `value`), with duplicates
preserved. Directives populate these fields through the shared Pandoc braced
attribute grammar; an absent or empty container produces empty attributes.
`Document.metadata` holds the first complete `---` envelope at the start of a
document. Its ordered `content` contains `data(MetadataRecord)` and
`comment(String)` cases: supported YAML properties become data, while comments,
non-YAML text, `...`, and unsupported members retain their source. Valid
properties before and after a failed member survive. Metadata stays outside
Markup children and visitor callbacks. Numbers retain exact decimal strings.
`Image.width` and `Image.height` remain absent until O9.

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

`==highlight==` produces `Mark` with parsed inline `content`, including nested
emphasis, links, and other inline nodes. Matching consumes two equals signs
at a time; unmatched signs remain text. Typed visitors and walking visitors
include the `Mark` case, and its scope covers both delimiters and the body.

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
