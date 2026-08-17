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

All parse options default to `true`: smart punctuation, footnotes, tables,
strikethrough, autolinks, task lists, formulas, directives, cross-links
(`[[reference]]`), and embeds (`![[reference]]`). The
`formulas` option controls every formula form, including dollar and LaTeX
delimiters and `formula` fenced code. Pass only the options you want to
override.

`Document` returns a discriminated `Markup` union with recursively readonly
TypeScript properties. The JavaScript objects are not runtime-frozen. The
package exposes parsing, appending, and AST traversal, not rendering or AST
mutation.

The tree is a value all the way down, so `JSON.stringify` serializes it,
`structuredClone` copies it, and `postMessage` carries it to a worker. Nothing
on a node needs a custom serializer: `MarkupID.series` is 16 hex digits rather
than a `bigint` so that an id survives the trip intact. What comes back is a
new object carrying the same two fields, not the interned one — so hand it to
`document.node(id)` or compare the fields. Reference equality and `Map`
lookups work for the ids a live document handed you, and not for revived ones.

Every node carries an identity: `id` (a `MarkupID` of the owning series
salt plus a raw value, always the same object for the same identity) and
`revision`, the revision at which the node's content last changed. Two nodes
with the same `id` and `revision` are guaranteed to have identical content.

Identity says nothing about POSITION. Every node carries its own `scope` — its
absolute start/end line and column, read straight off the value — but an
append can move a boundary on the hot tail without changing any node's
content, and revisions deliberately do not report that. A consumer that draws
anything positional — gutter numbers, underlines, a scroll anchor, a source
map — must read it from the NEW document's node even for one it skipped as
unchanged.

A document is the live head of a CHAIN, and the whole lifecycle is one
sentence: a mutation advances the chain, old handles die, decoded values live
forever. A chain grows one way: `append` is the one mutation; it returns the
next head and supersedes its receiver, whose only remaining obligation is
`document.close()` — each document on a chain is closed separately, and
closing a superseded one is O(1). Replacing the text is a new document (new
chain, new series). An unclosed document is released when it becomes
unreachable, but that is a backstop, not the contract. Everything a document
produced — content, scopes, diagnostics, dump — is a value and stays usable
forever.

## Traverse and Inspect

Use `MarkupWalker` for a read-only depth-first traversal. The callback overload
emits entering/exiting events with each node's scope:

```js
new MarkupWalker().walk(document, (event, node, scope) => {
  console.log(event, node.kind, scope.start.line);
});
```

The typed-visitor overload, `walker.walk(document, visitor)`, instead
dispatches each node once in preorder, with no exiting event and no scope
argument — `node.scope` is a field.

`document.dump()` and `MarkupDumper.dump(document, node)` emit the canonical
diagnostic tree for the complete document or a focused subtree (subtree scopes
print with the subtree as origin). The text is intended for logs, snapshots,
and debugging rather than persistence or data interchange.

## Append

There is no session type. A document is created from text and options, and a
document chain grows one way: `append` adds text at the end and returns the
document the resulting text describes. Replacing the text is a new document
(new chain, new series). Options are fixed for a chain's whole series —
changing what the parser means is a new document, not a mutation.

```js
import { Document } from "@nouprax/es-markdown-core";

const document = Document("# Title\n\nHello");
const next = document.append(" world");
// The heading did not change, so it compares equal — same id, same revision.
console.log(next.content[0].id === document.content[0].id); // true
console.log(next.content[0].revision === document.content[0].revision); // true
```

A successful append SUPERSEDES its receiver: the old head keeps answering
from its decoded values and still wants its `close()`, but mutating it again
is a deterministic error — history is linear, there is no forking. An
argument failure supersedes nothing; a failure past the argument checks ends
the chain ("the chain is done": only `close` remains, you still hold every
byte you sent, recovery is a new document).

`append` produces the same tree, the same dump, and the same node structure
as a one-shot parse of the concatenated text. Appending saves the DECODE as
well as the parse: appended bytes never move settled content, so after an append the
binding re-decodes only what changed — decode and value construction are
O(changed) per tick, while the JS-side index bookkeeping behind
`document.node(id)` is O(live nodes) of pure map writes. The native append grows the tree in
place on every tick — prose, headings, quotes, lists, fences, HTML blocks,
tables, formula blocks, directives, footnotes and definitions alike; a
definition that arrives re-refines only the units that mention its label,
and a node the append did not reach keeps its id and revision — so the
engine's cost per tick is the chunk, what it closed and the open leaf, not
the document. A node the append did not
reach is the predecessor's very value object — same `id`, same `revision`,
same `scope`, `===`. Any split of the text is legal, mid-word,
mid-marker, mid-line — even between the two halves of a surrogate pair: a
chunk ending in an unpaired high surrogate has that one code unit held back
until the next append completes it, so a split emoji never becomes U+FFFD
and the parsed text trails the appended units by at most one UTF-16 code
unit:

```js
import { Document } from "@nouprax/es-markdown-core";

let document = Document("");
for (const chunk of ["# Str", "eaming\n\nThe tail grows; ", "settled blocks keep their id."]) {
  const previous = document;
  document = document.append(chunk);
  previous.close(); // a superseded handle supports only close
}
console.log(document.content[0].kind); // "heading"
document.close();
```

### What changed is asked of the new tree

There is no change list riding along with an append; `(id, revision)` is the
whole update protocol. The stability a reactive framework needs is on the
TREE: an unchanged node keeps its `id` and its `revision`, `MarkupID` is
interned so `a.id === b.id` is O(1), and `id` goes unmodified into a `key`.
Hand the new document to React and stop.

A node's `revision` covers its whole subtree — it is the revision at which
the node's own fields, child list, or any descendant last changed — so equal
`id` and `revision` proves two subtrees identical without descending. A
consumer holding state it must edit in place rather than re-derive — a
display list, a text-measurement cache, an LSP token array — walks the new
tree top-down, compares each node's pair to the one it kept, and prunes
every subtree the pair proves unchanged:

```js
const reconcile = (kept, node) => {
  if (kept.get(node.id) === node.revision) return; // identical subtree
  kept.set(node.id, node.revision);
  redraw(node);
  for (const child of node.content ?? []) reconcile(kept, child);
};
```

A streaming consumer appends every socket message as it arrives — there is
no self-held accumulated string, and no reason to throttle the parse to the
render:

```js
import { Document } from "@nouprax/es-markdown-core";

let document = Document("");
socket.onmessage = ({ data }) => {
  const previous = document;
  document = document.append(data);
  previous.close();
  scheduleRender(document);
};
```

`document.diagnostics` lists everything an editor should underline — which,
for Markdown, is one thing: a directive's `{...}` attribute block that did not
parse. Every other "wrong" construct is a defined outcome of the standard
semantics, not a failure.
