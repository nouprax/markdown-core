# Markdown Core

Markdown Core is a cross-platform Markdown parser that exposes the same
immutable abstract syntax tree (AST) in C, Swift, Kotlin, and ECMAScript. The C
engine and every binding live in this repository, so a release gives each
platform the same parser behavior, node model, source locations, and extension
defaults.

The project provides parsing and AST traversal, not Markdown rendering or AST
mutation. It inherits from the cmark and cmark-gfm projects and from the
independently developed fork at
[DongyuZhao/cmark-gfm](https://github.com/DongyuZhao/cmark-gfm). Parts of the
inherited implementation were rewritten before and after this repository was
created. Markdown Core is an independent project and does not plan to merge its
changes back upstream. See [UPSTREAM.md](UPSTREAM.md) for the exact baseline,
divergence history, and license relationship.

## Usage

All platform APIs have one synchronous entry point, and it is the document
itself: `Document(markdown, options)` in Swift, Kotlin, and ECMAScript, and
`markdown_core_document_new` in C. Parsing produces a complete AST. The Swift,
Kotlin, and ECMAScript bindings copy that AST into platform values; the
document keeps the native parse only so that `append` can hold identities
stable across revisions, and releasing it never invalidates a value it
produced.

The default parse options enable smart punctuation, footnotes, HTML comment
stripping, tables, strikethrough, autolinks, task lists, formulas (including
dollar and LaTeX delimiters), directives, cross-links (`[[reference]]`), and
embeds (`![[reference]]`). Each option can be disabled per parse. `TreeDumper`
and `dump()` produce a canonical diagnostic representation for logs, tests,
and debugging; dump text is not a persistence or interchange format.

### Swift

The root Swift package supports iOS 18 and macOS 15 or later and exports the
`MarkdownCore` product and module:

```swift
.package(url: "https://github.com/nouprax/markdown-core", from: "2.0.0")
```

```swift
import MarkdownCore

let document = try Document(
    "# Hello",
    options: ParseOptions(directives: false)
)
print(document.dump())
```

The Swift AST is an immutable, `Sendable` value tree. The module also provides
exhaustive typed visitors and read-only depth-first walking.

### Kotlin Multiplatform

Use the root Maven coordinate from a Kotlin Multiplatform source set:

```kotlin
kotlin {
    sourceSets {
        commonMain.dependencies {
            implementation("com.nouprax:kotlin-markdown-core:2.0.0")
        }
    }
}
```

```kotlin
import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.ParseOptions

val document = Document(
    "# Hello",
    ParseOptions(directives = false),
)
println(document.dump())
```

The published targets are Android (API 21 or later), JVM 17, macOS arm64, and
Linux x64. Android's four-ABI JNI payload is an internal dependency; consumers
do not need a separate C or Prefab package. On JDK 26 or later, JVM applications
should launch with `--enable-native-access=ALL-UNNAMED` to avoid a restricted
native-access warning from the package-private JNI loader.

### ECMAScript and TypeScript

Install the ESM package with your package manager:

```sh
pnpm add @nouprax/es-markdown-core
```

```js
import { Document, MarkupDumper, MarkupWalker } from "@nouprax/es-markdown-core";

const document = Document("# Hello", { directives: false });
new MarkupWalker().walk(document, (event, node, scope) => {
  console.log(event, node.kind, scope.start.line);
});
console.log(MarkupDumper.dump(document));
```

The package supports Node.js 24 or later and browser environments that can load
its WebAssembly asset. Module import completes WebAssembly initialization, so
parsing is synchronous after the import resolves. The generated TypeScript
surface is recursively readonly; JavaScript objects are not runtime-frozen.
Native pointers, WebAssembly memory, and initialization internals are not
exported.

### C and C++

An installed CMake package exports one complete library target containing the
parser and all supported extensions:

```cmake
find_package(markdown-core CONFIG REQUIRED)
target_link_libraries(my-app PRIVATE markdown-core::markdown-core)
```

The installed facade intentionally has no compile-time version macro or
runtime version function. Discover its package version with
`pkg-config --modversion markdown-core`, or request a compatible version in
`find_package(markdown-core <version> CONFIG REQUIRED)`.

Include the read-only facade as `#include <markdown_core.h>`. Pass `NULL` for
parse options to use the defaults, and release every successful parse with
`markdown_core_document_free`. Nodes and string views borrow from their owning
document and must not outlive it. Error objects and allocated dump buffers use
their corresponding `markdown_core_error_free` and `markdown_core_dump_free`
functions.

The library initializes itself on the first parse. Concurrent parsing and
read-only access are safe; callers must ensure that a document is freed only
after all access to that document has finished. The complete C contract is in
[`markdown_core.h`](packages/markdown-core/include/markdown_core.h).

## Streaming

There is no session type. A document is the live head of a CHAIN, and the
chain grows one way: `append` adds bytes at the end — an LLM stream is
`document = document.append(chunk)` per tick, and any byte split is legal,
mid-word or mid-character. One rule: the successor supersedes the receiver,
the revision advances strictly by one on the chain's own counter, and
mutating a superseded handle is a deterministic error, so history is
linear. In one sentence: an append
advances the chain, old heads stop mutating, decoded values live forever. There is
no whole-text edit: replacing the text describes a different document, and
the way to say so is constructing a new one — a new chain with a new
series. Options are fixed for the chain's whole life — changing what the
parser means is a new chain too.

**The stability an application needs is on the TREE.** An id keeps naming the
same node until that node is removed, an unchanged node keeps its exact
revision, and a pure positional shift is not a change — so equality is O(1)
over (id, revision) and an id goes unmodified into a SwiftUI `ForEach(id:)`,
a Compose `key()`, or a React `key`. A node's revision is subtree-covering:
it is the document revision at which the node's own fields, child list, or
any descendant last changed, so a consumer holding values from the previous
document walks the new tree top-down and stops descending wherever the
(id, revision) pair is one it already has. That pair is the entire update
protocol — there is no change list to read. After any sequence of appends
the document is byte-for-byte dump-equal to a from-scratch parse of the
same text.

```swift
let document = try Document("# Hello\n")
let streamed = try document.append("world ")
let another = try streamed.append("and more\n")
```

```kotlin
Document("# Hello\nworld\n").use { document ->
    document.append(" and more\n").use { next -> println(next.root) }
}
```

```js
let document = Document("# Hello\n");
document = document.append("world");   // append supersedes and returns the next head
document.close();
```

```c
markdown_core_string text = {(const uint8_t *)"# Hello\n", 8};
markdown_core_document *document = markdown_core_document_new(text, NULL, NULL);
markdown_core_string chunk = {(const uint8_t *)"world", 5};
markdown_core_document *next = markdown_core_document_append(document, chunk, NULL);
/* The receiver is superseded: reads stay legal until the chain's next
 * mutation begins; free is legal at any time. */
markdown_core_document_free(document);
markdown_core_document_free(next);
```

A mutation is an exclusive operation on its chain — two mutations must be
externally serialized, and between them any number of threads may read the
live head. Documents on different chains never share state, so any number of
chains parse and mutate concurrently. A failed construction supersedes
nothing; a failed `append` ends the chain ("the chain is done": only free
remains, the caller still holds every byte it sent, recovery is a new
chain).

Every document also reports `diagnostics`: everything an editor should
underline, which for Markdown is one thing — a directive's `{...}` attribute
block that did not parse. Every other "wrong" construct is a defined outcome
of the standard semantics rather than a failure, and reporting those would be
reporting Markdown itself.

The language-neutral AST contract is
[`docs/specs/canonical-ast.md`](docs/specs/canonical-ast.md). The adopted
plan for the streaming redesign — `append` as the hot path, documents as
chain heads — is
[`docs/reviews/2026-08-12-streaming-plan.md`](docs/reviews/2026-08-12-streaming-plan.md);
[`docs/specs/incremental-canonical-ast.md`](docs/specs/incremental-canonical-ast.md)
is a frozen earlier design that plan supersedes, and no current public API
implements it.

## Repository layout

- `packages/markdown-core`: C parser, public facade, CLI, extensions, and C tests.
- `packages/swift-markdown-core`: Swift binding, tests, consumer fixture, and benchmarks.
- `packages/kotlin-markdown-core`: Kotlin binding, platform runtimes, tests, and consumer fixtures.
- `packages/es-markdown-core`: ECMAScript/TypeScript package and WebAssembly runtime.
- `specs/canonical-ast`: shared, platform-independent AST conformance fixtures.
- `scripts`: repository build, formatting, lint, audit, and consumer-check entry points.

## Build

Set up or validate the pinned contributor toolchain with
[`docs/development-environment.md`](docs/development-environment.md). The
non-interactive entry points are `scripts/init-environment.sh --check` and
`scripts/init-environment.sh --install`.

Install the pinned JavaScript development dependencies before using the root
`pnpm` tasks:

```sh
pnpm install --frozen-lockfile
```

Build an individual package with its native toolchain:

```sh
# C library and CLI
pnpm build:c

# Swift package
pnpm build:swift

# Kotlin/JVM artifact and its native payload
scripts/gradle.sh :packages:kotlin-markdown-core:jvmJar

# ECMAScript package and WebAssembly module
pnpm --dir packages/es-markdown-core build
```

The C build can also be driven directly:

```sh
cmake --preset default
cmake --build --preset default --parallel
cmake --install build/cmake --prefix /path/to/prefix
```

Its CLI is written to
`build/cmake/packages/markdown-core/core/markdown-core`. The main CMake options
are `MARKDOWN_CORE_SHARED`, `MARKDOWN_CORE_STATIC`, `MARKDOWN_CORE_TESTS`, and
`MARKDOWN_CORE_WARNINGS_AS_ERRORS`.

## Test

Correctness, public-contract conformance, and benchmarks are separate task
families. Run the targets for the platforms available on the current host:

```sh
# C host
pnpm test:c-host
pnpm conformance:c-host
pnpm benchmark:c-host

# Swift on macOS
pnpm test:swift-macos
pnpm conformance:swift-macos
pnpm benchmark:swift-macos

# Kotlin/JVM
pnpm test:kotlin-jvm
pnpm conformance:kotlin-jvm
pnpm benchmark:kotlin-jvm

# ECMAScript
pnpm test:es-node
pnpm test:es-browser
pnpm conformance:es-node
pnpm benchmark:es-node
```

Kotlin also has explicit Android host, Android emulator, macOS arm64, and Linux
x64 targets following the same `test:<platform>` and
`conformance:<platform>` naming. Swift has separate iOS Simulator targets.
There is intentionally no cross-host aggregate: required CI runs every
supported platform target on an appropriate host, simulator, browser, or
device.

Run repository-wide formatting, lint, contract, test-layout, and public-surface
checks with:

```sh
pnpm verify
```

The C presets also provide AddressSanitizer, UndefinedBehaviorSanitizer, and
ThreadSanitizer builds. For example:

```sh
cmake --preset asan
cmake --build --preset asan --parallel
ctest --preset correctness-asan
```

Replace `asan` with `ubsan` or `tsan` and use the matching correctness preset.
Packaging and isolated consumer checks are available through
`pnpm audit:packages` and `pnpm check:kotlin-consumers`; the Swift consumer is
part of `pnpm test:swift-macos`, and the installed C consumer is exercised by
the C test suite.

## Contributing and releasing

Pinned compiler, SDK, runtime, and IDE versions are documented in
[docs/toolchains.md](docs/toolchains.md). Release maintainers must follow
[docs/releasing.md](docs/releasing.md), including the no-secret release dry run,
protected tag/environment approval, Maven signing, npm OIDC, artifact
attestation, and post-publication verification. Release notes start from
[CHANGELOG.md](CHANGELOG.md).

## License

Markdown Core's own work is MIT. The cmark-derived engine it inherits remains
BSD-2-Clause and the bundled CommonMark specification text remains CC-BY-SA
4.0, so the combined work is `MIT AND BSD-2-Clause`. Every upstream copyright
and license notice is preserved in [LICENSE](LICENSE); [COPYING](COPYING) is
the same file under cmark's traditional name. [UPSTREAM.md](UPSTREAM.md)
records the exact baseline this project forked from.
