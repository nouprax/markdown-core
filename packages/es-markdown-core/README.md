# @nouprax/es-markdown-core

Immutable ECMAScript and TypeScript bindings for Markdown Core, backed by the
same C parser compiled to WebAssembly.

```js
import { Document, TreeDumper, Walker } from "@nouprax/es-markdown-core";

const document = Document.parse("# Hello");
new Walker().walk(document, (event, node) => {
  console.log(event, node.kind, node.scope);
});
console.log(document.dump());
console.log(TreeDumper.dump(document.content[0]));
```

WASM initialization completes while the ES module is imported. Consequently,
`Document.parse(source, options?)` is synchronous and is the only parse entry
point. The resulting recursive value tree is not runtime-frozen, but its
TypeScript surface is recursively readonly. Native pointers, memory, and
initialization details are not exported.

`TreeDumper.dump(markup)` and each Markup's non-enumerable `dump()` method emit
the canonical diagnostic tree for a complete document or focused subtree. The
text is intended for logs, snapshots, and debugging rather than persistence or
data interchange.

The maintained implementation is a single TypeScript source graph split by AST
concept under `src/model/`. Those modules contain only public readonly AST
interfaces, including the shared diagnostic method contract. WASM loading and
parsing are isolated in `runtime/`; one stateful,
exhaustive decoder in `wire/` validates the private boundary and constructs the
public discriminated union directly. The package root remains a thin public
barrel. `tsc` generates both ESM JavaScript and readonly declarations; neither
output is maintained by hand. The published module tree is internal except for
the root export and the declared WASM asset subpath.

Strongly coupled aggregate types are co-located: `List`/`ListItem` share
`list.ts`, `FootnoteDefinition`/`FootnoteReference` share `footnote.ts`, and
`Table`/`TableRow`/`TableCell` share `table.ts`.
