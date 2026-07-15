# @nouprax/es-markdown-core

Immutable ECMAScript and TypeScript bindings for Markdown Core, backed by the
same C parser compiled to WebAssembly.

## Install

```sh
pnpm add @nouprax/es-markdown-core
```

The package is ESM-only and supports Node.js 20 or later and browsers that can
load its WebAssembly asset. Importing the module completes WebAssembly
initialization, so `Document.parse` is synchronous.

## Parse Markdown

```js
import { Document, TreeDumper, Walker } from "@nouprax/es-markdown-core";

const document = Document.parse("# Hello", {
  directives: false,
});

console.log(document.content[0].kind);
console.log(document.dump());
console.log(TreeDumper.dump(document.content[0]));
```

All parse options default to `true`: smart punctuation, footnotes, HTML comment
stripping, tables, strikethrough, autolinks, task lists, formulas, dollar and
LaTeX formula delimiters, and directives. Pass only the options you want to
override.

`Document.parse` returns a discriminated `Markup` union with source scopes and
recursively readonly TypeScript properties. The JavaScript objects are not
runtime-frozen. The package exposes parsing and AST traversal, not rendering or
AST mutation.

## Traverse and Inspect

Use `Walker` for a read-only depth-first traversal:

```js
new Walker().walk(document, (event, node) => {
  console.log(event, node.kind, node.scope);
});
```

`TreeDumper.dump(markup)` and each Markup's non-enumerable `dump()` method emit
the canonical diagnostic tree for a complete document or focused subtree. The
text is intended for logs, snapshots, and debugging rather than persistence or
data interchange.
