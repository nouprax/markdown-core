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
import { Document, MarkupDumper, MarkupWalker } from "@nouprax/es-markdown-core";

const document = Document.parse("# Hello", {
  directives: false,
});

console.log(document.content[0].kind);
console.log(document.dump());
console.log(MarkupDumper.dump(document, document.content[0]));
```

All parse options default to `true`: smart punctuation, footnotes, HTML comment
stripping, tables, strikethrough, autolinks, task lists, formulas, dollar and
LaTeX formula delimiters, and directives. Pass only the options you want to
override.

`Document.parse` returns a discriminated `Markup` union with recursively
readonly TypeScript properties. The JavaScript objects are not runtime-frozen.
The package exposes parsing and AST traversal, not rendering or AST mutation.

Every node carries an identity: `id` (a `MarkupID` of the owning session's
`lineage` salt plus a raw value, always the same object for the same identity)
and `revision`, the commit revision at which the node's content last changed.
Two nodes with the same `id` and `revision` are guaranteed to have identical
content, and an unchanged node is the same object across consecutive
snapshots — safe fast paths for render caches and reconciliation keys.

Nodes do not store absolute positions. Resolve them through the snapshot:
`document.scope(node)` returns the node's absolute start/end line and column.

## Traverse and Inspect

Use `MarkupWalker` for a read-only depth-first traversal; every event carries the
resolved scope:

```js
new MarkupWalker().walk(document, (event, node, scope) => {
  console.log(event, node.kind, scope.start.line);
});
```

`document.dump()` and `MarkupDumper.dump(document, node)` emit the canonical
diagnostic tree for the complete document or a focused subtree (subtree scopes
print with the subtree as origin). The text is intended for logs, snapshots,
and debugging rather than persistence or data interchange.

## Incremental Sessions

`MarkupSession` owns one Markdown text and its living AST. Queue edits
(`append` is an edit at end-of-text), then `commit`: the session reparses
incrementally, keeps node identity wherever content is unchanged, and returns
the new immutable snapshot plus the exact delta — the ids that were `added`,
`removed`, `changed`, and `bubbled`. After any sequence of edits and commits
the document is semantically identical to a one-shot `Document.parse` of the
same final text.

Streaming consumers keep the two primitives on their natural cadences:
`append` on every network message (cheap — nothing parses), `commit` on the
render tick. Messages that arrive between ticks conflate into one commit, so
the parse rate follows your display, not the socket:

```js
import { MarkupSession } from "@nouprax/es-markdown-core";

const session = new MarkupSession();
socket.onmessage = ({ data }) => session.append(data);
const ticker = setInterval(() => {
  const commit = session.commit();
  render(commit.document, commit.delta);
}, 100);
```

Snapshots are plain immutable values that share every unchanged node with the
previous snapshot and stay usable after the session advances or closes.
`session.node(id)` answers the committed value for an id, and
`session.footnotes()`, `session.footnote(id)`, and
`session.references(id)` answer footnote numbering, resolution, and
back-reference ordinals as queries against the committed revision.
