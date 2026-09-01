# @nouprax/es-markdown-core

Immutable ECMAScript and TypeScript bindings for Markdown Core, backed by the
same C parser compiled to WebAssembly.

## Install

```sh
pnpm add @nouprax/es-markdown-core
```

The package is ESM-only and supports Node.js 20 or later and browsers that can
load its WebAssembly asset. Importing the module completes WebAssembly
initialization, so parsing is synchronous after the import resolves.

## Parse Markdown

The living `Document` is the one entry into the parser: it is fed text — in
one piece or many — and yields `Read` values, each carrying `semantic`, the
tree. Scopes are counted against the normalized source (NULs to U+FFFD, line
endings to `\n`), which the package does not hand back. The whole-text parse
is a one-chunk stream:

```js
import { Document, TreeDumper } from "@nouprax/es-markdown-core";

const read = new Document("# Hello", { directives: false }).seal();

console.log(read.semantic.content[0].kind);
console.log(read.dump());
console.log(TreeDumper.dump(read.semantic.content[0]));
```

All parse options default to `true`: smart punctuation, footnotes, HTML comment
stripping, tables, strikethrough, autolinks, task lists, formulas (dollar and
LaTeX delimiters included), and directives. Pass only the options you want to
override.

`semantic` is a discriminated `Markup` union with source scopes and recursively
readonly TypeScript properties; the JavaScript objects are not runtime-frozen.
Every node carries `id: Identity` — `{ block, ordinal }`, the name a consumer
tracks the element by across a stream's feeds: the render key. References
carry `definition: Identity`, the identity of the first definition of their
label, and definitions carry `norm`, the match key their label folds to.
The package exposes parsing and AST traversal, not rendering or AST mutation.

## Stream Markdown

Every `feed` returns the read after those bytes — a mid-stream projection
whose incomplete trailing line is not yet in it — and `seal` ends the stream
and releases the native shell, returning the sealed read, identical for the
same bytes however they were fed. A chunk is a string, or a `Uint8Array` of
raw UTF-8 that may end anywhere, mid-character included:

```js
import { Document } from "@nouprax/es-markdown-core";

const document = new Document();
let updated = document.feed("# Str");
updated = document.feed("eamed\n");
console.log(document.seal().dump());
```

Every returned read is a plain value: it stays readable after later feeds and
after the document is gone. A sealed document refuses every later call.
`dispose` releases a stream abandoned before `seal`, is idempotent, and the
class implements `Symbol.dispose`, so `using document = new Document()` scopes
it automatically.

## Traverse and Inspect

Use `Walker` for a read-only depth-first traversal:

```js
new Walker().walk(read.semantic, (event, node) => {
    console.log(event, node.kind, node.scope);
});
```

`TreeDumper.dump(markup)` and each Markup's non-enumerable `dump()` method emit
the canonical diagnostic tree for a complete document or focused subtree. The
text is intended for logs, snapshots, and debugging rather than persistence or
data interchange.
