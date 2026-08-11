# @nouprax/es-markdown-core

Immutable ECMAScript and TypeScript bindings for Markdown Core, backed by the
same C parser compiled to WebAssembly.

## Install

```sh
pnpm add @nouprax/es-markdown-core
```

The package is ESM-only and supports Node.js 24 or later and browsers that can
load its WebAssembly asset. Importing the module completes WebAssembly
initialization, so `Document` is synchronous.

## Parse Markdown

```js
import { Document, MarkupDumper, MarkupWalker } from "@nouprax/es-markdown-core";

const document = Document("# Hello", {
  directives: false,
});

console.log(document.content[0].kind);
console.log(document.dump());
console.log(MarkupDumper.dump(document, document.content[0]));
```

All parse options default to `true`: smart punctuation, footnotes, HTML comment
stripping, tables, strikethrough, autolinks, task lists, formulas, and
directives, cross-links (`[[reference]]`), and embeds (`![[reference]]`). The
`formulas` option controls every formula form, including dollar and LaTeX
delimiters and `formula` fenced code. Pass only the options you want to
override.

`Document` returns a discriminated `Markup` union with recursively readonly
TypeScript properties. The JavaScript objects are not runtime-frozen. The
package exposes parsing, editing, and AST traversal, not rendering or AST
mutation.

Every node carries an identity: `id` (a `MarkupID` of the owning lineage's
salt plus a raw value, always the same object for the same identity) and
`revision`, the revision at which the node's content last changed. Two nodes
with the same `id` and `revision` are guaranteed to have identical content,
and an unchanged node is the same object across consecutive revisions — safe
fast paths for render caches and reconciliation keys.

Nodes do not store absolute positions. Resolve them through the document:
`document.scope(node)` returns the node's absolute start/end line and column.

A document owns a native parse. `document.close()` releases it, and `edit`
hands it to the successor; a document that is neither closed nor edited is
released when it becomes unreachable, but that is a backstop, not the
contract. Everything the document produced — content, scopes, diagnostics,
dump — is a value and stays usable afterwards.

## Traverse and Inspect

Use `MarkupWalker` for a read-only depth-first traversal. The callback overload
emits entering/exiting events with the resolved scope:

```js
new MarkupWalker().walk(document, (event, node, scope) => {
  console.log(event, node.kind, scope.start.line);
});
```

The typed-visitor overload, `walker.walk(document, visitor)`, instead
dispatches each node once in preorder without resolving scopes.

`document.dump()` and `MarkupDumper.dump(document, node)` emit the canonical
diagnostic tree for the complete document or a focused subtree (subtree scopes
print with the subtree as origin). The text is intended for logs, snapshots,
and debugging rather than persistence or data interchange.

## Edit

There is no session type. A document is created from text and options, and
`edit` hands it new text: it returns the document that text describes plus the
`Delta` between the two. Options are fixed for a document's whole lineage —
changing what the parser means is a new document, not an edit.

```js
const document = Document("# Title\n\nHello");
const { document: next, delta } = document.edit("# Title\n\nHello world");
// The heading did not change, so it is the very same object: an unchanged
// node is never re-decoded.
console.log(next.content[0] === document.content[0]); // true
```

`edit` SUPERSEDES the receiver: the native parse moves to the successor, so
the document it was called on must not be edited again. Its already-extracted
values stay valid forever, because they are values.

A `Delta` is one list, in the new document's postorder: every node whose
projection differs appears after all of its own children, and a retired node
appears where it was found, before its former parent's row. Each row says
which parts differ — `value`, `text`, `children`, `descendant` — and a row
whose `parts.retired` is a node that no longer exists. A renderer reconciling
by id reads the list once, front to back:

```js
for (const diff of delta.diffs) {
  const node = next.node(diff.markup);
  if (node === null) retire(diff.markup);
  else refresh(node, diff.parts);
}
```

Streaming consumers edit on the render tick rather than on every socket
message, so the parse rate follows the display and not the socket:

```js
let document = Document("");
let streamed = "";
socket.onmessage = ({ data }) => {
  streamed += data;
};
const ticker = setInterval(() => {
  const commit = document.edit(streamed);
  document = commit.document;
  render(document, commit.delta);
}, 100);
```

`document.diagnostics` lists everything an editor should underline — which,
for Markdown, is one thing: a directive's `{...}` attribute block that did not
parse. Every other "wrong" construct is a defined outcome of the standard
semantics, not a failure.
