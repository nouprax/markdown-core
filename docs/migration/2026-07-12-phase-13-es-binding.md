# Phase 13: ECMAScript/TypeScript WASM binding

Phase 13 introduces the publishable `@nouprax/es-markdown-core:1.0.0` package.
It contains ESM JavaScript, recursive readonly TypeScript declarations, and the
same C engine/facade compiled to WebAssembly.

> Phase 18 replaces the package-local expected tree literal with direct,
> module-relative enumeration of the sole root canonical AST manifest. The Node
> conformance target uses only the public npm API and the spec remains excluded
> from the packed npm artifact.

## Initialization and public API

The ESM entry module instantiates `markdown-core.wasm` with top-level await. An
import therefore completes all asynchronous file/network loading before any
export can be used, while `Document.parse(source, options?)` itself remains a
synchronous function returning a copied `Document` value tree. Node loads the
adjacent WASM asset from its file URL; browsers use the same package-relative
URL through `fetch`.

The public runtime exports are `Document`, `ParseError`, `TreeDumper`, `visit`,
`Walker`, and `WalkEvent`, plus the model and visitor declarations with
`Markup.dump()`. No pointer, linear
memory, Emscripten runtime object, or initialization function is exported. The
npm subpath for the WASM file exists only so packaging/bundling tools can
resolve the declared asset.

The maintained source tree is TypeScript and follows the AST rather than
concentrating construction in the entry module. `model/` contains only public
readonly discriminated-union contracts (including `dump()`), `runtime/` owns
WASM loading and parsing,
and `wire/` owns one stateful exhaustive decoder that reads native fields and
directly constructs the public model without a generic intermediate tree.
`walker.ts` owns typed-property traversal. Inline/block pairs such
as `code`/`code-block`, `formula`/`formula-block`, `html`/`html-block`, and
`directive`/`directive-block` are separate modules. The aggregate
`List`/`ListItem` types share `list.ts`, the two footnote roles share
`footnote.ts`, and the typed table Markup nodes share `table.ts`. `index.ts` is a
thin public barrel. JavaScript and recursive readonly declarations are
generated from that single TypeScript source graph and are never maintained by
hand.

## Native boundary

`src/bridge.c` is package-local Emscripten glue and does not enter the portable
C package. It wraps typed facade accessors as primitive WASM calls for node
kind, scope, traversal, scalar fields, and borrowed UTF-8 string views. The ES
module recursively traverses those accessors and copies every field while the
native document is alive, then releases the document. Production code never
calls `markdown_core_document_dump`, never serializes JSON across the boundary,
and never reconstructs a tree from dump text.

One decoder instance owns a single scratch allocation for the complete copy,
validates raw kinds, enums, booleans, counts, string views, table structure, and
directive label boundaries, and requires row/cell Markup nodes to occur on
their legal typed ownership edges. Public model modules do not import the WASM
runtime.

`ParseOptions` preserves the eleven frozen defaults and uses a private bitmask
only at the WASM ABI. `Visitor<Result>` requires every type-specific dispatch
method and has no default or optional handler; the exported exhaustive `visit`
function performs type-safe dispatch without dynamic method-name construction. `Walker` performs
read-only depth-first enter/exit traversal through typed fields. Declarations
make the complete recursive AST readonly, while runtime tests explicitly prove
that neither objects nor arrays are frozen.

## Build and package

`pnpm --dir packages/es-markdown-core build` invokes Emscripten in reactor
mode, statically links the C engine/extensions and the package bridge, and emits
`dist/markdown-core.wasm` plus the deterministic ESM/declaration module tree
compiled by `tsc` from `src/**/*.ts`.
The package export map still exposes only the root entry and the WASM asset;
implementation modules are not public subpath exports. `npm pack` derives and
checks the exact module tree from maintained source files, and an independent
temporary npm consumer installs the packed tarball, imports the root export,
and synchronously parses Markdown. The same installed consumer runs the
TypeScript contract with NodeNext resolution through the package's
`exports.types`; it does not alias directly to a repository `dist/index.d.ts`.

The supported runtime baseline is Node 20 or newer and evergreen browsers with
ES modules, top-level await, `fetch`, and WebAssembly. CI uses the repository's
Node 26.5 policy and a real headless Chrome/Chromium HTTP load.

## Verification

`test:es-node` and `test:es-browser` delegate to package-native correctness
targets covering API, AST behavior, consumer, errors, ownership, robustness,
Unicode, types, packaging, and real-browser loading. `conformance:es-node`
delegates to a separate script that independently walks the copied ES AST and
verifies the public schema mapping. Public TypeScript `TreeDumper` combines the
exhaustive public `visit` dispatch with `Walker` enter/exit events and compares
a package-local focused tree snapshot. Every runtime Markup has a non-enumerable
`dump()` method that delegates to it; the class and declaration are emitted in
`dist` and exported from the package root. The correctness runner supports
`--list` and `--suite <name>` discovery and filtering.

`pnpm benchmark:es-node` is a separate warmup/repeat performance target for
large-document and deep-nesting workloads. It is excluded from correctness and
runs in the scheduled/manual benchmark workflow. The root aggregates and
topology audit include the ES platform targets; correctness CI has a dedicated
Emscripten, Node, and Chrome job.

Local Phase 13 acceptance used Emscripten 6.0.2, Node 26.5.0, pnpm 11.7.0, and
Google Chrome. All Node, browser, type, public-schema conformance,
packed-consumer, exports, and benchmark checks passed.
